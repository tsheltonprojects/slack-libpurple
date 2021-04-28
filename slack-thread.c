#include "slack-api.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-json.h"
#include "slack-message.h"
#include "slack-thread.h"

#include <debug.h>
#include <time.h>
#include <errno.h>

/**
 * Returns a deterministic color for a thread.
 *
 * Returned string must be g_string_free'd.
 *
 * @param ts "thread_ts" string value from Slack.
 */
static GString *slack_get_thread_color(const char *ts) {
	// Produce a color that works against white background by the following
	// algorithm:
	//
	// 1. Pick a pseudo-random number by seeding it with ts, IOW it is
	// deterministic.
	//
	// 2. Pick a 24-bit RGB value, but throw away the the original highest
	// bit of each byte, resulting in a range from 000000 to 7f7f7f.
	//
	// 3. Pick exactly one base color that will receive its highest bit, or
	// possibly none of them.
	//
	// This gives quite a random color, but often with a dominant RGB
	// component, never white, and deterministic from ts.

	guint r = g_str_hash(ts);

	// Pick random RGB color.
	guint color = r & 0x7f7f7f;

	// Pick random RGB high bit by shifting 0x800000 down by 0 (B), 8 (G),
	// 16 (R), or 24 (0). 0x3000000 are the first bits of 'r' we didn't use
	// yet.
	guint pref_color = (0x800000 >> ((r & 0x3000000) >> 21));
	color |= pref_color;

	GString *tmp = g_string_sized_new(7);
	g_string_printf(tmp, "%06x", color);

	return tmp;
}

void slack_append_formatted_thread_timestamp(GString *str, const char *ts) {
	time_t tt = slack_parse_time_str(ts);
	time_t now = time(NULL);
	struct tm now_time, thread_time;
	localtime_r(&tt, &thread_time);
	localtime_r(&now, &now_time);
	const char *time_fmt;

	GString *color = slack_get_thread_color(ts);

	if (thread_time.tm_yday != now_time.tm_yday || thread_time.tm_year != now_time.tm_year)
		time_fmt = "%x-%X";
	else
		time_fmt = "%X";

	char time_str[100];
	strftime(time_str, sizeof(time_str), time_fmt, &thread_time);

	g_string_append(str, "<font color=\"#");
	g_string_append(str, color->str);
	g_string_append(str, "\">");
	g_string_append(str, time_str);
	g_string_append(str, "</font>");

	g_string_free(color, TRUE);
}

/* it'd be nice to combine these two functions and allow .sub on normal times and larger ranges (%H:%S) */
static gboolean slack_is_slack_ts(const char *str) {
	/* 0000000000.000000 */
	int i = 0;
	while (str[i] >= '0' && str[i] <= '9')
		i ++;
	if (i != 10 || str[i++] != '.')
		return FALSE;
	while (str[i] >= '0' && str[i] <= '9')
		i ++;
	if (i != 17 || str[i])
		return FALSE;
	return TRUE;
}

static time_t slack_get_ts_from_time_str(const char *time_str) {
#ifndef _WIN32
	time_t ts = time(NULL);

	static const char *formats[] = {"%x-%X", "%X", NULL};
	const char **fmt;
	for (fmt = formats; *fmt; fmt++) {
		struct tm tm;
		localtime_r(&ts, &tm);
		char *result = strptime(time_str, *fmt, &tm);
		if (result && !*result)
			return mktime(&tm);
	}
#endif

	return 0;
}

typedef void slack_thread_lookup_ts_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts);

struct thread_lookup_ts {
	SlackObject *conv;
	slack_thread_lookup_ts_cb *cb;
	void *data;
};

static gboolean slack_thread_lookup_ts_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct thread_lookup_ts *lookup = data;
	const char *ts = NULL;

	json_value *list = json_get_prop_type(json, "messages", array);
	if (!list || error) {
		purple_debug_error("slack", "Error querying threads: %s\n", error ?: "missing");
	}
	else if (list->u.array.length <= 0) {
		slack_write_message(sa, lookup->conv, "Thread not found. If the thread start date is not today, make sure you specify the date in the thread timestamp.", PURPLE_MESSAGE_SYSTEM);

	}
	else if (list->u.array.length > 1) {
		GString *errmsg = g_string_new("Thread timestamp is ambiguous. Please use one of the following unambiguous thread IDs:\n");
		for (unsigned i = 0; i < list->u.array.length; i++) {
			json_value *entry = list->u.array.values[i];
			const char *ts = json_get_prop_strptr(entry, "ts");
			if (ts) {
				const char *msg = json_get_prop_strptr(entry, "text");
				GString *color = slack_get_thread_color(ts);
				g_string_append(errmsg, "<font color=\"#");
				g_string_append(errmsg, color->str);
				g_string_append(errmsg, "\">");
				g_string_append(errmsg, ts);
				g_string_append(errmsg, "</font> (\"");
				g_string_append(errmsg, msg ?: "NULL");
				g_string_append(errmsg, "\")\n");
				g_string_free(color, TRUE);
			}
		}
		slack_write_message(sa, lookup->conv, errmsg->str, PURPLE_MESSAGE_SYSTEM);
		g_string_free(errmsg, TRUE);
	}
	else {
		json_value *entry = list->u.array.values[0];
		ts = json_get_prop_strptr(entry, "ts");
	}

	lookup->cb(sa, lookup->conv, lookup->data, ts);
	g_object_unref(lookup->conv);
	g_free(lookup);
	return FALSE;
}

static void slack_thread_lookup_ts(SlackAccount *sa, slack_thread_lookup_ts_cb *cb, SlackObject *conv, gpointer data, const char *timestr) {
	if (slack_is_slack_ts(timestr))
		return cb(sa, conv, data, timestr);

	time_t ts = slack_get_ts_from_time_str(timestr);
	if (ts == 0) {
		slack_write_message(sa, conv, "Could not parse thread timestamp.", PURPLE_MESSAGE_SYSTEM);
		return cb(sa, conv, data, NULL);
	}

	struct thread_lookup_ts *lookup = g_new(struct thread_lookup_ts, 1);
	lookup->conv = g_object_ref(conv);
	lookup->cb = cb;
	lookup->data = data;

	char oldest[20] = "";
	snprintf(oldest, 19, "%ld.000000", ts);
	char latest[20] = "";
	snprintf(latest, 19, "%ld.999999", ts);

	const char *id = slack_conversation_id(conv);
	slack_api_get(sa, slack_thread_lookup_ts_history_cb, lookup, "conversations.history", "channel", id, "oldest", oldest, "latest", latest, NULL);
}

static void slack_thread_post_lookup_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts) {
	char *msg = data;
	if (thread_ts) {
		int r = slack_conversation_send(sa, conv, msg, 0, thread_ts);
		if (r < 0)
			purple_debug_error("slack", "Not able to send message \"%s\": %s\n", msg, strerror(-r));
	}
	g_free(msg);
}

void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr, const char *msg) {
	slack_thread_lookup_ts(sa, slack_thread_post_lookup_cb, obj, g_strdup(msg), timestr);
}

static void slack_thread_get_replies_lookup_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts) {
	if (thread_ts)
		slack_get_history(sa, conv, NULL, SLACK_HISTORY_LIMIT_COUNT, thread_ts, TRUE);
}

void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr) {
	slack_thread_lookup_ts(sa, slack_thread_get_replies_lookup_cb, obj, NULL, timestr);
}
