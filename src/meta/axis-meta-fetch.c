/*
 * axis-meta-fetch.c - asking providers: their requests, their threads, and
 * questions asked once however many wait for the answer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-private.h"

#include <string.h>

#include "axis-version.h"
#include "core/axis-http.h"
#include "library/axis-library-name.h"

/* The fixed user agent: Axis and its version, nothing about the user. */
#define USER_AGENT "Axis/" AXIS_VERSION

/* ── Keys ───────────────────────────────────────────────────────── */

void
axis_meta_secret_free(gchar *secret)
{
	if (secret == NULL)
		return;
	memset(secret, 0, strlen(secret));
	g_free(secret);
}

gchar *
axis_meta_provider_key(
	AxisMeta                    *self,
	const AxisMetaProviderEntry *provider
){
	gchar *key;

	if (self->settings == NULL || provider->key_setting == NULL ||
	    axis_settings_lookup(self->settings, provider->key_setting, NULL) == NULL)
		return NULL;
	key = axis_settings_get_string(self->settings, provider->key_setting);
	if (key != NULL && key[0] == '\0') {
		axis_meta_secret_free(key);
		return NULL;
	}
	return key;
}

gchar *
axis_meta_fetch_get_setting(
	AxisMetaFetch *fetch,
	const gchar   *key
){
	const AxisSettingInfo *info;
	AxisSettings *settings;

	g_return_val_if_fail(fetch != NULL && key != NULL, NULL);

	settings = fetch->meta->settings;
	if (settings == NULL || (info = axis_settings_lookup(settings, key, NULL)) == NULL)
		return NULL;
	if (info->type == AXIS_SETTING_BOOLEAN)
		return g_strdup(axis_settings_get_boolean(settings, key) ? "true" : "false");
	if (info->type != AXIS_SETTING_STRING && info->type != AXIS_SETTING_CHOICE)
		return NULL;
	return axis_settings_get_string(settings, key);
}

/* The rate setting, applied to the limiter before each request. */
static void
apply_rate(
	AxisMeta                    *self,
	const AxisMetaProviderEntry *provider
){
	gdouble rate = provider->info->default_rate;

	if (self->settings != NULL && provider->rate_setting != NULL &&
	    axis_settings_lookup(self->settings, provider->rate_setting, NULL) != NULL)
		rate = axis_settings_get_decimal(self->settings, provider->rate_setting);
	axis_meta_limiter_set_rate(provider->limiter, rate, provider->info->burst);
}

/* ── One request ────────────────────────────────────────────────── */

/* Why @provider is not asked before the real time @until. */
static void
set_held_error(
	const AxisMetaProviderEntry *provider,
	gint64                       until,
	gboolean                     paused,
	GError                     **error
){
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(until / G_USEC_PER_SEC);
	g_autofree gchar *text = when != NULL ? g_date_time_format(when, "%H:%M:%S UTC") : g_strdup("later");

	if (paused)
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_RATE_LIMITED,
			"%s asked Axis to wait, so it is not asked again until %s", provider->info->label, text);
	else
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE,
			"%s did not answer the last times it was asked, so it is not asked again until %s",
			provider->info->label, text);
}

/* Whether @provider is held (paused or backing off) beyond @deadline_real;
 * if so, *@error says why. */
static gboolean
held_beyond(
	const AxisMetaProviderEntry *provider,
	gint64                       deadline_real,
	GError                     **error
){
	AxisMetaLimitStatus status;
	gint64 until;
	gboolean held;

	memset(&status, 0, sizeof(status));
	axis_meta_limiter_get_status(provider->limiter, &status);
	until = MAX(status.paused_until_us, status.backoff_until_us);
	held = until > deadline_real;
	if (held)
		set_held_error(provider, until, status.paused_until_us >= status.backoff_until_us, error);
	axis_meta_limit_status_clear(&status);
	return held;
}

/* Waits for the provider's budget, as long as @fetch may. */
static gboolean
take_token(
	AxisMetaFetch  *fetch,
	GError        **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	gint64 until = 0;

	apply_rate(fetch->meta, fetch->provider);
	switch (axis_meta_limiter_acquire(fetch->provider->limiter, fetch->deadline_us, fetch->cancellable, &until)) {
	case AXIS_META_LIMIT_OK:
		return TRUE;
	case AXIS_META_LIMIT_CANCELLED:
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "cancelled");
		return FALSE;
	case AXIS_META_LIMIT_PAUSED:
		if (!held_beyond(fetch->provider, until - 1, error))
			set_held_error(fetch->provider, until, TRUE, error);
		return FALSE;
	default:
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_RATE_LIMITED,
			"%s's request limit leaves no room for this in time", info->label);
		return FALSE;
	}
}

/* What a failed request means for the provider and the caller. */
static void
report_failure(
	AxisMetaFetch          *fetch,
	const AxisHttpResponse *response,
	const GError           *local,
	GError                **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	AxisMetaLimiter *limiter = fetch->provider->limiter;
	guint status = response->status;

	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CANCELLED)) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "cancelled");
		return;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS)) {
		if (status == 404) {
			axis_meta_limiter_answered(limiter);
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "%s has no such title", info->label);
			return;
		}
		if (status == 429 || (status == 503 && response->retry_after_s >= 0)) {
			gint64 seconds = response->retry_after_s;
			g_autofree gchar *message = seconds >= 0 ?
				g_strdup_printf("%s answered %u and asked Axis to wait %" G_GINT64_FORMAT " s", info->label, status,
					seconds) :
				g_strdup_printf("%s answered %u (too many requests)", info->label, status);

			axis_meta_limiter_pause(limiter, seconds, message);
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_RATE_LIMITED, message);
			return;
		}
		if (status >= 400 && status < 500 && status != 401 && status != 403) {
			/* It answered; the question was one it does not take. */
			axis_meta_limiter_answered(limiter);
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE, local->message);
			return;
		}
	}
	axis_meta_limiter_failed(limiter, local->message);
	g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE, local->message);
}

