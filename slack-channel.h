#ifndef _PURPLE_SLACK_CHANNEL_H
#define _PURPLE_SLACK_CHANNEL_H

#include "json.h"
#include "slack-object.h"
#include "slack.h"

typedef enum _SlackChannelType {
	SLACK_CHANNEL_UNKNOWN = -1, /* no (new) information about type */
	SLACK_CHANNEL_DELETED = 0, /* any archived/deleted channel (not actually stored) */
	SLACK_CHANNEL_PUBLIC, /* public channel, not member */
	SLACK_CHANNEL_MEMBER, /* public channel, member */
	SLACK_CHANNEL_GROUP,  /* private channel */
	SLACK_CHANNEL_MPIM    /* multiparty IM */
} SlackChannelType;

/* SlackChannel can represent both channels and groups (private channels) */
struct _SlackChannel {
	SlackObject object;

	SlackChannelType type;
	int cid; /* purple chat id, in channel_cids */
};

#define SLACK_TYPE_CHANNEL slack_channel_get_type()
G_DECLARE_FINAL_TYPE(SlackChannel, slack_channel, SLACK, CHANNEL, SlackObject)

PurpleConvChat *slack_channel_get_conversation(SlackAccount *sa, SlackChannel *chan);

static inline PurpleChat *channel_buddy(SlackChannel *chan) {
	return PURPLE_CHAT(chan->object.buddy);
}

/* Initialization */
SlackChannel *slack_channel_set(SlackAccount *sa, json_value *json, SlackChannelType type);

/* Open a purple conversation for a channel */
void slack_chat_open(SlackAccount *sa, SlackChannel *chan);

/* RTM event handlers */
void slack_channel_update(SlackAccount *sa, json_value *json, SlackChannelType event);
void slack_member_joined_channel(SlackAccount *sa, json_value *json, gboolean joined);

int slack_channel_send(SlackAccount *sa, SlackChannel *chan, const char *msg, PurpleMessageFlags flags, const char *thread_ts);

/* Purple protocol handlers */
void slack_join_chat(PurpleConnection *gc, GHashTable *info);
void slack_chat_leave(PurpleConnection *gc, int cid);
int slack_chat_send(PurpleConnection *gc, int cid, const char *msg, PurpleMessageFlags flags);
void slack_chat_invite(PurpleConnection *gc, int cid, const char *message, const char *who);
void slack_set_chat_topic(PurpleConnection *gc, int id, const char *topic);

#endif // _PURPLE_SLACK_USER_H
