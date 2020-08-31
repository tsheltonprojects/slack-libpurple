#ifndef _PURPLE_SLACK_MESSAGE_H
#define _PURPLE_SLACK_MESSAGE_H

#include "json.h"
#include "slack.h"
#include "slack-object.h"

gchar *slack_html_to_message(SlackAccount *sa, const char *s, PurpleMessageFlags flags);
void slack_message_to_html(GString *html, SlackAccount *sa, gchar *s, PurpleMessageFlags *flags, gchar *prepend_newline_str);
void slack_json_to_html(GString *html, SlackAccount *sa, json_value *json, PurpleMessageFlags *flags);
/**
 * Display a pre-formatted string message
 *
 * @param html the HTML formatted message
 * @param flags additional flags for message
 */
void slack_write_message(SlackAccount *sa, SlackObject *obj, const char *html, PurpleMessageFlags flags);
/**
 * Display a JSON message
 *
 * @param json the json message object (should contain "subtype" field)
 * @param flags additional flags for message
 */
void slack_handle_message(SlackAccount *sa, SlackObject *conv, json_value *json, PurpleMessageFlags flags);

/**
 * Returns a deterministic color for a thread.
 *
 * Returned string must be g_string_free'd.
 *
 * @param ts "thread_ts" string value from Slack.
 */
GString *slack_get_thread_color(const char *ts);

/* RTM event handlers */
gboolean slack_message(SlackAccount *sa, json_value *json);
void slack_user_typing(SlackAccount *sa, json_value *json);

/* Purple protocol handlers */
unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state);

#endif // _PURPLE_SLACK_MESSAGE_H