/* The connection to @fetch's provider: its API origin (or its image origin,
 * which gets no credential), with @auth in its key header and @secret blanked
 * out of every message. */
static AxisHttpConfig
config_for(
	AxisMetaFetch  *fetch,
	gboolean        image,
	const gchar    *auth,
	const gchar    *secret
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	AxisHttpConfig config;

	memset(&config, 0, sizeof(config));
	config.origin = image ? &fetch->provider->image : &fetch->provider->api;
	config.auth_header = image || auth == NULL ? NULL : info->key_header;
	config.auth_value = image ? NULL : auth;
	config.token = secret;
	config.timeout_ms = fetch->meta->tuning.request_timeout_ms != 0 ? fetch->meta->tuning.request_timeout_ms :
		AXIS_META_REQUEST_TIMEOUT_MS;
	config.max_bytes = image ? AXIS_META_ART_MAX_BYTES : AXIS_META_MAX_ANSWER_BYTES;
	config.cancellable = fetch->cancellable;
	config.peer = info->label;
	config.secret_name = "key";
	config.user_agent = USER_AGENT;
	config.accept = image ? "image/jpeg, image/png, image/webp" : "application/json";
	config.traffic = AXIS_OUTBOUND_TRAFFIC_METADATA;
	return config;
}

/* One GET within the budget, with @secret (the key or a token) in the key
 * header after the provider's prefix (none for an image).  With @quiet_401,
 * a 401 is not counted as a failure (the caller logs in again and asks once
 * more); *@status is the answer's status. */
static gboolean
send_one(
	AxisMetaFetch  *fetch,
	gboolean        image,
	const gchar    *path,
	const gchar    *secret,
	gboolean        quiet_401,
	GBytes        **body,
	gchar         **content_type,
	guint          *status,
	GError        **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GError) local = NULL;
	gchar *auth = NULL;
	gboolean ok;

	*body = NULL;
	*status = 0;
	if (content_type != NULL)
		*content_type = NULL;
	if (!take_token(fetch, error))
		return FALSE;
	if (!image && secret != NULL)
		auth = g_strconcat(info->key_prefix != NULL ? info->key_prefix : "", secret, NULL);
	config = config_for(fetch, image, auth, secret);
	ok = axis_http_get_full(&config, path, NULL, body, &response, &local);
	axis_meta_secret_free(auth);
	*status = response.status;
	if (ok) {
		axis_meta_limiter_answered(fetch->provider->limiter);
		if (content_type != NULL)
			*content_type = g_steal_pointer(&response.content_type);
		axis_http_response_clear(&response);
		return TRUE;
	}
	if (image && g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_TOO_LARGE)) {
		/* The image host answered; the image is not one Axis keeps. */
		axis_meta_limiter_answered(fetch->provider->limiter);
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED, "it is larger than the limit");
		axis_http_response_clear(&response);
		return FALSE;
	}
	if (quiet_401 && response.status == 401 && g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS)) {
		axis_meta_limiter_answered(fetch->provider->limiter);
		g_propagate_error(error, g_steal_pointer(&local));
		axis_http_response_clear(&response);
		return FALSE;
	}
	report_failure(fetch, &response, local, error);
	axis_http_response_clear(&response);
	return FALSE;
}

/* ── Logins ─────────────────────────────────────────────────────── */

/* A random value of this process, so that the fingerprint of a key held in
 * memory is no plain hash of it. */
static const gchar *
fingerprint_salt(void)
{
	static gsize once = 0;
	static gchar salt[33];

	if (g_once_init_enter(&once)) {
		guint i;

		for (i = 0; i < 32; i += 8)
			g_snprintf(salt + i, 9, "%08x", (guint)g_random_int());
		g_once_init_leave(&once, 1);
	}
	return salt;
}

/* What tells one key and PIN from another without keeping them. */
static gchar *
fingerprint(
	const gchar *key,
	const gchar *pin
){
	g_autofree gchar *both = g_strconcat(key, "\n", pin != NULL ? pin : "", NULL);
	gchar *print = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)fingerprint_salt(), 32, both, -1);

	memset(both, 0, strlen(both));
	return print;
}

/* The provider's PIN, or %NULL; free with axis_meta_secret_free(). */
static gchar *
provider_pin(
	AxisMeta                    *self,
	const AxisMetaProviderEntry *provider
){
	gchar *pin;

	if (self->settings == NULL || provider->pin_setting == NULL ||
	    axis_settings_lookup(self->settings, provider->pin_setting, NULL) == NULL)
		return NULL;
	pin = axis_settings_get_string(self->settings, provider->pin_setting);
	if (pin != NULL && pin[0] == '\0') {
		axis_meta_secret_free(pin);
		return NULL;
	}
	return pin;
}

static void
set_refused_error(
	const AxisMetaProviderInfo *info,
	GError                    **error
){
	g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE,
		"%s refused the key: nothing is sent to it until the key or the PIN is changed in Settings", info->label);
}

