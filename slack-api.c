#include <debug.h>

#include "slack-api.h"
#include "slack-json.h"
#include "slack-channel.h"
#include "slack-user.h"

PurpleConnectionError slack_api_connection_error(const gchar *error) {
	if (!g_strcmp0(error, "not_authed"))
		return PURPLE_CONNECTION_ERROR_INVALID_USERNAME;
	if (!g_strcmp0(error, "invalid_auth") ||
			!g_strcmp0(error, "account_inactive"))
		return PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED;
	return PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
}

struct _SlackAPICall {
	SlackAccount *sa;
	char *url;
	char *request;
	PurpleUtilFetchUrlData *fetch;
	SlackAPICallback *callback;
	gpointer data;

	SlackAPICall **prev, *next;
};

typedef struct _SlackUrlAndRequest {
	GString *url;
	GString *request;
} SlackUrlAndRequest;

static void api_error(SlackAPICall *call, const char *error) {
	if ((*call->prev = call->next))
		call->next->prev = call->prev;
	if (call->callback)
		call->callback(call->sa, call->data, NULL, error);
	g_free(call->request);
	g_free(call->url);
	g_free(call);
};

static gboolean api_retry(gpointer data);

static void api_cb(G_GNUC_UNUSED PurpleUtilFetchUrlData *fetch, gpointer data, const gchar *buf, gsize len, const gchar *error) {
	SlackAPICall *call = data;

	purple_debug_misc("slack", "api response: %s\n", error ?: buf);
	if (error) {
		api_error(call, error);
		return;
	}

	json_value *json = json_parse(buf, len);
	if (!json) {
		api_error(call, "Invalid JSON response");
		return;
	}

	if (!json_get_prop_boolean(json, "ok", FALSE)) {
		const char *err = json_get_prop_strptr(json, "error");
		if (!g_strcmp0(err, "ratelimited")) {
			/* #27: correct thing to do on 429 status is parse the "Retry-After" header and wait that many seconds,
			 * but getting access to the headers here requires more work, so we just heuristically make up a number... */
			purple_timeout_add_seconds(purple_account_get_int(call->sa->account, "ratelimit_delay", 15), api_retry, call);
			json_value_free(json);
			return;
		}
		api_error(call, err ?: "Unknown error");
		json_value_free(json);
		return;
	}

	if ((*call->prev = call->next))
		call->next->prev = call->prev;
	if (call->callback)
		if (call->callback(call->sa, call->data, json, NULL))
			json = NULL;

	if (json)
		json_value_free(json);
	g_free(call->request);
	g_free(call->url);
	g_free(call);
}

static gboolean api_retry(gpointer data) {
	SlackAPICall *call = data;
	call->fetch = purple_util_fetch_url_request_len_with_account(call->sa->account,
			call->url, FALSE, NULL, TRUE, call->request, FALSE, 4096*1024,
			api_cb, call);
	return FALSE;
}

