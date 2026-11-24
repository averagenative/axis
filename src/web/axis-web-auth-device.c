/*
 * axis-web-auth-device.c - remembered devices and the forwarding hint
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A remembered device is a browser that has logged in to an account
 * before.  At every successful login it is given a cookie,
 *
 *     <account id>.<epoch>.<issued, seconds since 1970>.<nonce>.<signature>
 *
 * where the signature is a keyed BLAKE2b of the rest under the daemon's
 * device key, and is recorded with the account (its nonce, epoch and when
 * it was issued; at most AXIS_DEVICE_MAX_PER_USER, the oldest dropped).  It
 * is not a credential (the password is still needed); it only says which
 * count of failures a login attempt is held to.  A stranger who keeps a
 * known username locked, or who shares the owner's address (every client
 * does, behind a proxy that is not trusted), fills the username's and the
 * address's counts; the owner's own browser is held to its device's count,
 * which only its own failures fill, and to the account's ceiling for all
 * its devices together, so that devices cannot multiply guesses.
 *
 * A device is valid only while the account's epoch is the one it was
 * issued under: a password change, a disable or anything else that ends
 * every session of the account ends its remembered devices too.
 *
 * The key ("device-key") and the record of devices ("devices") are kept in
 * the data directory, so a device stays remembered across restarts; losing
 * them only makes every browser a stranger again until it logs in.
 */

#include "web/axis-web-auth-private.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-limits.h"

#include <errno.h>
#include <fcntl.h>
#include <sodium.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/axis-error.h"

#define DEVICE_KEY_FILE     "device-key"
#define DEVICE_KEY_TEMP     "device-key.tmp"
#define DEVICE_LIST_FILE    "devices"
#define DEVICE_LIST_TEMP    "devices.tmp"
#define DEVICE_LIST_MAX     (1024 * 1024)
#define DEVICE_SIGN_PURPOSE "axis device v2:"
#define DEVICE_MAC_BYTES    32
#define DEVICE_MAC_LEN      43	/* base64url of 32 bytes, no padding */

/* A device cookie issued a little in the future (clocks of two daemons on
 * one database) is still accepted. */
#define DEVICE_CLOCK_SLACK_S 300

static const gchar *
device_cookie_name(gboolean secure)
{
	return secure ? AXIS_DEVICE_COOKIE_SECURE : AXIS_DEVICE_COOKIE;
}

/* One remembered device of an account. */
typedef struct
{
	gchar  nonce[AXIS_PUBLIC_ID_LEN + 1];
	gint   epoch;
	gint64 issued_s;
} Device;

void
axis_web_auth_device_init(AxisWebAuth *self)
{
	randombytes_buf(self->device_key, sizeof(self->device_key));
	self->devices = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	self->device_dir_fd = -1;
}

void
axis_web_auth_device_clear(AxisWebAuth *self)
{
	g_clear_pointer(&self->devices, g_hash_table_unref);
	if (self->device_dir_fd >= 0) {
		close(self->device_dir_fd);
		self->device_dir_fd = -1;
	}
}

/* The devices of @user_id, oldest first (made when @create). */
static GPtrArray *
devices_of(
	AxisWebAuth *self,
	gint64       user_id,
	gboolean     create
){
	GPtrArray *list = g_hash_table_lookup(self->devices, &user_id);

	if (list == NULL && create) {
		gint64 *key = g_new(gint64, 1);

		*key = user_id;
		list = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(self->devices, key, list);
	}
	return list;
}

/* Records a device of @user_id; only the account's newest
 * AXIS_DEVICE_MAX_PER_USER under @epoch are kept. */
static void
devices_add(
	AxisWebAuth *self,
	gint64       user_id,
	gint         epoch,
	gint64       issued_s,
	const gchar *nonce
){
	GPtrArray *list = devices_of(self, user_id, TRUE);
	Device *device = g_new0(Device, 1);
	guint i;

	/* Those of an earlier epoch are of no use any more. */
	for (i = list->len; i > 0; i--) {
		if (((Device *)g_ptr_array_index(list, i - 1))->epoch != epoch)
			g_ptr_array_remove_index(list, i - 1);
	}
	while (list->len >= AXIS_DEVICE_MAX_PER_USER)
		g_ptr_array_remove_index(list, 0);

	g_strlcpy(device->nonce, nonce, sizeof(device->nonce));
	device->epoch = epoch;
	device->issued_s = issued_s;
	g_ptr_array_add(list, device);
}

