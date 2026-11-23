/*
 * axis-web-browser-dashboard.c - what needs attention in the library
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The library's attention sources (add-attention-notices, slice N2): what
 * the dashboard's card "Needs attention" and the page of that name say of
 * the files.  These were lines a dashboard part worked out on every view of
 * the dashboard, by asking the file list for the count of each list; they
 * are read in the background now, each by one statement over a partial index
 * that holds only the rows counted, never the index of files as a whole:
 *
 *   library.unread    files Axis could not read, or that took too long to
 *                     read (media_info_failed_idx: the rows whose status is
 *                     not 'ok')
 *   library.missing   files the last complete scan did not find, and files
 *                     missing (library_file_absent_idx: the rows whose state
 *                     is not 'present')
 *   library.unscanned library folders of which a scan has ended and none ever
 *                     ended complete; not one whose first scan has not
 *                     ended, nor one on a disk that is away, and "not known
 *                     yet" while the only such folders are being scanned
 *                     again (unscanned_source())
 *
 * All three are "Worth a look": the library works meanwhile.  Each count is
 * of the rows, not of the list its link opens under that list's own filters,
 * so the two can differ by the files of a folder that was since removed.
 */

#include "web/axis-web-browser-private.h"

#include <string.h>

#include "library/axis-library.h"
#include "web/axis-web-attention.h"

static gchar *
count_words(
	gint64       n,
	const gchar *one,
	const gchar *many
){
	return n == 1 ? g_strdup_printf("1 %s", one) : g_strdup_printf("%" G_GINT64_FORMAT " %s", n, many);
}

/* Nothing says when a file was last found unreadable, so "newest" is the
 * highest file among them: a file that fails for the first time is a new
 * occurrence unless it is an older one. */
static gboolean
unread_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebBrowser *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *failed_text = NULL;
	g_autofree gchar *timeout_text = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 failed = 0;
	gint64 timeout = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(axis_storage_get_pool(self->storage),
	        "SELECT count(*) FILTER (WHERE status = 'failed'), count(*) FILTER (WHERE status = 'timeout'),"
	        " coalesce(max(file_id), 0) FROM media_info WHERE status <> 'ok'", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &failed);
	axis_db_result_get_int64(result, 0, 1, &timeout);
	axis_db_result_get_int64(result, 0, 2, &newest);
	if (failed + timeout <= 0)
		return TRUE;
	failed_text = count_words(failed, "file could not be read", "files could not be read");
	timeout_text = count_words(timeout, "file took too long to read", "files took too long to read");
	text = g_strdup_printf("%s%s%s. Axis knows nothing of what such a file holds until it is read again.",
		failed > 0 ? failed_text : "", failed > 0 && timeout > 0 ? " and " : "", timeout > 0 ? timeout_text : "");
	item = axis_attention_item_new("library.unread", "", AXIS_ATTENTION_LOOK, (guint64)(failed + timeout), newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, failed > 0 ? "/library/files?probe=failed" : "/library/files?probe=timeout",
		"Open the files");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* A file not found is removed from the index by itself after its keeping
 * time, so the notice ends without anyone; it is shown because files that
 * went from a disk are worth a person's look while they can still act. */
static gboolean
missing_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebBrowser *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *suspected_text = NULL;
	g_autofree gchar *missing_text = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 suspected = 0;
	gint64 missing = 0;

	if (!axis_db_pool_query_bounded(axis_storage_get_pool(self->storage),
	        "SELECT count(*) FILTER (WHERE state = 'suspected'), count(*) FILTER (WHERE state = 'missing')"
	        " FROM library_file WHERE state <> 'present'", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &suspected);
	axis_db_result_get_int64(result, 0, 1, &missing);
	if (suspected + missing <= 0)
		return TRUE;
	suspected_text = count_words(suspected, "file was not found by the last complete scan",
		"files were not found by the last complete scan");
	missing_text = count_words(missing, "file is missing", "files are missing");
	text = g_strdup_printf("%s%s%s.", suspected > 0 ? suspected_text : "", suspected > 0 && missing > 0 ? " and " : "",
		missing > 0 ? missing_text : "");
	/* No id rises with them (a file is found missing long after it was
	 * indexed): "newest" is 0, and more of them is what shows it again. */
	item = axis_attention_item_new("library.missing", "", AXIS_ATTENTION_LOOK, (guint64)(suspected + missing), 0);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, suspected > 0 ? "/library/files?state=suspected" : "/library/files?state=missing",
		"Open the files");
	g_ptr_array_add(items, item);
	return TRUE;
}

static gboolean
unscanned_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebBrowser *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;
	gint64 newest = 0;
	gint64 unsure = 0;

	/*
	 * A folder counts once a scan of it has ended and none ever ended
	 * complete.  Of a folder that was never completely scanned, three things
	 * are not that, and not "scanned" either ("unsure"): its first scan has
	 * not ended (none was begun yet, one is queued, one is running: no notice
	 * for a folder Axis is only now reading); a scan of it is queued or
	 * running again (it may end complete); its disk is away (that disk has
	 * its own notice, and a scan that ended "unavailable" says nothing of the
	 * folder).  The scan's job is its disk's (AXIS_LIBRARY_SCAN_KIND
	 * ":volume:<id>"), found by its key.
	 */
	if (!axis_db_pool_query_bounded(axis_storage_get_pool(self->storage),
	        "SELECT count(*) FILTER (WHERE NOT u.unsure), coalesce(max(u.id) FILTER (WHERE NOT u.unsure), 0),"
	        " count(*) FILTER (WHERE u.unsure)"
	        " FROM (SELECT r.id, (ls.id IS NULL OR ls.status = 'running' OR v.state IN ('offline', 'mismatch')"
	        "   OR EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_LIBRARY_SCAN_KIND ":volume:' || r.volume_id"
	        "    AND j.state IN ('queued', 'running'))) AS unsure"
	        "  FROM library_root r JOIN volume v ON v.id = r.volume_id"
	        "  LEFT JOIN library_root_state s ON s.root_id = r.id"
	        "  LEFT JOIN library_scan ls ON ls.id = s.last_scan_id"
	        "  WHERE s.last_complete_scan_id IS NULL) u", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &n);
	axis_db_result_get_int64(result, 0, 1, &newest);
	axis_db_result_get_int64(result, 0, 2, &unsure);
	if (n <= 0) {
		/* None for certain, and some that cannot be said yet: the notice
		 * stays as it was (there, dismissed, or not there at all) rather than
		 * ending for the length of a scan and coming back undismissed. */
		if (unsure > 0)
			axis_attention_not_known(items, "library.unscanned", NULL);
		return TRUE;
	}
	text = n == 1 ? g_strdup("1 library folder was never completely scanned: Axis does not know all of its files.") :
		g_strdup_printf("%" G_GINT64_FORMAT " library folders were never completely scanned: Axis does not know all "
			"of their files.", n);
	item = axis_attention_item_new("library.unscanned", "", AXIS_ATTENTION_LOOK, (guint64)n, newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/library", "Open Library");
	g_ptr_array_add(items, item);
	return TRUE;
}

void
axis_web_browser_register_dashboard(
	AxisWebBrowser *self,
	AxisWeb        *web
){
	axis_web_add_attention_source(web, "the files Axis could not read", unread_source, self);
	axis_web_add_attention_source(web, "the files that were not found", missing_source, self);
	axis_web_add_attention_source(web, "the library folders' scans", unscanned_source, self);
}
