#ifndef _PURPLE_SLACK_THREAD_H
#define _PURPLE_SLACK_THREAD_H

#include "slack.h"

struct _SlackThread {
	SlackObject object;

	char *thread_ts; /* Currently selected thread. */
};

#define SLACK_TYPE_THREAD slack_thread_get_type()
G_DECLARE_FINAL_TYPE(SlackThread, slack_thread, SLACK, THREAD, SlackObject)

void slack_thread_switch_to_channel(SlackAccount *sa, SlackObject *obj);
void slack_thread_switch_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr);
void slack_thread_switch_to_latest(SlackAccount *sa, SlackObject *obj);
void slack_thread_post_to_channel(SlackAccount *sa, SlackObject *obj, const char *msg);
void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr, const char *msg);
void slack_thread_post_to_latest(SlackAccount *sa, SlackObject *obj, const char *msg);

#endif // _PURPLE_SLACK_THREAD_H