static gboolean
devices_has(
	AxisWebAuth *self,
	gint64       user_id,
	gint         epoch,
	const gchar *nonce
){
	GPtrArray *list = devices_of(self, user_id, FALSE);
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		const Device *device = g_ptr_array_index(list, i);

		if (device->epoch == epoch && axis_secret_equal(device->nonce, nonce))
			return TRUE;
	}
	return FALSE;
}

/* Writes the record of devices to the data directory, when there is one:
 * a temporary file renamed over the record.  Not synced: losing it to a
 * crash only forgets the devices. */
static void
devices_save(AxisWebAuth *self)
{
	GString *text;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gsize done = 0;
	gboolean ok;
	gint fd;

	if (self->device_dir_fd < 0)
		return;

	text = g_string_new(NULL);
	g_hash_table_iter_init(&iter, self->devices);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GPtrArray *list = value;
		guint i;

		for (i = 0; i < list->len; i++) {
			const Device *device = g_ptr_array_index(list, i);

			g_string_append_printf(text, "%" G_GINT64_FORMAT " %d %" G_GINT64_FORMAT " %s\n", *(gint64 *)key,
				device->epoch, device->issued_s, device->nonce);
		}
	}

	fd = openat(self->device_dir_fd, DEVICE_LIST_TEMP, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	ok = fd >= 0;
	while (ok && done < text->len) {
		gssize n = write(fd, text->str + done, text->len - done);

		if (n < 0 && errno == EINTR)
			continue;
		ok = n > 0;
		if (ok)
			done += (gsize)n;
	}
	if (fd >= 0 && close(fd) != 0)
		ok = FALSE;
	if (ok)
		ok = renameat(self->device_dir_fd, DEVICE_LIST_TEMP, self->device_dir_fd, DEVICE_LIST_FILE) == 0;
	if (!ok) {
		unlinkat(self->device_dir_fd, DEVICE_LIST_TEMP, 0);
		g_warning("the record of remembered browsers (%s in the data directory) could not be written: %s; they are "
			"remembered until the daemon stops", DEVICE_LIST_FILE, g_strerror(errno));
	}
	sodium_memzero(text->str, text->len);
	g_string_free(text, TRUE);
}

/* Reads the record of devices from @dir_fd; a line that cannot be read is
 * skipped (a device it named is forgotten). */
static void
devices_load(
	AxisWebAuth *self,
	gint         dir_fd
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	struct stat st;
	gsize got = 0;
	gint fd;
	guint i;

	fd = openat(dir_fd, DEVICE_LIST_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > DEVICE_LIST_MAX) {
		close(fd);
		return;
	}
	text = g_malloc0((gsize)st.st_size + 1);
	while (got < (gsize)st.st_size) {
		gssize n = read(fd, text + got, (gsize)st.st_size - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (gsize)n;
	}
	close(fd);
	text[got] = '\0';

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = g_strsplit(lines[i], " ", 5);
		gint64 user_id;
		gint64 epoch;
		gint64 issued;

		if (g_strv_length(fields) != 4 || !axis_auth_token_is_wellformed(fields[3], AXIS_PUBLIC_ID_LEN))
			continue;
		if (!g_ascii_string_to_signed(fields[0], 10, 1, G_MAXINT64, &user_id, NULL) ||
		    !g_ascii_string_to_signed(fields[1], 10, 0, G_MAXINT, &epoch, NULL) ||
		    !g_ascii_string_to_signed(fields[2], 10, 0, G_MAXINT64, &issued, NULL))
			continue;
		devices_add(self, user_id, (gint)epoch, issued, fields[3]);
	}
	sodium_memzero(text, got);
}