/* A token that may be sent: printable ASCII, no space, within its limit. */
static gboolean
token_usable(const gchar *token)
{
	const gchar *p;

	if (token == NULL || token[0] == '\0' || strlen(token) > AXIS_META_TOKEN_MAX_BYTES)
		return FALSE;
	for (p = token; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

static gint64
renew_us(AxisMeta *self)
{
	return (gint64)(self->tuning.token_renew_s != 0 ? self->tuning.token_renew_s : AXIS_META_TOKEN_RENEW_S) *
		G_USEC_PER_SEC;
}

/* With token_lock held: a copy of the token held for @print, unless it is
 * old or it is @stale (a token just refused). */
static gchar *
held_token(
	AxisMetaFetch *fetch,
	const gchar   *print,
	const gchar   *stale
){
	AxisMetaProviderEntry *provider = fetch->provider;

	if (provider->token == NULL || g_strcmp0(provider->token_for, print) != 0 ||
	    g_get_monotonic_time() - provider->token_us > renew_us(fetch->meta) ||
	    (stale != NULL && strcmp(provider->token, stale) == 0))
		return NULL;
	return g_strdup(provider->token);
}

/* With token_lock held. */
static void
drop_token(AxisMetaProviderEntry *provider)
{
	axis_meta_secret_free(provider->token);
	provider->token = NULL;
	g_clear_pointer(&provider->token_for, g_free);
	provider->token_us = 0;
}

void
axis_meta_fetch_login_status(
	AxisMeta               *self,
	AxisMetaProviderEntry  *provider,
	AxisMetaProviderStatus *status
){
	g_autofree gchar *print = NULL;
	gchar *key;
	gchar *pin;

	status->logs_in = provider->info->login != NULL;
	if (!status->logs_in && provider->info->test == NULL)
		return;
	key = axis_meta_provider_key(self, provider);
	pin = provider_pin(self, provider);
	if (key != NULL)
		print = fingerprint(key, pin);
	axis_meta_secret_free(pin);
	axis_meta_secret_free(key);
	g_mutex_lock(&provider->token_lock);
	status->key_refused = print != NULL && g_strcmp0(provider->refused, print) == 0;
	status->has_token = provider->token != NULL && print != NULL && g_strcmp0(provider->token_for, print) == 0;
	status->test_text = g_strdup(provider->test_text);
	status->test_ok = provider->test_ok;
	status->test_us = provider->test_us;
	g_mutex_unlock(&provider->token_lock);
}

void
axis_meta_fetch_clear_provider(AxisMetaProviderEntry *provider)
{
	drop_token(provider);
	g_clear_pointer(&provider->refused, g_free);
	g_clear_pointer(&provider->test_text, g_free);
	g_mutex_clear(&provider->token_lock);
	g_cond_clear(&provider->login_done);
}

/* How long a login may take, whoever waits for it: one request's deadline
 * and a second for the rest. */
static gint64
login_wait_us(AxisMeta *self)
{
	guint request = self->tuning.request_timeout_ms != 0 ? self->tuning.request_timeout_ms :
		AXIS_META_REQUEST_TIMEOUT_MS;

	return ((gint64)request + 1000) * 1000;
}

/* The provider's login, now, with @key and @pin. */
static gboolean
log_in(
	AxisMetaFetch  *fetch,
	const gchar    *key,
	const gchar    *pin,
	gchar         **token,
	GError        **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	g_autoptr(GError) local = NULL;
	gboolean ok;

	*token = NULL;
	fetch->login_key = key;
	fetch->login_pin = pin;
	fetch->login_refused = FALSE;
	ok = info->login(info, fetch, token, &local);
	fetch->login_key = NULL;
	fetch->login_pin = NULL;
	if (ok && !token_usable(*token)) {
		axis_meta_secret_free(*token);
		*token = NULL;
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "%s's login gave no token Axis can send",
			info->label);
		return FALSE;
	}
	if (!ok) {
		axis_meta_secret_free(*token);
		*token = NULL;
		g_propagate_error(error, g_steal_pointer(&local));
	}
	return ok;
}

/**
 * current_token:
 * @stale: (nullable): a token the provider has just refused: one other than
 *   it is wanted
 * @fresh: (out): the token was got by a login made for this call
 *
 * The token to send for the key and PIN set now: the one held, or one from a
 * new login (one at a time; whoever waited for it takes it).
 */
static gboolean
current_token(
	AxisMetaFetch  *fetch,
	const gchar    *stale,
	gchar         **token,
	gboolean       *fresh,
	GError        **error
){
	AxisMetaProviderEntry *provider = fetch->provider;
	const AxisMetaProviderInfo *info = provider->info;
	g_autofree gchar *print = NULL;
	gchar *key;
	gchar *pin;
	gchar *got = NULL;
	gboolean ok;

	*token = NULL;
	*fresh = FALSE;
	key = axis_meta_provider_key(fetch->meta, provider);
	if (key == NULL) {
		g_mutex_lock(&provider->token_lock);
		drop_token(provider);
		g_mutex_unlock(&provider->token_lock);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NO_KEY,
			"no key is set for %s: set it in Settings, under Metadata", info->label);
		return FALSE;
	}
	pin = provider_pin(fetch->meta, provider);
	print = fingerprint(key, pin);

	g_mutex_lock(&provider->token_lock);
	if (provider->refused != NULL && strcmp(provider->refused, print) == 0) {
		g_mutex_unlock(&provider->token_lock);
		axis_meta_secret_free(pin);
		axis_meta_secret_free(key);
		set_refused_error(info, error);
		return FALSE;
	}
	/* The key or the PIN changed since a refusal: they are tried again. */
	g_clear_pointer(&provider->refused, g_free);
	*token = held_token(fetch, print, stale);
	if (*token == NULL && provider->logging_in) {
		/* Another's login: waited for, no longer than its deadline, and its
		 * outcome taken; this caller does not log in after it (review of
		 * V1/V2, fault 4). */
		gint64 until = MIN(fetch->deadline_us, g_get_monotonic_time() + login_wait_us(fetch->meta));
		gboolean refused;

		while (provider->logging_in && g_cond_wait_until(&provider->login_done, &provider->token_lock, until))
			;
		*token = held_token(fetch, print, stale);
		refused = provider->refused != NULL && strcmp(provider->refused, print) == 0;
		g_mutex_unlock(&provider->token_lock);
		axis_meta_secret_free(pin);
		axis_meta_secret_free(key);
		if (*token != NULL)
			return TRUE;
		if (refused)
			set_refused_error(info, error);
		else
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE,
				"%s's login did not succeed just now; it is tried again later", info->label);
		return FALSE;
	}
	if (*token != NULL) {
		g_mutex_unlock(&provider->token_lock);
		axis_meta_secret_free(pin);
		axis_meta_secret_free(key);
		return TRUE;
	}
	provider->logging_in = TRUE;
	g_mutex_unlock(&provider->token_lock);

	{
		/* The login has a deadline of its own, for itself and its waiters. */
		gint64 deadline = fetch->deadline_us;

		fetch->deadline_us = MIN(deadline, g_get_monotonic_time() + login_wait_us(fetch->meta));
		ok = log_in(fetch, key, pin, &got, error);
		fetch->deadline_us = deadline;
	}
	axis_meta_secret_free(pin);
	axis_meta_secret_free(key);
	g_mutex_lock(&provider->token_lock);
	if (ok) {
		drop_token(provider);
		provider->token = g_strdup(got);
		provider->token_for = g_strdup(print);
		provider->token_us = g_get_monotonic_time();
	} else if (fetch->login_refused) {
		drop_token(provider);
		provider->refused = g_strdup(print);
	}
	provider->logging_in = FALSE;
	g_cond_broadcast(&provider->login_done);
	g_mutex_unlock(&provider->token_lock);
	if (!ok)
		return FALSE;
	*token = got;
	*fresh = TRUE;
	return TRUE;
}

