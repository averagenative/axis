/*
 * axis-compat-kind.c - the kinds of media server: what each is asked and how
 * its answers are read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The collection jobs know a server only through AxisCompatKind
 * (axis-compat-private.h).  Everything that names an endpoint, a header or a
 * field of one server's answers is in this file and in the parser file of
 * that server (axis-compat-plex.c, axis-compat-jellyfin.c).  Only GET
 * requests are made here; telling a server which folder changed is in
 * axis-compat-notify-plex.c and axis-compat-notify-jellyfin.c.
 */

#include "compat/axis-compat-private.h"

#include <string.h>

#include "compat/axis-compat-jellyfin.h"
#include "compat/axis-compat-plex.h"

/* ── Plex ───────────────────────────────────────────────────────── */

static gboolean
plex_identity(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	AxisCompatIdentity         *identity,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/identity", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	if (!axis_compat_plex_parse_identity(data, length, identity, &problem)) {
		axis_compat_set_server_error(error, "the identity request", problem);
		return FALSE;
	}
	if (identity->machine_id == NULL) {
		axis_compat_set_server_error(error, "the identity request", "it does not say which server it is");
		return FALSE;
	}
	/* The name is the root document's: its absence is not a failure. */
	g_clear_pointer(&body, g_bytes_unref);
	if (axis_compat_fetch(self, connection, "/", NULL, cancellable, &body, NULL)) {
		data = g_bytes_get_data(body, &length);
		axis_compat_plex_parse_root(data, length, identity, NULL);
	}
	return TRUE;
}