guint
axis_web_auth_count_devices(
	AxisWebAuth *self,
	gint64       user_id
){
	GPtrArray *list;

	g_return_val_if_fail(self != NULL, 0);

	list = devices_of(self, user_id, FALSE);
	return list != NULL ? list->len : 0;
}

/* The signature of @text under the device key, base64url. */
static gchar *
device_mac(
	AxisWebAuth *self,
	const gchar *text
){
	guint8 out[DEVICE_MAC_BYTES];
	gchar encoded[sodium_base64_ENCODED_LEN(DEVICE_MAC_BYTES, sodium_base64_VARIANT_URLSAFE_NO_PADDING)];
	crypto_generichash_state state;

	crypto_generichash_init(&state, self->device_key, sizeof(self->device_key), sizeof(out));
	crypto_generichash_update(&state, (const guint8 *)DEVICE_SIGN_PURPOSE, strlen(DEVICE_SIGN_PURPOSE));
	crypto_generichash_update(&state, (const guint8 *)text, strlen(text));
	crypto_generichash_final(&state, out, sizeof(out));
	sodium_bin2base64(encoded, sizeof(encoded), out, sizeof(out), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
	sodium_memzero(out, sizeof(out));

	return g_strdup(encoded);
}

/* Whether @text is 1 to @max decimal digits, without a leading zero. */
static gboolean
all_digits(
	const gchar *text,
	gsize        max
){
	gsize len = strlen(text);
	gsize i;

	if (len == 0 || len > max || text[0] == '0')
		return FALSE;
	for (i = 0; i < len; i++) {
		if (text[i] < '0' || text[i] > '9')
			return FALSE;
	}

	return TRUE;
}

/* Whether @text is a decimal number of 1 to @max digits ("0" alone, or no
 * leading zero). */
static gboolean
a_number(
	const gchar *text,
	gsize        max
){
	return g_strcmp0(text, "0") == 0 || all_digits(text, max);
}

gchar *
axis_web_auth_device_of(
	AxisWebAuth    *self,
	HtmxRequest    *request,
	const AxisUser *account
){
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autofree gchar *cookie = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	cookie = axis_web_auth_get_cookie(request, device_cookie_name(state != NULL && state->secure));
	return axis_web_auth_device_of_value(self, cookie, account);
}

gchar *
axis_web_auth_device_of_value(
	AxisWebAuth    *self,
	const gchar    *cookie,
	const AxisUser *account
){
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *signed_part = NULL;
	g_autofree gchar *expected = NULL;
	gint64 issued;
	gint64 now;

	g_return_val_if_fail(self != NULL, NULL);

	if (account == NULL || account->id <= 0 || account->disabled)
		return NULL;
	if (cookie == NULL || strlen(cookie) > 160)
		return NULL;

	parts = g_strsplit(cookie, ".", 6);
	if (g_strv_length(parts) != 5 || !all_digits(parts[0], 18) || !a_number(parts[1], 10) ||
	    !all_digits(parts[2], 12) || !axis_auth_token_is_wellformed(parts[3], AXIS_PUBLIC_ID_LEN) ||
	    !axis_auth_token_is_wellformed(parts[4], DEVICE_MAC_LEN))
		return NULL;

	signed_part = g_strdup_printf("%s.%s.%s.%s", parts[0], parts[1], parts[2], parts[3]);
	expected = device_mac(self, signed_part);
	if (!axis_secret_equal(expected, parts[4]))
		return NULL;

	/* Signed by this daemon's key: now what it says can be believed.  It
	 * is this account's, under its epoch now, recent, and one of the
	 * devices recorded for it. */
	if (g_ascii_strtoll(parts[0], NULL, 10) != account->id || g_ascii_strtoll(parts[1], NULL, 10) != account->epoch)
		return NULL;
	issued = g_ascii_strtoll(parts[2], NULL, 10);
	now = axis_web_auth_now(self) / G_USEC_PER_SEC;
	if (issued > now + DEVICE_CLOCK_SLACK_S || now - issued > AXIS_DEVICE_MAX_AGE)
		return NULL;
	if (!devices_has(self, account->id, account->epoch, parts[3]))
		return NULL;

	return g_strdup_printf("%s/%s", parts[0], parts[3]);
}

void
axis_web_auth_remember_device(
	AxisWebAuth    *self,
	HtmxRequest    *request,
	HtmxResponse   *response,
	const AxisUser *account
){
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean secure = state != NULL && state->secure;
	g_autofree gchar *value = NULL;

	g_return_if_fail(self != NULL);
	g_return_if_fail(account != NULL);

	value = axis_web_auth_device_issue(self, account);
	axis_web_auth_set_cookie(response, device_cookie_name(secure), value, AXIS_DEVICE_MAX_AGE, secure, "Strict");
}

gchar *
axis_web_auth_device_issue(
	AxisWebAuth    *self,
	const AxisUser *account
){
	g_autofree gchar *nonce = axis_random_public_id();
	g_autofree gchar *signed_part = NULL;
	g_autofree gchar *mac = NULL;
	gint64 issued = axis_web_auth_now(self) / G_USEC_PER_SEC;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(account != NULL, NULL);

	signed_part = g_strdup_printf("%" G_GINT64_FORMAT ".%d.%" G_GINT64_FORMAT ".%s", account->id, account->epoch,
		issued, nonce);
	mac = device_mac(self, signed_part);
	devices_add(self, account->id, account->epoch, issued, nonce);
	devices_save(self);
	return g_strdup_printf("%s.%s", signed_part, mac);
}

/* ── The key ────────────────────────────────────────────────────── */

/* Reads exactly AXIS_DEVICE_KEY_BYTES from the regular file @fd into @key:
 * FALSE (and nothing in @key) when it is anything else. */
static gboolean
read_key(
	gint    fd,
	guint8 *key
){
	guint8 buffer[AXIS_DEVICE_KEY_BYTES + 1];
	struct stat st;
	gsize got = 0;

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != AXIS_DEVICE_KEY_BYTES)
		return FALSE;
	while (got < sizeof(buffer)) {
		gssize n = read(fd, buffer + got, sizeof(buffer) - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (gsize)n;
	}
	if (got != AXIS_DEVICE_KEY_BYTES) {
		sodium_memzero(buffer, sizeof(buffer));
		return FALSE;
	}

	memcpy(key, buffer, AXIS_DEVICE_KEY_BYTES);
	sodium_memzero(buffer, sizeof(buffer));
	return TRUE;
}

/* Writes a new random key to a temporary file and renames it over the key
 * file, so that the file is either the old one or the whole new one. */
static gboolean
write_key(
	gint     dir_fd,
	guint8  *key,
	GError **error
){
	guint8 fresh[AXIS_DEVICE_KEY_BYTES];
	gsize done = 0;
	gboolean ok;
	gint saved;
	gint fd;

	randombytes_buf(fresh, sizeof(fresh));
	fd = openat(dir_fd, DEVICE_KEY_TEMP, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "cannot create %s: %s", DEVICE_KEY_TEMP,
			g_strerror(errno));
		sodium_memzero(fresh, sizeof(fresh));
		return FALSE;
	}
	while (done < sizeof(fresh)) {
		gssize n = write(fd, fresh + done, sizeof(fresh) - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (gsize)n;
	}
	errno = 0;
	ok = done == sizeof(fresh) && fsync(fd) == 0;
	saved = errno;
	if (close(fd) != 0 && ok) {
		ok = FALSE;
		saved = errno;
	}
	if (!ok) {
		unlinkat(dir_fd, DEVICE_KEY_TEMP, 0);
		sodium_memzero(fresh, sizeof(fresh));
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "cannot write %s: %s", DEVICE_KEY_TEMP,
			g_strerror(saved != 0 ? saved : EIO));
		return FALSE;
	}
	if (renameat(dir_fd, DEVICE_KEY_TEMP, dir_fd, DEVICE_KEY_FILE) != 0) {
		saved = errno;
		unlinkat(dir_fd, DEVICE_KEY_TEMP, 0);
		sodium_memzero(fresh, sizeof(fresh));
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "cannot rename %s to %s: %s", DEVICE_KEY_TEMP,
			DEVICE_KEY_FILE, g_strerror(saved));
		return FALSE;
	}
	fsync(dir_fd);

	memcpy(key, fresh, sizeof(fresh));
	sodium_memzero(fresh, sizeof(fresh));
	return TRUE;
}

