#include <errno.h>

#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-rtm.h"
#include "slack-blist.h"
#include "slack-message.h"
#include "slack-user.h"
#include "slack-channel.h"
#include "slack-im.h"

void slack_presence_sub(SlackAccount *sa) {
	GString *ids = g_string_new("[");
	GHashTableIter iter;
	gpointer id;
	SlackUser *user;
	g_hash_table_iter_init(&iter, sa->ims);
	gboolean first = TRUE;
	while (g_hash_table_iter_next(&iter, &id, (gpointer*)&user)) {
		if (!user->object.buddy)
			continue;
		if (first)
			first = FALSE;
		else
			g_string_append_c(ids, ',');
		append_json_string(ids, user->object.id);
	}
	g_string_append_c(ids, ']');

	slack_rtm_send(sa, NULL, NULL, "presence_sub", "ids", ids->str, NULL);
	g_string_free(ids, TRUE);
}

SlackUser *slack_im_set(SlackAccount *sa, json_value *json, SlackUser *user, gboolean is_open, gboolean update_sub) {
	const char *sid = json_get_strptr(json);
	if (sid)
		json = NULL;
	else
		sid = json_get_prop_strptr(json, "id");
	if (!sid)
		return NULL;
	slack_object_id id;
	slack_object_id_set(id, sid);

	if (!user)
		user = g_hash_table_lookup(sa->ims, id);

	is_open = json_get_prop_boolean(json, "is_open", is_open);
	gboolean changed = FALSE;

	const char *user_id = json_get_prop_strptr(json, "user") ?: (user ? user->object.id : NULL);
	g_return_val_if_fail(user_id, user);


	if (!user) {
		user = (SlackUser *)slack_object_hash_table_lookup(sa->users, user_id);
		if (!user) {
			purple_debug_warning("slack", "IM %s for unknown user: %s\n", sid, user_id);
			printf("IM %s for unknown user: %s\n", sid, user_id);
			return user;
		}
	} else
		g_warn_if_fail(slack_object_id_is(user->object.id, user_id));
	if (slack_object_id_cmp(user->im, id)) {
		if (*user->im)
			g_hash_table_remove(sa->ims, user->im);
		slack_object_id_copy(user->im, id);
		g_hash_table_insert(sa->ims, user->im, user);
		changed = TRUE;
	}

	if (is_open) {
		if (!user->object.buddy) {
			user->object.buddy = g_hash_table_lookup(sa->buddies, sid);
			if (user->object.buddy && PURPLE_BLIST_NODE_IS_BUDDY(user->object.buddy)) {
				if (user->object.name && strcmp(user->object.name, purple_buddy_get_name(user_buddy(user)))) {
					purple_blist_rename_buddy(user_buddy(user), user->object.name);
					changed = TRUE;
				}
			} else {
				user->object.buddy = PURPLE_BLIST_NODE(purple_buddy_new(sa->account, user->object.name, NULL));
				slack_blist_cache(sa, user->object.buddy, sid);
				purple_blist_add_buddy(user_buddy(user), NULL, sa->blist, NULL);
				changed = TRUE;
			}
		}

		slack_update_avatar(sa, user);
	} else if (user->object.buddy) {
		slack_blist_uncache(sa, user->object.buddy);
		purple_blist_remove_buddy(user_buddy(user));
		user->object.buddy = NULL;
	}

	purple_debug_misc("slack", "im %s: %s\n", user->im, user->object.id);
	printf("im %s: %s\n", user->im, user->object.id);

	if (changed && update_sub)
		slack_presence_sub(sa);
	return user;
}

void slack_im_close(SlackAccount *sa, json_value *json) {
	slack_im_set(sa, json_get_prop(json, "channel"), NULL, FALSE, TRUE);
}

static void slack_im_open_user(SlackAccount *sa, void *data, SlackUser *user) {
	json_value *json = data;
	slack_im_set(sa, json_get_prop(json, "channel"), user, TRUE, TRUE);
	json_value_free(json);
}

void slack_im_open(SlackAccount *sa, json_value *json) {
	slack_user_retrieve(sa, json_get_prop_strptr(json, "user"), slack_im_open_user, json);
}

struct send_im {
	SlackUser *user;
	char *msg;
	PurpleMessageFlags flags;
	char *thread;
};

static void send_im_free(struct send_im *send) {
	g_object_unref(send->user);
	g_free(send->msg);
	g_free(send->thread);
	g_free(send);
}

static void send_im_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct send_im *send = data;

	if (error)
		purple_conv_present_error(send->user->object.name, sa->account, error);

	if (send->user->object.last_sent)
		g_free(send->user->object.last_sent);
	send->user->object.last_sent = g_strdup(json_get_prop_strptr(json, "ts"));

	send_im_free(send);
}

static gboolean send_im_api_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	send_im_cb(sa, data, json, error);
	return FALSE;
}

static gboolean send_im_open_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct send_im *send = data;

	json = json_get_prop_type(json, "channel", object);
	if (json)
		slack_im_set(sa, json, send->user, TRUE, TRUE);

	if (error || !*send->user->im) {
		purple_conv_present_error(send->user->object.name, sa->account, error ?: "failed to open IM channel");
		send_im_free(send);
		return FALSE;
	}

	char * ts = json_get_prop_strptr(json, "id");


	if (send->thread)
		slack_api_post(sa, send_im_api_cb, send, "chat.postMessage", "channel", send->user->im, "text", send->msg,
				"thread_ts", send->thread, "as_user", "true", NULL);
	else {
		slack_api_post(sa, send_im_api_cb, send, "chat.postMessage", "channel", send->user->im, "text", send->msg,
				"as_user", "true", NULL);

	}
	return FALSE;
}

int slack_im_send(SlackAccount *sa, SlackUser *user, const char *msg, PurpleMessageFlags flags, const char *thread) {
	gchar *m = slack_html_to_message(sa, msg, flags);
	glong mlen = g_utf8_strlen(m, 16384);

	if (mlen > 4000)
		return -E2BIG;

	struct send_im *send = g_new(struct send_im, 1);
	send->user = g_object_ref(user);
	send->msg = m;
	send->flags = flags;
	send->thread = g_strdup(thread);

	if (user->im) {
		slack_api_post(sa, send_im_open_cb, send, "conversations.open", "users", user->object.id, "return_im", "true", NULL);
	} else {
		send_im_open_cb(sa, send, NULL, NULL);
	}

	return 0;
}

int slack_send_im(PurpleConnection *gc, const char *who, const char *msg, PurpleMessageFlags flags) {
	SlackAccount *sa = gc->proto_data;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user)
		return -ENOENT;

	return slack_im_send(sa, user, msg, flags, NULL);
}