static GString *slack_api_encode_url(SlackAccount *sa, const char *pfx, const char *endpoint, va_list qargs) {
	GString *url = g_string_new(NULL);
	g_string_printf(url, "%s/%s%s?token=%s", sa->api_url, pfx, endpoint, sa->token);

	const char *param;
	while ((param = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		g_string_append_printf(url, "&%s=%s", param, purple_url_encode(val));
	}

	return url;
}

static GString *slack_api_escape_quotes(const char *str) {
	GString *escaped = g_string_new(NULL);
	for (size_t i = 0; str[i] != '\0'; i++) {
		switch (str[i]) {
		case '"':
			g_string_append(escaped, "\\\"");
			break;
		case '\\':
			g_string_append(escaped, "\\\\");
			break;
		default:
			g_string_append_c(escaped, str[i]);
			break;
		}
	}
	return escaped;
}

static SlackUrlAndRequest slack_api_encode_url_and_post_request(SlackAccount *sa, const char *endpoint, va_list qargs) {
	GString *url = g_string_new(NULL);
	g_string_printf(url, "%s/%s", sa->api_url, endpoint);

	GString *postdata;
	const char *param;
	const char *sep = "";

	postdata = g_string_new("{");
	while ((param = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		GString *escaped = slack_api_escape_quotes(val);
		g_string_append_printf(postdata, "%s\"%s\":\"%s\"", sep, param, escaped->str);
		g_string_free(escaped, TRUE);
		sep = ",";
	}

	g_string_append_c(postdata, '}');

	GString *request;
	gchar *host = NULL, *path = NULL, *user = NULL, *password = NULL;
	int port;
	purple_url_parse(url->str, &host, &port, &path, &user, &password);

	request = g_string_new(NULL);

	g_string_append_printf(request, "POST /%s HTTP/1.0\r\n", path);
	g_string_append(request, "Connection: close\r\n");
	g_string_append_printf(request, "Host: %s\r\n", host);
	g_string_append(request, "Accept: */*\r\n");
	g_string_append_printf(request, "Authorization: Bearer %s\r\n", sa->token);
	g_string_append(request, "Content-Type: application/json\r\n");

	g_string_append_printf(request, "Content-Length: %" G_GSIZE_FORMAT "\r\n", strlen(postdata->str));
	g_string_append(request, "\r\n");

	g_string_append(request, postdata->str);

	g_free(host);
	g_free(path);
	g_free(user);
	g_free(password);
	g_string_free(postdata, TRUE);

	SlackUrlAndRequest ret = {url, request};
	return ret;
}

static void slack_api_call_url(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const char *url, const char *request) {
	SlackAPICall *call = g_new0(SlackAPICall, 1);
	call->sa = sa;
	call->callback = callback;
	call->url = g_strdup(url);
	if (request)
		call->request = g_strdup(request);
	else
		call->request = NULL;
	call->data = user_data;
	if ((call->next = sa->api_calls))
		call->next->prev = &call->next;
	call->prev = &sa->api_calls;
	sa->api_calls = call;

	purple_debug_misc("slack", "api call: %s\nRequest: %s\n", url, request ?: "Default");
	api_retry(call);
}

void slack_api_get(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const char *endpoint, ...)
{
	va_list qargs;
	va_start(qargs, endpoint);
	GString *url = slack_api_encode_url(sa, "", endpoint, qargs);
	va_end(qargs);

	slack_api_call_url(sa, callback, user_data, url->str, NULL);

	g_string_free(url, TRUE);
}

void slack_api_post(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const gchar *endpoint, ...)
{
	va_list qargs;
	va_start(qargs, endpoint);
	SlackUrlAndRequest url_and_request = slack_api_encode_url_and_post_request(sa, endpoint, qargs);
	va_end(qargs);

	slack_api_call_url(sa, callback, user_data, url_and_request.url->str, url_and_request.request->str);

	g_string_free(url_and_request.url, TRUE);
	g_string_free(url_and_request.request, TRUE);
}

gboolean slack_api_channel_get(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, SlackObject *obj, const char *endpoint, ...) {
	g_return_val_if_fail(obj, FALSE);
	const char *type = NULL, *id = NULL;
	if (SLACK_IS_CHANNEL(obj)) {
		SlackChannel *chan = (SlackChannel*)obj;
		switch (chan->type) {
			case SLACK_CHANNEL_MEMBER:
				type = "channels.";
				break;
			case SLACK_CHANNEL_GROUP:
				type = "groups.";
				break;
			case SLACK_CHANNEL_MPIM:
				type = "mpim.";
				break;
			default:
				break;
		}
		id = chan->object.id;
	} else if (SLACK_IS_USER(obj)) {
		SlackUser *user = (SlackUser*)obj;
		if (*user->im) {
			type = "im.";
			id = user->im;
		}
	}

	if (!type || !id)
		return FALSE;

	va_list qargs;
	va_start(qargs, endpoint);
	GString *url = slack_api_encode_url(sa, type, endpoint, qargs);
	va_end(qargs);
	g_string_append_printf(url, "&channel=%s", purple_url_encode(id));

	slack_api_call_url(sa, callback, user_data, url->str, NULL);
	g_string_free(url, TRUE);
	return TRUE;
}

void slack_api_disconnect(SlackAccount *sa) {
	while (sa->api_calls) {
		purple_util_fetch_url_cancel(sa->api_calls->fetch);
		api_error(sa->api_calls, "disconnected");
	}
}
