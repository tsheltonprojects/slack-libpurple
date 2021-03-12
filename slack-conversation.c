#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-channel.h"
#include "slack-message.h"
#include "slack-im.h"
#include "slack-message.h"
#include "slack-conversation.h"

static SlackObject *conversation_update(SlackAccount *sa, json_value *json) {
	if (json_get_prop_boolean(json, "is_im", FALSE))
		return (SlackObject*)slack_im_set(sa, json, NULL, TRUE, FALSE);
	else
		return (SlackObject*)slack_channel_set(sa, json, SLACK_CHANNEL_UNKNOWN);
}

#define CONVERSATIONS_LIST_CALL(sa, ARGS...) \
	slack_api_get(sa, conversations_list_cb, NULL, "conversations.list", "types", "public_channel,private_channel,mpim,im", "exclude_archived", "true", SLACK_PAGINATE_LIMIT, ##ARGS, NULL)

static gboolean conversations_list_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *chans = json_get_prop_type(json, "channels", array);
	if (!chans) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error ?: "Missing conversation list");
		return FALSE;
	}

	for (unsigned i = 0; i < chans->u.array.length; i++)
		conversation_update(sa, chans->u.array.values[i]);

	char *cursor = json_get_prop_strptr(json_get_prop(json, "response_metadata"), "next_cursor");
	if (cursor && *cursor)
		CONVERSATIONS_LIST_CALL(sa, "cursor", cursor);
	else
		slack_login_step(sa);
	return FALSE;
}

void slack_conversations_load(SlackAccount *sa) {
	g_hash_table_remove_all(sa->channels);
	g_hash_table_remove_all(sa->ims);
	CONVERSATIONS_LIST_CALL(sa);
}

static inline void conversation_counts_check_unread(SlackAccount *sa, SlackObject *conv, json_value *json, gboolean load_history) {
	if (!conv || !load_history)
		return;
	gboolean has_unreads = FALSE;
	if (json_get_prop_val(json, "has_unreads", boolean, FALSE) ||
			json_get_prop_val(json, "unread_count", integer, 0) > 0)
		has_unreads = TRUE;
	if (!has_unreads || json_get_prop_val(json, "is_muted", boolean, FALSE))
		return;
	const char *since = json_get_prop_strptr(json, "last_read");
	if (!since)
		return;
	slack_get_history(sa, conv, since, /* TODO pagination */ 200);
}

static inline void conversation_counts_channels(SlackAccount *sa, json_value *json, const char *prop, SlackChannelType type, gboolean load_history) {
	json_value *chans = json_get_prop_type(json, prop, array);
	if (!chans)
		return;
	for (unsigned i = 0; i < chans->u.array.length; i++) {
		json_value *j = chans->u.array.values[i];
		SlackChannel *chan = slack_channel_set(sa, j, type);
		conversation_counts_check_unread(sa, (SlackObject *)chan, j, load_history);
	}
}

static gboolean conversation_counts_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	if (error) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error);
		return FALSE;
	}

	gboolean load_history = purple_account_get_bool(sa->account, "load_history", FALSE);

	json_value *ims = json_get_prop_type(json, "ims", array);
	for (unsigned i = 0; i < ims->u.array.length; i++) {
		json_value *im = ims->u.array.values[i];
		const char *user_id = json_get_prop_strptr(im, "user_id");
		if (!user_id)
			continue;
		/* hopefully this is the right name? */
		SlackUser *user = slack_user_set(sa, user_id, json_get_prop_strptr(im, "name"));
		slack_im_set(sa, im, user, TRUE, FALSE);
		conversation_counts_check_unread(sa, (SlackObject *)user, im, load_history);
	}

	load_history = load_history && purple_account_get_bool(sa->account, "get_history", FALSE);
	conversation_counts_channels(sa, json, "channels", SLACK_CHANNEL_PUBLIC, load_history);
	conversation_counts_channels(sa, json, "groups", SLACK_CHANNEL_GROUP, load_history);
	conversation_counts_channels(sa, json, "mpims", SLACK_CHANNEL_MPIM, load_history);

	slack_login_step(sa);
	return FALSE;
}

void slack_conversation_counts(SlackAccount *sa) {
	/* Private API, not documented. Found by EionRobb (Github). */
	slack_api_get(sa, conversation_counts_cb, NULL, "users.counts", "mpim_aware", "true", "only_relevant_ims", "true", "simple_unreads", "true", NULL);
}

SlackObject *slack_conversation_get_conversation(SlackAccount *sa, PurpleConversation *conv) {
	switch (conv->type) {
		case PURPLE_CONV_TYPE_IM:
			return g_hash_table_lookup(sa->user_names, purple_conversation_get_name(conv));
		case PURPLE_CONV_TYPE_CHAT:
			return g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv))));
		default:
			return NULL;
	}
}

struct conversation_retrieve {
	SlackConversationCallback *cb;
	gpointer data;
	json_value *json;
};

static void conversation_retrieve_user_cb(SlackAccount *sa, gpointer data, SlackUser *user) {
	struct conversation_retrieve *lookup = data;
	json_value *chan = json_get_prop_type(lookup->json, "channel", object);
	lookup->cb(sa, lookup->data, conversation_update(sa, chan));
	json_value_free(lookup->json);
	g_free(lookup);
}

