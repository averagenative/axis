/*
 * axis-compat-notify-jellyfin.c - telling Jellyfin which folders changed
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Jellyfin's "media updated" request, POST /Library/Media/Updated, is the one
 * write a media-server connection can make.  It is pinned in core/axis-http.h
 * (the method, the exact path, a body built there from a fixed shape, a
 * capped number of folders, never followed on a redirect); this file only
 * hands it the folders and the key, and is the one place in src/compat/ that
 * calls it (tests/test-http.c reads the sources to prove it).
 */

#include "compat/axis-compat-notify.h"

#include <string.h>

#include "compat/axis-compat-private.h"
#include "jobs/axis-job-error.h"

static AxisHttpRescanType
rescan_type(gint change)
{
	switch ((AxisCompatChange)change) {
	case AXIS_COMPAT_CHANGE_CREATED:
		return AXIS_HTTP_RESCAN_CREATED;
	case AXIS_COMPAT_CHANGE_DELETED:
		return AXIS_HTTP_RESCAN_DELETED;
	default:
		return AXIS_HTTP_RESCAN_MODIFIED;
	}
}

gboolean
axis_compat_jellyfin_notify(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                  *items,
	GError                    **error
){
	AxisHttpConfig config;
	guint start;

	memset(&config, 0, sizeof(config));
	config.origin = &connection->address;
	config.auth_header = connection->kind->auth_header;
	config.auth_value = connection->auth_value;
	config.token = connection->token;
	config.accept_untrusted_certificate = connection->untrusted;
	config.timeout_ms = self->tuning.request_timeout_ms;
	config.max_bytes = self->tuning.max_body_bytes;
	config.cancellable = cancellable;
	/* Bound only with outbound.bind_scope = all (core/axis-outbound.h). */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER;
	/* One request for every folder of the batch, as many as the request may
	 * hold at a time. */
	for (start = 0; start < items->len; start += AXIS_HTTP_RESCAN_MAX_UPDATES) {
		g_autofree AxisHttpRescanUpdate *updates = g_new0(AxisHttpRescanUpdate, AXIS_HTTP_RESCAN_MAX_UPDATES);
		g_autoptr(GError) local = NULL;
		guint n = MIN(items->len - start, (guint)AXIS_HTTP_RESCAN_MAX_UPDATES);
		guint i;
		gboolean ok;

		for (i = 0; i < n; i++) {
			const AxisCompatNotifyItem *item = g_ptr_array_index(items, start + i);

			updates[i].path = item->server_folder;
			updates[i].type = rescan_type(item->change);
		}
		ok = axis_http_post_rescan(&config, updates, n, NULL, &local);
		for (i = 0; i < n; i++) {
			AxisCompatNotifyItem *item = g_ptr_array_index(items, start + i);

			if (ok) {
				item->result = AXIS_COMPAT_NOTIFY_SENT;
			} else if (local->domain == AXIS_HTTP_ERROR && local->code == AXIS_HTTP_ERROR_CANCELLED) {
				g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
				return FALSE;
			} else {
				item->result = AXIS_COMPAT_NOTIFY_FAILED;
				item->note = g_strdup(local->message);
			}
		}
	}
	return TRUE;
}