gboolean
axis_meta_fetch_login(
	AxisMetaFetch  *fetch,
	GBytes        **body,
	GError        **error
){
	const AxisMetaProviderInfo *info;
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GError) local = NULL;
	gboolean ok;

	g_return_val_if_fail(fetch != NULL && body != NULL, FALSE);

	*body = NULL;
	info = fetch->provider->info;
	if (fetch->login_key == NULL || info->login == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "a login is made by the layer only");
		return FALSE;
	}
	if (!take_token(fetch, error))
		return FALSE;
	/* No credential header: the key and the PIN are in the body only. */
	config = config_for(fetch, FALSE, NULL, fetch->login_key);
	config.max_bytes = AXIS_META_TOKEN_MAX_BYTES * 2;
	ok = axis_http_post_login(&config, fetch->login_key, fetch->login_pin, body, &response, &local);
	if (ok) {
		axis_meta_limiter_answered(fetch->provider->limiter);
		axis_http_response_clear(&response);
		return TRUE;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS) &&
	    (response.status == 401 || response.status == 403)) {
		/* It answered: the key (or the PIN) is not one it takes. */
		axis_meta_limiter_answered(fetch->provider->limiter);
		fetch->login_refused = TRUE;
		set_refused_error(info, error);
		axis_http_response_clear(&response);
		return FALSE;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG)) {
		/* A key that no login can carry: refused here, nothing sent. */
		fetch->login_refused = TRUE;
		set_refused_error(info, error);
		axis_http_response_clear(&response);
		return FALSE;
	}
	report_failure(fetch, &response, local, error);
	axis_http_response_clear(&response);
	return FALSE;
}

/* A GET to a provider that logs in: with its token, logging in when there is
 * none; a 401 to a token that worked before brings one new login and one
 * more try. */
static gboolean
token_request(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	GError        **error
){
	g_autoptr(GError) local = NULL;
	gchar *token = NULL;
	gchar *renewed = NULL;
	gboolean fresh = FALSE;
	guint status = 0;
	gboolean ok;

	*body = NULL;
	if (!current_token(fetch, NULL, &token, &fresh, error))
		return FALSE;
	ok = send_one(fetch, FALSE, path, token, !fresh, body, NULL, &status, &local);
	if (!ok && !fresh && status == 401 && g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS)) {
		g_clear_error(&local);
		if (!current_token(fetch, token, &renewed, &fresh, error)) {
			axis_meta_secret_free(token);
			return FALSE;
		}
		ok = send_one(fetch, FALSE, path, renewed, FALSE, body, NULL, &status, &local);
		axis_meta_secret_free(renewed);
	}
	axis_meta_secret_free(token);
	if (!ok)
		g_propagate_error(error, g_steal_pointer(&local));
	return ok;
}

static gboolean
request(
	AxisMetaFetch  *fetch,
	gboolean        image,
	const gchar    *path,
	GBytes        **body,
	gchar         **content_type,
	GError        **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	gchar *key = NULL;
	guint status = 0;
	gboolean ok;

	*body = NULL;
	if (content_type != NULL)
		*content_type = NULL;
	if (!image && info->login != NULL)
		return token_request(fetch, path, body, error);
	if (!image && info->key_header != NULL) {
		key = axis_meta_provider_key(fetch->meta, fetch->provider);
		if (key == NULL) {
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NO_KEY,
				"no key is set for %s: set it in Settings, under Metadata", info->label);
			return FALSE;
		}
	}
	ok = send_one(fetch, image, path, key, FALSE, body, content_type, &status, error);
	axis_meta_secret_free(key);
	return ok;
}

gboolean
axis_meta_fetch_get(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	GError        **error
){
	g_return_val_if_fail(fetch != NULL && body != NULL, FALSE);

	return request(fetch, FALSE, path, body, NULL, error);
}

gboolean
axis_meta_fetch_image(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	gchar         **content_type,
	GError        **error
){
	return request(fetch, TRUE, path, body, content_type, error);
}

/* ── Questions ──────────────────────────────────────────────────── */

AxisMetaPending *
axis_meta_pending_new(
	AxisMetaAsk            ask,
	AxisMetaProviderEntry *provider
){
	AxisMetaPending *pending = g_new0(AxisMetaPending, 1);

	pending->refs = 1;
	pending->ask = ask;
	pending->provider = provider;
	pending->began_us = g_get_monotonic_time();
	return pending;
}

static void
pending_free(AxisMetaPending *pending)
{
	g_free(pending->key);
	g_free(pending->text);
	g_free(pending->query);
	g_clear_error(&pending->error);
	g_free(pending);
}

void
axis_meta_pending_unref(
	AxisMeta        *self,
	AxisMetaPending *pending
){
	if (pending != NULL && g_atomic_int_dec_and_test(&pending->refs))
		pending_free(pending);
}

static gboolean
check_titles(
	GPtrArray    *titles,
	const gchar  *provider,
	gboolean      detail,
	GError      **error
){
	guint i;

	if (titles->len > AXIS_META_MAX_RESULTS) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer has too many results");
		return FALSE;
	}
	for (i = 0; i < titles->len; i++) {
		AxisMetaTitle *title = g_ptr_array_index(titles, i);
		g_autofree gchar *problem = NULL;

		title->detail = detail;
		if (!axis_meta_title_check(title, provider, &problem)) {
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s", problem);
			return FALSE;
		}
	}
	return TRUE;
}