static gboolean conversation_retrieve_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct conversation_retrieve *lookup = data;
	json_value *chan = json_get_prop_type(json, "channel", object);
	if (!chan || error) {
		purple_debug_error("slack", "Error retrieving conversation: %s\n", error ?: "missing");
		lookup->cb(sa, lookup->data, NULL);
		g_free(lookup);
		return FALSE;
	}
	lookup->json = json;
	if (json_get_prop_boolean(json, "is_im", FALSE)) {
		/* Make sure we know the user, too */
		const char *uid = json_get_prop_strptr(json, "user");
		if (uid) {
			slack_user_retrieve(sa, uid, conversation_retrieve_user_cb, lookup);
			return TRUE;
		}
	}
	conversation_retrieve_user_cb(sa, lookup, NULL);
	return TRUE;
}

void slack_conversation_retrieve(SlackAccount *sa, const char *sid, SlackConversationCallback *cb, gpointer data) {
	SlackObject *obj = slack_conversation_lookup_sid(sa, sid);
	if (obj)
		return cb(sa, data, obj);
	struct conversation_retrieve *lookup = g_new(struct conversation_retrieve, 1);
	lookup->cb = cb;
	lookup->data = data;
	slack_api_get(sa, conversation_retrieve_cb, lookup, "conversations.info", "channel", sid, NULL);
}

static gboolean mark_conversation_timer(gpointer data) {
	SlackAccount *sa = data;
	sa->mark_timer = 0; /* always return FALSE */

	/* we just send them all at once -- maybe would be better to chain? */
	SlackObject *next = sa->mark_list;
	sa->mark_list = MARK_LIST_END;
	while (next != MARK_LIST_END) {
		SlackObject *obj = next;
		next = obj->mark_next;
		obj->mark_next = NULL;
		g_free(obj->last_mark);
		obj->last_mark = g_strdup(obj->last_read);
		slack_api_post(sa, NULL, NULL, "conversations.mark", "channel", slack_conversation_id(obj), "ts", obj->last_mark, NULL);
	}

	return FALSE;
}

void slack_mark_conversation(SlackAccount *sa, PurpleConversation *conv) {
	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj)
		return;

	int c = GPOINTER_TO_INT(purple_conversation_get_data(conv, "unseen-count"));
	if (c != 0)
		/* we could update read count to farther back, but best to only move it forward to latest */
		return;

	if (slack_ts_cmp(obj->last_mesg, obj->last_mark) <= 0)
		return; /* already marked newer */
	g_free(obj->last_read);
	obj->last_read = g_strdup(obj->last_mesg);

	if (obj->mark_next)
		return; /* already on list */

	/* add to list */
	obj->mark_next = sa->mark_list;
	sa->mark_list = obj;

	if (sa->mark_timer)
		return; /* already running */

	/* start */
	sa->mark_timer = purple_timeout_add_seconds(5, mark_conversation_timer, sa);
}

static gboolean get_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *conv = data;
	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error loading channel history: %s\n", error ?: "missing");
	} else {
		/* what order are these in? */
		for (unsigned i = list->u.array.length; i; i --) {
			json_value *msg = list->u.array.values[i-1];
			if (g_strcmp0(json_get_prop_strptr(msg, "type"), "message"))
				continue;

			slack_handle_message(sa, conv, msg, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED);
		}
		/* TODO: pagination has_more? */
	}

	return FALSE;
}

void slack_get_history(SlackAccount *sa, SlackObject *conv, const char *since, unsigned count) {
	purple_debug_misc("slack", "get_history %s %u\n", since, count);

	if (count == 0)
		return;
	if (since && !g_strcmp0(since, "0000000000.000000"))
		/* even though it gives this as a last_read, it doesn't like it in since */
		since = NULL;

	if (SLACK_IS_CHANNEL(conv)) {
		SlackChannel *chan = (SlackChannel*)conv;
		if (!chan->cid) {
			if (purple_account_get_bool(sa->account, "get_history", FALSE)) {
				/* this will call back into get_history */
				slack_chat_open(sa, chan);
			}
			return;
		}
	}
	const char *id = slack_conversation_id(conv);
	if (id == NULL) {
		get_history_cb(sa, conv, NULL, "no conversation ID");
		return;
	}

	char count_buf[6] = "";
	snprintf(count_buf, 5, "%u", MIN(count,500));
	slack_api_get(sa, get_history_cb, conv, "conversations.history", "channel", id, "oldest", since ?: "0", "limit", count_buf, NULL);
}

void slack_get_history_unread(SlackAccount *sa, SlackObject *conv, json_value *json) {
	slack_get_history(sa, conv,
			json_get_prop_strptr(json, "last_read"),
			json_get_prop_val(json, "unread_count", integer, -1));
}

static gboolean get_conversation_unread_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *conv = data;
	json = json_get_prop_type(json, "channel", object);

	if (!json || error) {
		purple_debug_error("slack", "Error getting conversation unread info: %s\n", error ?: "missing");
		g_object_unref(conv);
		return FALSE;
	}

	slack_get_history_unread(sa, conv, json);
	g_object_unref(conv);
	return FALSE;
}

void slack_get_conversation_unread(SlackAccount *sa, SlackObject *conv) {
	const char *id = slack_conversation_id(conv);
	g_return_if_fail(id);
	slack_api_get(sa, get_conversation_unread_cb, g_object_ref(conv), "conversations.info", "channel", id, NULL);
}