gboolean
axis_web_auth_load_device_key(
	AxisWebAuth  *self,
	gint          dir_fd,
	GError      **error
){
	gboolean known = FALSE;
	gint fd;

	g_return_val_if_fail(self != NULL, FALSE);

	fd = openat(dir_fd, DEVICE_KEY_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0) {
		known = read_key(fd, self->device_key);
		close(fd);
		/* Remembered devices are all that is lost with it. */
		if (!known)
			g_warning("the device key (%s in the data directory) is not %d bytes: making a new one; browsers "
				"that logged in before are no longer remembered", DEVICE_KEY_FILE, AXIS_DEVICE_KEY_BYTES);
	} else if (errno != ENOENT) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "cannot read %s: %s", DEVICE_KEY_FILE,
			g_strerror(errno));
		return FALSE;
	}
	if (!known && !write_key(dir_fd, self->device_key, error))
		return FALSE;

	/* The record of devices is kept beside the key; under a new key the
	 * devices it named are no longer valid, so it starts empty. */
	if (self->device_dir_fd >= 0)
		close(self->device_dir_fd);
	self->device_dir_fd = fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);
	g_hash_table_remove_all(self->devices);
	if (known)
		devices_load(self, dir_fd);
	else
		devices_save(self);
	return TRUE;
}