/* Stores a list of results as the lookup @lookup:@query. */
static gboolean
store_results(
	AxisMeta              *self,
	AxisMetaProviderEntry *provider,
	AxisMetaKind           kind,
	const gchar           *lookup,
	const gchar           *query,
	gint                   year,
	GPtrArray             *titles,
	GError               **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;
	guint i;

	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);
		gint64 id = 0;

		if (title->kind != kind)
			continue;
		if (!axis_meta_store_title_from(conn, title, provider->info->secondary, &id, &local)) {
			axis_db_pool_release(self->pool, conn);
			axis_meta_set_failed(error, local);
			return FALSE;
		}
		/* (A secondary provider's result about a title Axis does not hold
		 * is not kept.) */
		if (id != 0)
			g_array_append_val(ids, id);
	}
	if (!axis_meta_store_put_lookup(conn, provider->info->name, kind, lookup, query, year, ids, &local)) {
		axis_db_pool_release(self->pool, conn);
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

static gboolean
run_title(
	AxisMeta        *self,
	AxisMetaFetch   *fetch,
	AxisMetaPending *pending,
	GError         **error
){
	const AxisMetaProviderInfo *info = pending->provider->info;
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	AxisDbConn *conn;
	gint64 id = 0;
	gboolean ok;

	ok = info->title(info, fetch, pending->kind, pending->text, &title, &local);
	if (!ok && !g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (ok) {
		if (title != NULL)
			title->detail = TRUE;
		if (title == NULL || !axis_meta_title_check(title, info->name, &problem) ||
		    title->kind != pending->kind || g_strcmp0(title->provider_id, pending->text) != 0) {
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s",
				problem != NULL ? problem : "it is about another title");
			return FALSE;
		}
	}
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	if (ok && !axis_meta_store_title_from(conn, title, info->secondary, &id, &local)) {
		axis_db_pool_release(self->pool, conn);
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	if (ok && id == 0) {
		/* A secondary provider's record of a series Axis does not hold: not
		 * kept, and no answer is remembered for it. */
		axis_db_pool_release(self->pool, conn);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "%s adds to a series TMDb knows; one Axis does "
			"not hold is not kept from %s alone", info->label, info->label);
		return FALSE;
	}
	if (ok)
		g_array_append_val(ids, id);
	/* A negative answer is remembered as an empty lookup, and a positive
	 * one replaces it.  Failing to note it is not worth failing for. */
	(void)axis_meta_store_put_lookup(conn, info->name, pending->kind, "title", pending->text, 0, ids, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "%s has no such title", info->label);
		return FALSE;
	}
	return TRUE;
}

static gboolean refuse_emptied(AxisMeta *self, const AxisMetaProviderInfo *info, const gchar *series_id,
	const gchar *source, gint number, GError **error);

static gboolean
run_season(
	AxisMeta        *self,
	AxisMetaFetch   *fetch,
	AxisMetaPending *pending,
	GError         **error
){
	const AxisMetaProviderInfo *info = pending->provider->info;
	g_autoptr(AxisMetaSeason) season = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	AxisDbConn *conn;
	gint64 title_id = 0;

	if (!info->season(info, fetch, pending->text, pending->number, &season, error))
		return FALSE;
	if (season == NULL || season->number != pending->number || !axis_meta_season_check(season, &problem)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s",
			problem != NULL ? problem : "it is about another season");
		return FALSE;
	}
	if ((season->episodes == NULL || season->episodes->len == 0) && refuse_emptied(self, info, pending->text,
	        pending->query != NULL ? pending->query : info->name, pending->number, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	if (!axis_meta_store_find_id(conn, AXIS_META_KIND_SERIES, info->name, pending->text, &title_id, &local) ||
	    title_id == 0 || !axis_meta_store_season(conn, title_id, pending->query != NULL ? pending->query : info->name,
	        season, &local)) {
		axis_db_pool_release(self->pool, conn);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			g_propagate_error(error, g_steal_pointer(&local));
		else if (local == NULL)
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "the series is not stored");
		else
			axis_meta_set_failed(error, local);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

/* Whether the store holds an episode of @source of @title_id (in season
 * @number, or any when it is -1). */
static gboolean
holds_episodes(
	AxisDbConn   *conn,
	gint64        title_id,
	const gchar  *source,
	gint          number,
	gboolean     *holds,
	GError      **error
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autofree gchar *season = g_strdup_printf("%d", number);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];

	params[0] = id;
	params[1] = source;
	params[2] = season;
	*holds = FALSE;
	if (!axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM meta_episode WHERE title_id = $1::bigint AND source = $2"
	        " AND ($3::integer < 0 OR season_number = $3::integer))", 3, params, &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, holds);
	return TRUE;
}

/* An answer with no episode for a source the store holds episodes of is the
 * provider's fault, never stored: it would remove them all (review of
 * V1/V2, fault 3). */
static gboolean
refuse_emptied(
	AxisMeta                   *self,
	const AxisMetaProviderInfo *info,
	const gchar                *series_id,
	const gchar                *source,
	gint                        number,
	GError                    **error
){
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, &local);
	gint64 title_id = 0;
	gboolean holds = FALSE;
	gboolean ok;

	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return TRUE;
	}
	ok = axis_meta_store_find_id(conn, AXIS_META_KIND_SERIES, info->name, series_id, &title_id, &local) &&
		(title_id == 0 || holds_episodes(conn, title_id, source, number, &holds, &local));
	if (ok && holds) {
		/* The same empty answer on separate refreshes spread over a day is
		 * a real removal, taken; before that, the refusal is counted and the
		 * season waits its turn again (0058_empty_seasons.sql). */
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
		g_autofree gchar *season = g_strdup_printf("%d", number);
		g_autofree gchar *needed = g_strdup_printf("%d", AXIS_META_EMPTY_ANSWERS);
		g_autoptr(AxisDbResult) taken = NULL;
		const gchar *params[4];

		params[0] = id;
		params[1] = source;
		params[2] = season;
		params[3] = needed;
		ok = axis_db_conn_exec(conn, "SELECT coalesce(bool_and(empty_answers + 1 >= $4::integer"
			" AND empty_since <= now() - interval '1 day'), false) FROM meta_season WHERE title_id = $1::bigint"
			" AND source = $2 AND ($3::integer < 0 OR number = $3::integer)", 4, params, &taken, &local);
		if (ok) {
			gboolean real = FALSE;

			axis_db_result_get_boolean(taken, 0, 0, &real);
			holds = !real;
		}
		if (ok && holds)
			ok = axis_db_conn_exec(conn, "UPDATE meta_season SET empty_answers = empty_answers + 1,"
				" empty_since = coalesce(empty_since, now()), fetched_at = now(), episodes_fetched_at = now()"
				" WHERE title_id = $1::bigint AND source = $2 AND ($3::integer < 0 OR number = $3::integer)", 3, params,
				NULL, &local);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_meta_set_failed(error, local);
		return TRUE;
	}
	if (holds)
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "%s answered no episode for a series it listed "
			"episodes of before; that answer is not kept unless it is given again on later refreshes", info->label);
	return holds;
}

