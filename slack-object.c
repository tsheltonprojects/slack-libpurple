#include "slack-object.h"

guint slack_object_id_hash(gconstpointer p) {
	const guint *x = p+1;
	return x[0] ^ (x[1] << 1);
}

gboolean slack_object_id_equal(gconstpointer a, gconstpointer b) {
	return !slack_object_id_cmp(a, b);
}

G_DEFINE_ABSTRACT_TYPE(SlackObject, slack_object, G_TYPE_OBJECT);

static void slack_object_finalize(GObject *gobj) {
	SlackObject *obj = SLACK_OBJECT(gobj);

	g_free(obj->name);
	g_free(obj->last_mesg);
	g_free(obj->last_read);
	g_free(obj->last_mark);
	g_free(obj->last_sent);

	g_free(obj->last_thread_timestr);
	g_free(obj->last_thread_ts);
}

static void slack_object_class_init(SlackObjectClass *klass) {
	GObjectClass *gobj = G_OBJECT_CLASS(klass);
	gobj->finalize = slack_object_finalize;
}

static void slack_object_init(SlackObject *self) {
}
