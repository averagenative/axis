/*
 * axis-library-hand.c - what a person did by hand to a library file's link
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-library-hand.h"

gchar *
axis_library_hand_how(const AxisLibraryHandHow *how)
{
	GString *text = g_string_new(NULL);

	if (how->other != NULL) {
		g_string_append_printf(text, "Linked to %s", how->other);
	} else if (!how->linked) {
		if (how->unlinked_hand)
			g_string_append_printf(text, "Unlinked by hand%s%s%s%s: left alone until a person links it or lets Axis manage "
				"it again", how->unlinked_by != NULL ? " by " : "", how->unlinked_by != NULL ? how->unlinked_by : "",
				how->unlinked_on != NULL ? " on " : "", how->unlinked_on != NULL ? how->unlinked_on : "");
		else
			g_string_append(text, "No link");
	} else {
		if (how->hand_how != NULL)
			g_string_append_printf(text, "%s%s%s%s%s", g_strcmp0(how->hand_how, "forced") == 0 ?
				"From a release grabbed by force" : g_strcmp0(how->hand_how, "assigned") == 0 ? "Assigned by hand" :
				g_strcmp0(how->hand_how, "brought") != 0 ? "Linked by hand" :
				g_strcmp0(how->brought_from, "Sonarr") == 0 ? "Migrated from Sonarr" : "Migrated from Radarr",
				how->hand_by != NULL ? " by " : "", how->hand_by != NULL ? how->hand_by : "",
				how->hand_on != NULL ? " on " : "", how->hand_on != NULL ? how->hand_on : "");
		else
			g_string_append(text, how->by_import ? "Linked by an import" : how->by_identity ?
				"Linked by identification" : "Linked by Axis");
		/* A file brought over is kept from the start, by no person's choice
		 * (import-from-arr): said as what Axis does, not as "by hand".  One a
		 * person keeps by hand since is said as any file kept by hand. */
		if (how->kept && how->kept_brought && g_strcmp0(how->hand_how, "brought") == 0)
			g_string_append(text, "; Axis does not replace it");
		else if (how->kept)
			g_string_append(text, "; kept by hand");
	}
	return g_string_free(text, FALSE);
}

gboolean
axis_library_hand_unlink_mark(
	AxisDbConn   *conn,
	gint64        file_id,
	const gchar  *module,
	gint64        owner_id,
	const gchar  *by,
	GError      **error
){
	g_autofree gchar *file = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	g_autofree gchar *owner = g_strdup_printf("%" G_GINT64_FORMAT, owner_id);
	const gchar *params[4];

	params[0] = file;
	params[1] = module;
	params[2] = owner;
	params[3] = by;
	return axis_db_conn_exec(conn, "INSERT INTO library_hand_unlink (file_id, module, owner_id, by_user)"
		" VALUES ($1::bigint, $2, $3::bigint, $4) ON CONFLICT (file_id) DO UPDATE"
		" SET module = EXCLUDED.module, owner_id = EXCLUDED.owner_id, by_user = EXCLUDED.by_user,"
		" created_at = now()", 4, params, NULL, error);
}

gboolean
axis_library_hand_unlink_clear(
	AxisDbConn   *conn,
	gint64        file_id,
	GError      **error
){
	g_autofree gchar *file = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	const gchar *params[1];

	params[0] = file;
	return axis_db_conn_exec(conn, "DELETE FROM library_hand_unlink WHERE file_id = $1::bigint", 1, params, NULL, error);
}