/* Every season of a source of numbering, from a provider that numbers a
 * series in several orders: read whole, checked whole, then stored apart
 * from every other source's seasons in one transaction. */
static gboolean
run_order(
	AxisMeta        *self,
	AxisMetaFetch   *fetch,
	AxisMetaPending *pending,
	GError         **error
){
	const AxisMetaProviderInfo *info = pending->provider->info;
	g_autoptr(GPtrArray) seasons = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	AxisDbConn *conn;
	gint64 title_id = 0;
	guint i;

	if (!info->order(info, fetch, pending->text, pending->query, &seasons, error))
		return FALSE;
	if (seasons == NULL || seasons->len > AXIS_META_MAX_SEASONS) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: "
			"it is no list of seasons");
		return FALSE;
	}
	{
		guint episodes = 0;

		for (i = 0; i < seasons->len; i++)
			episodes += ((const AxisMetaSeason *)g_ptr_array_index(seasons, i))->episodes != NULL ?
				((const AxisMetaSeason *)g_ptr_array_index(seasons, i))->episodes->len : 0;
		/* An empty order for a source with episodes: refused, until it is
		 * taken as a real removal. */
		if (episodes == 0 && refuse_emptied(self, info, pending->text, pending->query, -1, error))
			return FALSE;
	}
	for (i = 0; i < seasons->len; i++) {
		const AxisMetaSeason *season = g_ptr_array_index(seasons, i);
		guint j;

		if (!axis_meta_season_check(season, &problem) || season->episodes == NULL) {
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s",
				problem != NULL ? problem : "a season has no episodes");
			return FALSE;
		}
		for (j = 0; j < i; j++) {
			if (((const AxisMetaSeason *)g_ptr_array_index(seasons, j))->number == season->number) {
				g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE,
					"the answer cannot be used: a season is listed twice");
				return FALSE;
			}
		}
	}
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	if (!axis_meta_store_find_id(conn, AXIS_META_KIND_SERIES, info->name, pending->text, &title_id, &local) ||
	    title_id == 0 || !axis_meta_store_order(conn, title_id, pending->query, seasons, &local)) {
		axis_db_pool_release(self->pool, conn);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			g_propagate_error(error, g_steal_pointer(&local));
		else if (local == NULL)
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "the series is not stored");
		else
			axis_meta_set_failed(error, local);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

/* Asks and stores, on whatever thread calls it. */
static gboolean
run(
	AxisMeta        *self,
	AxisMetaPending *pending,
	GCancellable    *cancellable,
	guint            budget_ms,
	GError         **error
){
	const AxisMetaProviderInfo *info = pending->provider->info;
	AxisMetaFetch fetch;
	g_autoptr(GPtrArray) titles = NULL;
	gboolean ok = FALSE;

	memset(&fetch, 0, sizeof(fetch));
	fetch.meta = self;
	fetch.provider = pending->provider;
	fetch.cancellable = cancellable;
	fetch.deadline_us = g_get_monotonic_time() + (gint64)budget_ms * 1000;

	switch (pending->ask) {
	case AXIS_META_ASK_SEARCH:
		if (!info->search(info, &fetch, pending->kind, pending->text, pending->number, MAX(pending->page, 1), &titles,
		        error))
			return FALSE;
		if (titles == NULL)
			titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
		if (!check_titles(titles, info->name, FALSE, error))
			return FALSE;
		ok = store_results(self, pending->provider, pending->kind, pending->page > 1 ? "search2" : "search",
			pending->query, pending->number, titles, error);
		break;
	case AXIS_META_ASK_FIND: {
		g_autofree gchar *query = g_strdup_printf("%s:%s", pending->text, pending->query);

		if (!info->find_by_external(info, &fetch, pending->kind, pending->text, pending->query, &titles, error))
			return FALSE;
		if (titles == NULL)
			titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
		if (!check_titles(titles, info->name, FALSE, error))
			return FALSE;
		ok = store_results(self, pending->provider, pending->kind, "find", query, 0, titles, error);
		break;
	}
	case AXIS_META_ASK_TITLE:
		ok = run_title(self, &fetch, pending, error);
		break;
	case AXIS_META_ASK_SEASON:
		ok = run_season(self, &fetch, pending, error);
		break;
	case AXIS_META_ASK_ART:
		ok = axis_meta_art_fetch_now(self, &fetch, pending->text, pending->size, NULL, error);
		break;
	case AXIS_META_ASK_ORDER:
		ok = run_order(self, &fetch, pending, error);
		break;
	}
	return ok;
}

static guint
background_ms(AxisMeta *self)
{
	return self->tuning.background_ms != 0 ? self->tuning.background_ms : AXIS_META_BACKGROUND_MS;
}

