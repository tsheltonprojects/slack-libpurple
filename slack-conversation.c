#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-channel.h"
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
	slack_api_call(sa, conversations_list_cb, NULL, "conversations.list", "types", "public_channel,private_channel,mpim,im", "exclude_archived", "true", SLACK_PAGINATE_LIMIT, ##ARGS, NULL)

static void conversations_list_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *chans = json_get_prop_type(json, "channels", array);
	if (!chans) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error ?: "Missing conversation list");
		return;
	}

	for (unsigned i = 0; i < chans->u.array.length; i++)
		conversation_update(sa, chans->u.array.values[i]);

	char *cursor = json_get_prop_strptr(json_get_prop(json, "response_metadata"), "next_cursor");
	if (cursor && *cursor)
		CONVERSATIONS_LIST_CALL(sa, "cursor", cursor);
	else
		slack_login_step(sa);
}

static inline void conversations_counts_channels(SlackAccount *sa, json_value *json, const char *prop, SlackChannelType type) {
	json_value *chans = json_get_prop_type(json, prop, array);
	if (!chans)
		return;
	for (unsigned i = 0; i < chans->u.array.length; i++)
		slack_channel_set(sa, chans->u.array.values[i], type);
}

static void conversations_counts_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	if (error) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error);
		return;
	}

	conversations_counts_channels(sa, json, "channels", SLACK_CHANNEL_PUBLIC);
	conversations_counts_channels(sa, json, "groups", SLACK_CHANNEL_GROUP);
	conversations_counts_channels(sa, json, "mpims", SLACK_CHANNEL_MPIM);
	json_value *ims = json_get_prop_type(json, "ims", array);
	for (unsigned i = 0; i < ims->u.array.length; i++) {
		json_value *im = ims->u.array.values[i];
		const char *user_id = json_get_prop_strptr(im, "user_id");
		if (!user_id)
			continue;
		/* hopefully this is the right name? */
		SlackUser *user = slack_user_set(sa, user_id, json_get_prop_strptr(im, "name"));
		slack_im_set(sa, im, user, TRUE, FALSE);
	}
	/* TODO: unread_count */

	slack_login_step(sa);
}

void slack_conversations_load(SlackAccount *sa, gboolean lazy) {
	g_hash_table_remove_all(sa->channels);
	g_hash_table_remove_all(sa->ims);
	if (lazy)
		/* undoumented */
		slack_api_call(sa, conversations_counts_cb, NULL, "users.counts", "mpim_aware", "true", /* "only_relevant_ims", "true", */ NULL);
	else
		CONVERSATIONS_LIST_CALL(sa);
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
	lookup->cb(sa, lookup->data, conversation_update(sa, lookup->json));
	g_free(lookup);
}

static void conversation_retrieve_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct conversation_retrieve *lookup = data;
	json_value *chan = json_get_prop_type(json, "channel", object);
	if (!chan || error) {
		purple_debug_error("slack", "Error retrieving conversation: %s\n", error ?: "missing");
		lookup->cb(sa, lookup->data, NULL);
		g_free(lookup);
		return;
	}
	lookup->json = chan;
	if (json_get_prop_boolean(json, "is_im", FALSE)) {
		/* Make sure we know the user, too */
		const char *uid = json_get_prop_strptr(json, "user");
		if (uid)
			return slack_user_retrieve(sa, uid, conversation_retrieve_user_cb, lookup);
	}
	return conversation_retrieve_user_cb(sa, lookup, NULL);
}

void slack_conversation_retrieve(SlackAccount *sa, const char *sid, SlackConversationCallback *cb, gpointer data) {
	SlackObject *obj = slack_conversation_lookup_sid(sa, sid);
	if (obj)
		return cb(sa, data, obj);
	struct conversation_retrieve *lookup = g_new(struct conversation_retrieve, 1);
	lookup->cb = cb;
	lookup->data = data;
	slack_api_call(sa, conversation_retrieve_cb, lookup, "conversations.info", "channel", sid, NULL);
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
		/* XXX conversations.mark call??? */
		slack_api_channel_call(sa, NULL, NULL, obj, "mark", "ts", obj->last_mark, NULL);
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

static void get_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *obj = data;
	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error loading channel history: %s\n", error ?: "missing");
		g_object_unref(obj);
		return;
	}

	/* what order are these in? */
	for (unsigned i = list->u.array.length; i; i --) {
		json_value *msg = list->u.array.values[i-1];
		if (g_strcmp0(json_get_prop_strptr(msg, "type"), "message"))
			continue;

		slack_handle_message(sa, obj, msg, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED);
	}
	/* TODO: has_more? */

	g_object_unref(obj);
}

void slack_get_history(SlackAccount *sa, SlackObject *conv, const char *since, unsigned count) {
	if (SLACK_IS_CHANNEL(conv)) {
		SlackChannel *chan = (SlackChannel*)conv;
		if (!chan->cid)
			slack_chat_open(sa, chan);
	}
	if (count == 0)
		return;
	const char *id = slack_conversation_id(conv);
	g_return_if_fail(id);

	char count_buf[6] = "";
	snprintf(count_buf, 5, "%u", count);
	slack_api_call(sa, get_history_cb, g_object_ref(conv), "conversations.history", "channel", id, "oldest", since ?: "0", "limit", count_buf, NULL);
}

void slack_get_history_unread(SlackAccount *sa, SlackObject *conv, json_value *json) {
	slack_get_history(sa, conv,
			json_get_prop_strptr(json, "last_read"),
			json_get_prop_val(json, "unread_count", integer, 0));
}

static void get_conversation_unread_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *conv = data;
	json = json_get_prop_type(json, "channel", object);

	if (!json || error) {
		purple_debug_error("slack", "Error getting conversation unread info: %s\n", error ?: "missing");
		g_object_unref(conv);
		return;
	}

	slack_get_history_unread(sa, conv, json);
	g_object_unref(conv);
}

void slack_get_conversation_unread(SlackAccount *sa, SlackObject *conv) {
	const char *id = slack_conversation_id(conv);
	g_return_if_fail(id);
	slack_api_call(sa, get_conversation_unread_cb, g_object_ref(conv), "conversations.info", "channel", id, NULL);
}