static gboolean
plex_libraries(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **libraries,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/library/sections", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*libraries = axis_compat_plex_parse_libraries(data, length, &problem);
	if (*libraries == NULL) {
		axis_compat_set_server_error(error, "the libraries request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
plex_devices(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **devices,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/devices", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*devices = axis_compat_plex_parse_devices(data, length, &problem);
	if (*devices == NULL) {
		axis_compat_set_server_error(error, "the devices request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
plex_sessions(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **sessions,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/status/sessions", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*sessions = axis_compat_plex_parse_sessions(data, length, &problem);
	if (*sessions == NULL) {
		axis_compat_set_server_error(error, "the sessions request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
plex_item_file(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	const gchar                *item_id,
	gchar                     **file,
	GError                    **error
){
	g_autofree gchar *path = g_strdup_printf("/library/metadata/%s", item_id);
	g_autoptr(GBytes) body = NULL;
	AxisCompatItem item;
	gsize length;
	const gchar *data;

	*file = NULL;
	if (!axis_compat_fetch(self, connection, path, NULL, cancellable, &body, error))
		return FALSE;
	memset(&item, 0, sizeof(item));
	data = g_bytes_get_data(body, &length);
	if (axis_compat_plex_parse_item(data, length, &item, NULL))
		*file = g_steal_pointer(&item.file);
	axis_compat_item_clear(&item);
	return TRUE;
}

/* ── Jellyfin ───────────────────────────────────────────────────── */

/* The secret goes between double quotes in the header's value. */
static gboolean
jellyfin_secret_usable(const gchar *secret)
{
	return strpbrk(secret, "\"\\") == NULL;
}

static gboolean
jellyfin_identity(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	AxisCompatIdentity         *identity,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	/* The key is needed for this one (the public description is not asked
	 * for), so a refused key is found out by the test. */
	if (!axis_compat_fetch(self, connection, "/System/Info", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	if (!axis_compat_jellyfin_parse_identity(data, length, identity, &problem)) {
		axis_compat_set_server_error(error, "the identity request", problem);
		return FALSE;
	}
	if (identity->machine_id == NULL) {
		axis_compat_set_server_error(error, "the identity request", "it does not say which server it is");
		return FALSE;
	}
	return TRUE;
}

static gboolean
jellyfin_libraries(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **libraries,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/Library/VirtualFolders", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*libraries = axis_compat_jellyfin_parse_libraries(data, length, &problem);
	if (*libraries == NULL) {
		axis_compat_set_server_error(error, "the libraries request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
jellyfin_devices(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **devices,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/Devices", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*devices = axis_compat_jellyfin_parse_devices(data, length, &problem);
	if (*devices == NULL) {
		axis_compat_set_server_error(error, "the devices request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
jellyfin_sessions(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                 **sessions,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	gsize length;
	const gchar *data;

	if (!axis_compat_fetch(self, connection, "/Sessions", NULL, cancellable, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*sessions = axis_compat_jellyfin_parse_sessions(data, length, &problem);
	if (*sessions == NULL) {
		axis_compat_set_server_error(error, "the sessions request", problem);
		return FALSE;
	}
	return TRUE;
}

static gboolean
jellyfin_item_file(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	const gchar                *item_id,
	gchar                     **file,
	GError                    **error
){
	g_autofree gchar *path = g_strdup_printf("/Items?Ids=%s&Fields=Path", item_id);
	g_autoptr(GBytes) body = NULL;
	AxisCompatItem item;
	gsize length;
	const gchar *data;

	*file = NULL;
	if (!axis_compat_fetch(self, connection, path, NULL, cancellable, &body, error))
		return FALSE;
	memset(&item, 0, sizeof(item));
	data = g_bytes_get_data(body, &length);
	if (axis_compat_jellyfin_parse_item(data, length, &item, NULL))
		*file = g_steal_pointer(&item.file);
	axis_compat_item_clear(&item);
	return TRUE;
}

/* ── The table ──────────────────────────────────────────────────── */

static const AxisCompatKind kinds[] = {
	{
		.name = AXIS_COMPAT_KIND_PLEX,
		.label = "Plex",
		.product = "Plex Media Server",
		.address_setting = AXIS_COMPAT_SETTING_ADDRESS,
		.token_setting = AXIS_COMPAT_SETTING_TOKEN,
		.untrusted_setting = AXIS_COMPAT_SETTING_UNTRUSTED_TLS,
		.notify_setting = AXIS_COMPAT_SETTING_NOTIFY_PLEX,
		.auth_header = "X-Plex-Token",
		.has_history = TRUE,
		.library_from_path = FALSE,
		.fetch_identity = plex_identity,
		.fetch_libraries = plex_libraries,
		.fetch_devices = plex_devices,
		.fetch_sessions = plex_sessions,
		.fetch_item_file = plex_item_file,
		.notify = axis_compat_plex_notify
	},
	{
		.name = AXIS_COMPAT_KIND_JELLYFIN,
		.label = "Jellyfin",
		.product = "Jellyfin Server",
		.address_setting = AXIS_COMPAT_JELLYFIN_ADDRESS,
		.token_setting = AXIS_COMPAT_JELLYFIN_TOKEN,
		.untrusted_setting = AXIS_COMPAT_JELLYFIN_UNTRUSTED_TLS,
		.notify_setting = AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN,
		.auth_header = "Authorization",
		.auth_prefix = "MediaBrowser Token=\"",
		.auth_suffix = "\"",
		.has_history = FALSE,
		.library_from_path = TRUE,
		.secret_usable = jellyfin_secret_usable,
		.fetch_identity = jellyfin_identity,
		.fetch_libraries = jellyfin_libraries,
		.fetch_devices = jellyfin_devices,
		.fetch_sessions = jellyfin_sessions,
		.fetch_item_file = jellyfin_item_file,
		.notify = axis_compat_jellyfin_notify
	}
};

const AxisCompatKind *
axis_compat_kind_at(guint index)
{
	return index < G_N_ELEMENTS(kinds) ? &kinds[index] : NULL;
}

const AxisCompatKind *
axis_compat_kind_find(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(kinds); i++) {
		if (strcmp(kinds[i].name, name) == 0)
			return &kinds[i];
	}
	return NULL;
}
