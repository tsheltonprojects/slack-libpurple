#include "slack-api.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-json.h"
#include "slack-message.h"
#include "slack-thread.h"

#include <debug.h>

#include <errno.h>

static gboolean slack_is_slack_ts(const char *str) {
	gboolean found_prefix = FALSE;
	gboolean found_dot = FALSE;
	gboolean found_suffix = FALSE;
	for (int i = 0; str[i] != '\0'; i++) {
		if (str[i] >= '0' && str[i] <= '9') {
			if (found_dot)
				found_suffix = TRUE;
			else
				found_prefix = TRUE;
			continue;
		}
		if (str[i] == '.') {
			if (found_dot)
				return FALSE;
			else
				found_dot = TRUE;
		}
	}
	return found_prefix && found_dot && found_suffix;
}

static time_t slack_get_ts_from_time_str(const char *time_str) {
	struct tm tm;
	char *result;

	time_t ts = time(NULL);
	localtime_r(&ts, &tm);

	// Time only.
	result = strptime(time_str, "%X", &tm);
	if (result == time_str + strlen(time_str))
		return mktime(&tm);

	// Date and time.
	result = strptime(time_str, "%x-%X", &tm);
	if (result == time_str + strlen(time_str))
		return mktime(&tm);

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
		slack_get_thread_replies(sa, conv, thread_ts);
}

void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr) {
	slack_thread_lookup_ts(sa, slack_thread_get_replies_lookup_cb, obj, NULL, timestr);
}
