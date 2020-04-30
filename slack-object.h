#ifndef _PURPLE_SLACK_OBJECT_H
#define _PURPLE_SLACK_OBJECT_H

#include <string.h>

#include <blist.h>
#include <glib-object.h>
#include "glibcompat.h"

/* object IDs seem to always be of the form "TXXXXXXXX" where T is a type identifier and X are [0-9A-Z] (base32?) */
#define SLACK_OBJECT_ID_SIZ	12
/* These may be safely treated as strings (always NULL terminated and padded),
 * but strings are not valid slack_object_ids */
typedef char slack_object_id[SLACK_OBJECT_ID_SIZ];

#define slack_object_id_copy(dst, src) \
	memcpy(dst, src, SLACK_OBJECT_ID_SIZ)
#define slack_object_id_cmp(a, b) \
	memcmp(a, b, SLACK_OBJECT_ID_SIZ-1)
#define slack_object_id_clear(id) \
	memset(id, 0, SLACK_OBJECT_ID_SIZ)

static inline void slack_object_id_set(slack_object_id id, const char *s) {
	if (s) {
		strncpy((char *)id, s, SLACK_OBJECT_ID_SIZ-1);
		id[SLACK_OBJECT_ID_SIZ-1] = 0;
		g_warn_if_fail(s[SLACK_OBJECT_ID_SIZ-1] == 0);
	} else
		slack_object_id_clear(id);
}

static inline gboolean slack_object_id_is(const slack_object_id id, const char *s) {
	return s ? !strncmp(id, s, SLACK_OBJECT_ID_SIZ-1) : !*id;
}

guint slack_object_id_hash(gconstpointer id);
gboolean slack_object_id_equal(gconstpointer a, gconstpointer b);

struct _SlackObject {
	GObject parent;

	slack_object_id id;

	char *name;
	PurpleBlistNode *buddy;

	char *last_mesg, *last_read, *last_mark; /* ts marking */
	struct _SlackObject *mark_next; /* on mark_list if non-null */
};

#define SLACK_TYPE_OBJECT slack_object_get_type()
G_DECLARE_FINAL_TYPE(SlackObject, slack_object, SLACK, OBJECT, GObject)

#define slack_object_hash_table_new() \
	g_hash_table_new_full(slack_object_id_hash, slack_object_id_equal, NULL, g_object_unref)

static inline void slack_object_hash_table_replace(GHashTable *hash_table, SlackObject *obj) {
	g_hash_table_replace(hash_table, obj->id, obj);
}

static inline SlackObject *slack_object_hash_table_lookup(GHashTable *hash_table, const char *sid) {
	if (!sid)
		return NULL;
	slack_object_id id;
	slack_object_id_set(id, sid);
	return g_hash_table_lookup(hash_table, id);
}

static inline gboolean slack_object_hash_table_remove(GHashTable *hash_table, const char *sid) {
	slack_object_id id;
	slack_object_id_set(id, sid);
	return g_hash_table_remove(hash_table, id);
}

#endif
