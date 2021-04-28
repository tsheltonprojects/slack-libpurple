#ifndef _PURPLE_SLACK_THREAD_H
#define _PURPLE_SLACK_THREAD_H

#include "slack.h"

void slack_thread_switch_to_channel(SlackAccount *sa, SlackObject *obj);
void slack_thread_switch_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr);
void slack_thread_switch_to_latest(SlackAccount *sa, SlackObject *obj);
void slack_thread_post_to_channel(SlackAccount *sa, SlackObject *obj, const char *msg);
void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr, const char *msg);
void slack_thread_post_to_latest(SlackAccount *sa, SlackObject *obj, const char *msg);
void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr);

#endif // _PURPLE_SLACK_THREAD_H