/* A caller reads the store, finds nothing (or something stale), and only
 * then asks.  Between the two, another caller's identical question can be
 * answered and leave the in-flight table, and the provider would be asked
 * twice.  So the time each question was last answered is remembered for a
 * while, and a question that began (AxisMetaPending.began_us, taken before
 * the caller read the store) before that answer arrived is not sent: the
 * store already holds what it would fetch.  A question that began after
 * the answer is sent as usual. */
#define ANSWERED_US (60 * G_USEC_PER_SEC)
#define ANSWERED_MAX 1024

static gboolean
answered_is_old(
	gpointer key,
	gpointer value,
	gpointer user_data
){
	return *(const gint64 *)value + ANSWERED_US < *(const gint64 *)user_data;
}

/* With the lock held. */
static void
note_answered(
	AxisMeta    *self,
	const gchar *key
){
	gint64 now = g_get_monotonic_time();
	gint64 *when;

	g_hash_table_foreach_remove(self->answered, answered_is_old, &now);
	if (g_hash_table_size(self->answered) >= ANSWERED_MAX)
		g_hash_table_remove_all(self->answered);
	when = g_new(gint64, 1);
	*when = now;
	g_hash_table_replace(self->answered, g_strdup(key), when);
}

/* With the lock held. */
static gboolean
answered_since(
	AxisMeta    *self,
	const gchar *key,
	gint64       began_us
){
	const gint64 *when = g_hash_table_lookup(self->answered, key);

	return when != NULL && *when >= began_us;
}

/* A fetch thread: one question. */
static void
run_task(
	gpointer data,
	gpointer user_data
){
	AxisMetaPending *pending = data;
	AxisMeta *self = user_data;
	GError *error = NULL;
	gboolean ok;

	ok = run(self, pending, self->cancel, background_ms(self), &error);
	g_mutex_lock(&self->lock);
	pending->finished = TRUE;
	pending->ok = ok;
	pending->error = error;
	if (g_hash_table_lookup(self->inflight, pending->key) == pending)
		g_hash_table_remove(self->inflight, pending->key);
	if (ok)
		note_answered(self, pending->key);
	g_cond_broadcast(&self->done);
	g_mutex_unlock(&self->lock);
	axis_meta_pending_unref(self, pending);
}

void
axis_meta_fetch_init_provider(
	AxisMeta              *self,
	AxisMetaProviderEntry *provider
){
	provider->threads = g_thread_pool_new(run_task, self, AXIS_META_FETCH_THREADS, FALSE, NULL);
}

gboolean
axis_meta_ask(
	AxisMeta         *self,
	AxisMetaPending  *pending,
	guint             wait_ms,
	GError          **error
){
	AxisMetaPending *existing;
	g_autoptr(GError) held = NULL;
	gint64 deadline;
	gboolean ok;

	/* A provider that asked to wait (or is backing off) for longer than
	 * the caller may wait is not waited for: the question is queued, and
	 * the caller is told at once. */
	if (wait_ms > 0)
		held_beyond(pending->provider, g_get_real_time() + (gint64)wait_ms * 1000, &held);
	g_mutex_lock(&self->lock);
	if (g_atomic_int_get(&self->stopped)) {
		g_mutex_unlock(&self->lock);
		axis_meta_pending_unref(self, pending);
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "the daemon is stopping");
		return FALSE;
	}
	existing = g_hash_table_lookup(self->inflight, pending->key);
	if (existing == NULL && answered_since(self, pending->key, pending->began_us)) {
		/* Answered for someone else since this caller looked: the store has it. */
		g_mutex_unlock(&self->lock);
		axis_meta_pending_unref(self, pending);
		return TRUE;
	}
	if (existing != NULL) {
		axis_meta_pending_unref(self, pending);
		pending = existing;
		g_atomic_int_inc(&pending->refs);
	} else {
		/* The thread's reference; the table borrows it. */
		g_atomic_int_inc(&pending->refs);
		g_hash_table_insert(self->inflight, pending->key, pending);
		g_thread_pool_push(pending->provider->threads, pending, NULL);
	}
	if (held != NULL && !pending->finished) {
		g_mutex_unlock(&self->lock);
		axis_meta_pending_unref(self, pending);
		g_propagate_error(error, g_steal_pointer(&held));
		return FALSE;
	}
	deadline = g_get_monotonic_time() + (gint64)wait_ms * 1000;
	while (!pending->finished && wait_ms > 0) {
		if (!g_cond_wait_until(&self->done, &self->lock, deadline))
			break;
	}
	ok = pending->finished && pending->ok;
	if (!ok) {
		if (!pending->finished)
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE, "%s did not answer within %u s",
				pending->provider->info->label, (wait_ms + 999) / 1000);
		else if (pending->error != NULL)
			g_propagate_error(error, g_error_copy(pending->error));
		else
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "the question failed");
	}
	g_mutex_unlock(&self->lock);
	axis_meta_pending_unref(self, pending);
	return ok;
}

gboolean
axis_meta_ask_now(
	AxisMeta         *self,
	AxisMetaPending  *pending,
	GCancellable     *cancellable,
	GError          **error
){
	gboolean ok = run(self, pending, cancellable, background_ms(self), error);

	axis_meta_pending_unref(self, pending);
	return ok;
}

/* ── Lists, asked for by a job ──────────────────────────────────── */

/* The provider called @name, offering @list for @kind (any list when @list
 * is -1), with its fetch handle filled in. */
static AxisMetaProviderEntry *
list_provider(
	AxisMeta       *self,
	const gchar    *name,
	AxisMetaKind    kind,
	gint            list,
	GCancellable   *cancellable,
	AxisMetaFetch  *fetch,
	GError        **error
){
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, name);
	guint offered;

	if (entry == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "there is no such metadata provider");
		return NULL;
	}
	offered = axis_meta_provider_lists(entry->info, kind);
	if (offered == 0 || (list >= 0 && (offered & AXIS_META_LIST_BIT(list)) == 0)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "%s offers no such list of %s", entry->info->label,
			kind == AXIS_META_KIND_SERIES ? "series" : "movies");
		return NULL;
	}
	if (g_atomic_int_get(&self->stopped)) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "the daemon is stopping");
		return NULL;
	}
	memset(fetch, 0, sizeof(*fetch));
	fetch->meta = self;
	fetch->provider = entry;
	fetch->cancellable = cancellable != NULL ? cancellable : self->cancel;
	fetch->deadline_us = g_get_monotonic_time() + (gint64)background_ms(self) * 1000;
	return entry;
}

