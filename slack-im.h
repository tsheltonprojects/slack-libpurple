#ifndef _PURPLE_SLACK_IM_H
#define _PURPLE_SLACK_IM_H

#include "json.h"
#include "slack.h"
#include "slack-user.h"

/* Initialization */
void slack_presence_sub(SlackAccount *sa);
SlackUser *slack_im_set(SlackAccount *sa, json_value *json, SlackUser *user, gboolean is_open, gboolean update_sub);

/* RTM event handlers */
void slack_im_close(SlackAccount *sa, json_value *json);
void slack_im_open(SlackAccount *sa, json_value *json);

/* Purple protocol handlers */
int slack_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags flags);

#endif // _PURPLE_SLACK_IM_H
