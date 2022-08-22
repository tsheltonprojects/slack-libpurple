#ifndef _PURPLE_SLACK_THREAD_H
#define _PURPLE_SLACK_THREAD_H

#include "slack.h"

struct _SlackCachedThreadTs {
	char *timestr;
	char *thread_ts;
};

#define SLACK_TYPE_CACHED_THREAD_TS slack_cached_thread_ts_get_type()
G_DECLARE_FINAL_TYPE(SlackCachedThreadTs, slack_cached_thread_ts, SLACK, CACHED_THREAD_RS, SlackObject)

/**
 * Appends a formatted ts timestamp to str.
 *
 * @param str String to append to. Will be modified.
 * @param ts Timestamp for format and append.
 */
void slack_append_formatted_thread_timestamp(SlackAccount *sa, GString *str, const char *ts, gboolean exact);
void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr_and_msg);
void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr);

#endif // _PURPLE_SLACK_THREAD_H
