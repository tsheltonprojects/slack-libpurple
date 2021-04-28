#ifndef _PURPLE_SLACK_USER_H
#define _PURPLE_SLACK_USER_H

#include "json.h"
#include "slack-object.h"
#include "slack.h"

/* SlackUser represents both a user object, and an optional im object */
struct _SlackUser {
	SlackObject object;

	char *status;
	char *avatar_hash;
	char *avatar_url;

	/* when there is an open IM channel: */
	slack_object_id im; /* in ims */
};

#define SLACK_TYPE_USER slack_user_get_type()
G_DECLARE_FINAL_TYPE(SlackUser, slack_user, SLACK, USER, SlackObject)

static inline PurpleBuddy *user_buddy(SlackUser *user) {
	return PURPLE_BUDDY(user->object.buddy);
}

/* Initialization */
void slack_users_load(SlackAccount *sa);

SlackUser *slack_user_set(SlackAccount *sa, const char *sid, const char *name);
SlackUser *slack_user_update(SlackAccount *sa, json_value *json);

typedef void SlackUserCallback(SlackAccount *sa, gpointer data, SlackUser *user);

/**
 * Get the SlackUser associated with a user id (in sa->users).
 * If it's not known, look it up.
 * The callback may be made inline or later, possibly with a NULL obj on unknown user or error.
 */
void slack_user_retrieve(SlackAccount *sa, const char *uid, SlackUserCallback *cb, gpointer data);

/* RTM event handlers */
void slack_user_changed(SlackAccount *sa, json_value *json);
void slack_presence_change(SlackAccount *sa, json_value *json);

/* Purple protocol handlers */
void slack_set_info(PurpleConnection *gc, const char *info);
char *slack_status_text(PurpleBuddy *buddy);
void slack_get_info(PurpleConnection *gc, const char *who);

void slack_update_avatar(SlackAccount *sa, SlackUser *user);

#endif // _PURPLE_SLACK_USER_H