gboolean
axis_meta_fetch_list(
	AxisMeta           *self,
	const gchar        *provider,
	AxisMetaKind        kind,
	AxisMetaList        list,
	const gchar        *id,
	guint               page,
	GCancellable       *cancellable,
	AxisMetaListPage  **out,
	GError            **error
){
	g_autoptr(AxisMetaListPage) answer = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	AxisMetaProviderEntry *entry;
	AxisMetaFetch fetch;
	AxisDbConn *conn;
	guint i;

	g_return_val_if_fail(self != NULL && out != NULL, FALSE);

	*out = NULL;
	if (axis_meta_list_to_string(list) == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "there is no such list");
		return FALSE;
	}
	entry = list_provider(self, provider, kind, (gint)list, cancellable, &fetch, error);
	if (entry == NULL)
		return FALSE;
	if (page < 1 || page > AXIS_META_LIST_MAX_PAGE) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "that is not a page of a list");
		return FALSE;
	}
	if ((list == AXIS_META_LIST_RECOMMENDED) != (id != NULL) ||
	    (id != NULL && !axis_meta_id_valid(entry->info->name, id))) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID,
			"recommendations are asked for one of the provider's titles, and only they");
		return FALSE;
	}
	if (!entry->info->list(entry->info, &fetch, kind, list, id, page, &answer, error))
		return FALSE;
	if (answer == NULL || !axis_meta_list_page_check(answer, kind, entry->info->name, &problem)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s",
			problem != NULL ? problem : "there is no list");
		return FALSE;
	}

	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	for (i = 0; i < answer->entries->len; i++) {
		AxisMetaListEntry *item = g_ptr_array_index(answer->entries, i);
		gint64 stored = 0;

		if (!axis_meta_store_title_from(conn, item->title, entry->info->secondary, &stored, &local)) {
			axis_db_pool_release(self->pool, conn);
			axis_meta_set_failed(error, local);
			return FALSE;
		}
		item->title->id = stored;
	}
	axis_db_pool_release(self->pool, conn);
	*out = g_steal_pointer(&answer);
	return TRUE;
}

gboolean
axis_meta_fetch_genres(
	AxisMeta      *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	GCancellable  *cancellable,
	GPtrArray    **genres,
	GError       **error
){
	g_autoptr(GPtrArray) answer = NULL;
	g_autofree gchar *problem = NULL;
	AxisMetaProviderEntry *entry;
	AxisMetaFetch fetch;

	g_return_val_if_fail(self != NULL && genres != NULL, FALSE);

	*genres = NULL;
	entry = list_provider(self, provider, kind, -1, cancellable, &fetch, error);
	if (entry == NULL)
		return FALSE;
	if (entry->info->genres == NULL) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "%s names no genres", entry->info->label);
		return FALSE;
	}
	if (!entry->info->genres(entry->info, &fetch, kind, &answer, error))
		return FALSE;
	if (answer == NULL || !axis_meta_genres_check(answer, &problem)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the answer cannot be used: %s",
			problem != NULL ? problem : "there is no list of genres");
		return FALSE;
	}
	*genres = g_steal_pointer(&answer);
	return TRUE;
}

/* ── Testing a key ──────────────────────────────────────────────── */

/* Why the test did not work, safe to show: the layer's and the client's own
 * messages hold no key (they are written from fixed text and scrubbed). */
static gchar *
problem_line(
	const AxisMetaProviderInfo *info,
	const GError               *error
){
	if (error != NULL && (error->domain == AXIS_META_ERROR || error->domain == AXIS_HTTP_ERROR))
		return axis_library_text_clean(error->message, 300);
	return g_strdup_printf("%s could not be asked", info->label);
}

static void
note_test(
	AxisMetaProviderEntry *entry,
	gboolean               ok,
	const gchar           *text
){
	g_mutex_lock(&entry->token_lock);
	g_free(entry->test_text);
	entry->test_text = g_strdup(text);
	entry->test_ok = ok;
	entry->test_us = g_get_real_time();
	g_mutex_unlock(&entry->token_lock);
}

gboolean
axis_meta_test_key(
	AxisMeta      *self,
	const gchar   *provider,
	GCancellable  *cancellable,
	gchar        **summary,
	GError       **error
){
	AxisMetaProviderEntry *entry;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *said = NULL;
	AxisMetaFetch fetch;

	g_return_val_if_fail(self != NULL && summary != NULL, FALSE);

	*summary = NULL;
	entry = axis_meta_entry_find(self, provider);
	if (entry == NULL || entry->info->test == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "that provider has no test of its key");
		return FALSE;
	}
	if (g_atomic_int_get(&self->stopped)) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "the daemon is stopping");
		return FALSE;
	}
	memset(&fetch, 0, sizeof(fetch));
	fetch.meta = self;
	fetch.provider = entry;
	fetch.cancellable = cancellable != NULL ? cancellable : self->cancel;
	fetch.deadline_us = g_get_monotonic_time() + (gint64)background_ms(self) * 1000;
	/* The key itself is tested: a new login, not the token held. */
	g_mutex_lock(&entry->token_lock);
	drop_token(entry);
	g_mutex_unlock(&entry->token_lock);
	if (!entry->info->test(entry->info, &fetch, &said, &local)) {
		g_autofree gchar *text = problem_line(entry->info, local);

		note_test(entry, FALSE, text);
		*summary = g_steal_pointer(&text);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	note_test(entry, TRUE, said);
	*summary = g_steal_pointer(&said);
	return TRUE;
}
