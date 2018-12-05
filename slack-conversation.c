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
		return (SlackObject*)slack_im_set(sa, json, &json_value_none, FALSE);
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

static void get_latest_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *chan_json = json_get_prop(json, "channel");
	if (!chan_json)
		return;

	SlackObject *conv;
	slack_object_id id;
	const char *sid;

	sid = json_get_prop_strptr(chan_json, "user");
	if (sid) {
		// Direct IM, use user ID.
		slack_object_id_set(id, sid);
		conv = g_hash_table_lookup(sa->users, id);
	} else {
		// MPIM, use conversation ID.
		sid = json_get_prop_strptr(chan_json, "id");
		if (!sid) {
			purple_debug_warning("slack", "slack response did not include 'user' or 'id' while fetching history\n");
			return;
		}
		slack_object_id_set(id, sid);
		conv = g_hash_table_lookup(sa->channels, id);
	}

	if (!conv) {
		purple_debug_warning("slack", "unable to locate user/channel '%s' while fetching history\n", sid);
		return;
	}

	purple_debug_misc("slack", "fetching history for conversation '%s'\n", sid);
	slack_get_history(sa, conv,
			  json_get_prop_strptr(chan_json, "last_read"),
			  json_get_prop_val(chan_json, "unread_count", integer, 0));
}

static gboolean get_queued_unread_messages_cb(gpointer data) {
	SlackAccount *sa = data;

	if (!g_queue_is_empty(sa->fetch_unread_queue)) {
		char *chan_id = g_queue_pop_head(sa->fetch_unread_queue);
		slack_api_call(sa, get_latest_history_cb, NULL, "conversations.info", "channel", chan_id, NULL);
		g_free(chan_id);
	}

	if (!g_queue_is_empty(sa->fetch_unread_queue)) {
		/* Wait and then fetch next. */
		return TRUE;
	} else {
		/* We won't need the queue anymore. */
		g_queue_free(sa->fetch_unread_queue);
		sa->fetch_unread_queue = NULL;
		sa->fetch_unread_timer = 0;
		return FALSE;
	}
}

static void get_unread_messages_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *all_ims[2] = { json_get_prop(json, "ims"), json_get_prop(json, "mpims") };

	if (!sa->fetch_unread_queue) {
		sa->fetch_unread_queue = g_queue_new();
	}

	for (unsigned im_type = 0; im_type < 2; im_type++) {
		json_value *chans = all_ims[im_type];
		for (unsigned i = 0; chans && i < chans->u.array.length; i++) {
			json_value *chan_json = chans->u.array.values[i];
			const char *latest = json_get_prop_strptr(chan_json, "latest");
			const char *last_read = json_get_prop_strptr(chan_json, "last_read");
			if (latest && last_read && strcmp(latest, last_read) == 0) {
				/* Skip API call if we have read everything already. */
				continue;
			}
			const char *chan_id = json_get_prop_strptr(chan_json, "id");
			if (!chan_id) {
				purple_debug_warning("slack", "slack response did not include 'id' while fetching unread messages\n");
				continue;
			}

			g_queue_push_tail(sa->fetch_unread_queue, g_strdup(chan_id));
		}
	}

	if (!g_queue_is_empty(sa->fetch_unread_queue)) {
		sa->fetch_unread_timer = purple_timeout_add_seconds(5, get_queued_unread_messages_cb, sa);
	} else {
		/* We won't need the queue anymore. */
		g_queue_free(sa->fetch_unread_queue);
		sa->fetch_unread_queue = NULL;
	}
}

static void get_unread_messages(SlackAccount *sa) {
	/* Private API, not documented. Found by EionRobb (Github). */
	slack_api_call(sa, get_unread_messages_cb, NULL, "users.counts", "mpim_aware", "true", "only_relevant_ims", "true", "simple_unreads", "true", NULL);
}

void slack_conversations_load(SlackAccount *sa) {
	g_hash_table_remove_all(sa->channels);
	g_hash_table_remove_all(sa->ims);
	CONVERSATIONS_LIST_CALL(sa);
	if (purple_account_get_bool(sa->account, "get_history", FALSE)) {
		get_unread_messages(sa);
	}
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