/* ── The forwarding hint ────────────────────────────────────────── */

void
axis_web_auth_note_forwarded(
	AxisWebAuth *self,
	const gchar *peer
){
	AxisIpAddress address;
	g_autofree gchar *hint = NULL;

	if (self->forwarding_mixed || peer == NULL)
		return;
	if (self->forwarding_peer == NULL) {
		if (!axis_ip_address_parse(peer, &address) || !axis_ip_address_is_private(&address)) {
			self->forwarding_mixed = TRUE;
			return;
		}
		self->forwarding_peer = g_strdup(peer);
	} else if (strcmp(self->forwarding_peer, peer) != 0) {
		self->forwarding_mixed = TRUE;
		return;
	}

	if (self->forwarding_seen < G_MAXUINT)
		self->forwarding_seen++;
	if (self->forwarding_logged || self->forwarding_seen < AXIS_FORWARDING_HINT_REQUESTS)
		return;

	self->forwarding_logged = TRUE;
	hint = axis_web_auth_forwarding_hint(self);
	if (hint != NULL)
		g_warning("%s", hint);
}

gchar *
axis_web_auth_forwarding_hint(AxisWebAuth *self)
{
	g_autofree gchar *forwarded = NULL;
	g_autofree gchar *refused = NULL;
	AxisWebLimits *limits;

	g_return_val_if_fail(self != NULL, NULL);

	if (!self->forwarding_mixed && self->forwarding_peer != NULL &&
	    self->forwarding_seen >= AXIS_FORWARDING_HINT_REQUESTS && axis_proxy_list_get_length(self->trusted) == 0)
		forwarded = g_strdup_printf("Every request so far came through one private address, %s, with an "
			"X-Forwarded-For header, and no reverse proxy is trusted. If that address is your reverse proxy, Axis "
			"sees every visitor as that one address, so they share one login throttle and one set of limits. Set "
			"AXIS_TRUSTED_PROXIES (trusted_proxies in the configuration file) to it and restart Axis.",
			self->forwarding_peer);

	/* What the connection limits refused says the same from another side. */
	limits = self->web != NULL ? axis_web_get_limits(self->web) : NULL;
	if (limits != NULL)
		refused = axis_web_limits_describe_refused(limits);

	if (forwarded != NULL && refused != NULL)
		return g_strconcat(forwarded, " ", refused, NULL);
	if (forwarded != NULL)
		return g_steal_pointer(&forwarded);
	return g_steal_pointer(&refused);
}
