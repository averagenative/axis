/*
 * axis-import-assign.c - assigning the files of a download by hand: an
 * administrator says which video file of an import is the movie, or which
 * episodes each video file holds, and the import takes them as that
 * (add-force-grab-and-assign, slice B)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The review is the import's own decision, made here on the files as the
 * import's last run recorded them (import_file) with the assignment put in
 * and every tick counted as given: what a run would import, under what name,
 * and what it would replace.  The ticks it needs are then asked of the
 * person.  So the review and the import cannot disagree, and the file system
 * is not touched to show it.
 *
 * Applying it writes the assignment (import_assignment) and queues the
 * import's job, in one transaction with the import and the title locked.  The
 * run then decides an assigned file as it decides the file of a grab by
 * force (axis-import-decide.c: bound to what the person named, the profile
 * set aside, a library file replaced only with its tick), and leaves every
 * other file of the download alone.  No file is deleted, and none is replaced
 * without its tick: a file that was not ticked blocks the import, and "import
 * anyway" does not override that.
 */

#include "import/axis-import-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-library-name.h"

/* ── Records ────────────────────────────────────────────────────── */

AxisImportAssignAsk *
axis_import_assign_ask_new(void)
{
	AxisImportAssignAsk *ask = g_new0(AxisImportAssignAsk, 1);

	ask->positions = g_array_new(FALSE, FALSE, sizeof(gint));
	ask->said = g_ptr_array_new_with_free_func(g_free);
	ask->runtime_ok = g_array_new(FALSE, FALSE, sizeof(gint));
	ask->replace = g_array_new(FALSE, FALSE, sizeof(gint64));
	return ask;
}

void
axis_import_assign_ask_free(AxisImportAssignAsk *ask)
{
	if (ask == NULL)
		return;
	g_array_unref(ask->positions);
	g_ptr_array_unref(ask->said);
	g_array_unref(ask->runtime_ok);
	g_array_unref(ask->replace);
	g_free(ask);
}

static void
replace_free(AxisImportAssignReplace *replace)
{
	g_free(replace->path);
	g_free(replace->quality);
	g_free(replace);
}

static void
file_free(AxisImportAssignFile *file)
{
	g_free(file->path);
	g_free(file->read);
	g_free(file->said);
	g_free(file->label);
	g_free(file->name);
	g_free(file->quality);
	g_clear_pointer(&file->replaces, g_ptr_array_unref);
	g_free(file->problem);
	g_free(file);
}

void
axis_import_assignment_free(AxisImportAssignment *assignment)
{
	if (assignment == NULL)
		return;
	g_free(assignment->module);
	g_free(assignment->title_label);
	g_free(assignment->state);
	g_clear_pointer(&assignment->files, g_ptr_array_unref);
	g_free(assignment->refused);
	g_free(assignment->missing);
	g_free(assignment->digest);
	g_free(assignment);
}

void
axis_import_assigned_free(AxisImportAssigned *assigned)
{
	if (assigned == NULL)
		return;
	g_free(assigned->path);
	g_clear_pointer(&assigned->ids, g_array_unref);
	g_clear_pointer(&assigned->replaces, g_array_unref);
	g_strfreev(assigned->replaces_quality);
	g_free(assigned);
}

gboolean
axis_import_assigned_load(
	AxisImport   *self,
	AxisDbConn   *conn,
	gint64        import_id,
	GPtrArray   **assigned,
	gchar       **by_user,
	GError      **error
){
	static const gchar sql[] = "SELECT position, path, array_to_string(episode_ids, ','),"
		" array_to_string(replaces, ','), array_to_string(replaces_quality, ','), runtime_ok, by_user"
		" FROM import_assignment WHERE import_id = $1::bigint ORDER BY position";
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	guint i;

	*assigned = NULL;
	if (by_user != NULL)
		*by_user = NULL;
	params[0] = id;
	if (!(conn != NULL ? axis_db_conn_exec(conn, sql, 1, params, &rows, error) :
	      axis_db_pool_exec(self->pool, sql, 1, params, &rows, error)))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	*assigned = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_assigned_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportAssigned *a = g_new0(AxisImportAssigned, 1);
		const gchar *qualities = axis_db_result_get_text(rows, i, 4);
		gint64 position = 0;

		axis_db_result_get_int64(rows, i, 0, &position);
		a->position = (gint)position;
		a->path = g_strdup(axis_db_result_get_text(rows, i, 1));
		a->ids = axis_import_file_ids(axis_db_result_get_text(rows, i, 2));
		a->replaces = axis_import_file_ids(axis_db_result_get_text(rows, i, 3));
		a->replaces_quality = g_strsplit(qualities != NULL ? qualities : "", ",", 0);
		axis_db_result_get_boolean(rows, i, 5, &a->runtime_ok);
		g_ptr_array_add(*assigned, a);
		if (by_user != NULL && *by_user == NULL)
			*by_user = g_strdup(axis_db_result_get_text(rows, i, 6));
	}
	return TRUE;
}

/* ── The review ─────────────────────────────────────────────────── */

static gboolean
has_int(
	const GArray *array,
	gint          value
){
	guint i;

	for (i = 0; array != NULL && i < array->len; i++) {
		if (g_array_index(array, gint, i) == value)
			return TRUE;
	}
	return FALSE;
}

static gboolean
has_id(
	const GArray *array,
	gint64        value
){
	guint i;

	for (i = 0; array != NULL && i < array->len; i++) {
		if (g_array_index(array, gint64, i) == value)
			return TRUE;
	}
	return FALSE;
}

/* A file of the import as the decision reads one, from its row: its place
 * and name, and what the probe read of it. */
static AxisImportFound *
found_of_row(
	AxisDbResult *rows,
	guint         row,
	const gchar  *source_path
){
	AxisImportFound *f = g_new0(AxisImportFound, 1);
	const gchar *path = axis_db_result_get_text(rows, row, 1);
	const gchar *kind = axis_db_result_get_text(rows, row, 2);
	const gchar *reasons = axis_db_result_get_text(rows, row, 11);
	g_autoptr(GString) bytes = NULL;
	const gchar *slash;
	gint64 value = 0;

	axis_db_result_get_int64(rows, row, 0, &value);
	f->position = (gint)value;
	f->path = g_strdup(path != NULL ? path : "");
	/* A download that is one file: its name is the source's last part. */
	if (!axis_library_name_decode(f->path[0] != '\0' ? f->path : source_path, &bytes) || bytes == NULL)
		bytes = g_string_new(f->path);
	slash = strrchr(bytes->str, '/');
	f->name = g_strdup(slash != NULL ? slash + 1 : bytes->str);
	f->dir = f->path[0] != '\0' && slash != NULL ? g_strndup(bytes->str, (gsize)(slash - bytes->str)) : g_strdup("");
	f->type = g_strcmp0(kind, "link") == 0 ? AXIS_IMPORT_FOUND_LINK : g_strcmp0(kind, "special") == 0 ?
		AXIS_IMPORT_FOUND_SPECIAL : AXIS_IMPORT_FOUND_FILE;
	f->kind = g_strcmp0(kind, "video") == 0 ? AXIS_IMPORT_FILE_VIDEO : g_strcmp0(kind, "subtitle") == 0 ?
		AXIS_IMPORT_FILE_SUBTITLE : g_strcmp0(kind, "archive") == 0 ? AXIS_IMPORT_FILE_ARCHIVE : AXIS_IMPORT_FILE_OTHER;
	value = 0;
	axis_db_result_get_int64(rows, row, 3, &value);
	f->size = (guint64)MAX(value, 0);
	value = 0;
	axis_db_result_get_int64(rows, row, 4, &value);
	f->inode = (guint64)MAX(value, 0);
	f->folder = AXIS_IMPORT_FOLDER_PLAIN;
	f->unit_of = -1;
	if (!axis_db_result_is_null(rows, row, 5)) {
		f->probed = TRUE;
		f->probe_ok = TRUE;
		axis_db_result_get_int64(rows, row, 5, &f->duration_ms);
		value = 0;
		axis_db_result_get_int64(rows, row, 6, &value);
		f->width = (gint)value;
		value = 0;
		axis_db_result_get_int64(rows, row, 7, &value);
		f->height = (gint)value;
		/* The run found no picture in it: its row says so. */
		f->video_streams = reasons != NULL && strstr(reasons, "\"no_video_stream\"") != NULL ? 0 : 1;
		f->video_codec = g_strdup(axis_db_result_get_text(rows, row, 8));
	}
	return f;
}

/* "S01E03 S01E04", as a person writes what a file holds. */
static gchar *
unit_said(const AxisImportUnit *unit)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < unit->n_episodes; i++)
		g_string_append_printf(out, "%sS%02dE%02d", i > 0 ? " " : "", unit->season, unit->episodes[i]);
	return g_string_free(out, FALSE);
}

/* The name the file gets in the library, from what is known before it is
 * imported (what only its probe at import reads, its sound and HDR, is not
 * in it). */
static gchar *
name_of(
	const AxisImportSettings *settings,
	const AxisImportTarget   *target,
	const AxisImportFound    *f,
	const AxisRelease        *release,
	const AxisRelease        *file,
	AxisQuality               quality,
	gboolean                  proper,
	const AxisImportUnit     *unit
){
	g_autofree gchar *ext = axis_import_extension(f->name, strlen(f->name));
	AxisImportNaming naming;
	AxisImportName name;
	gchar *result = NULL;

	memset(&naming, 0, sizeof(naming));
	memset(&name, 0, sizeof(name));
	naming.rename = settings->rename;
	naming.layout = settings->layout;
	naming.format = settings->format;
	naming.title = target->title;
	naming.year = target->year;
	naming.tmdb_id = target->tmdb_id;
	naming.imdb_id = target->imdb_id;
	naming.release = release;
	naming.file = file;
	naming.file_name = f->name;
	naming.extension = ext;
	naming.quality = quality;
	naming.proper = proper;
	naming.video_codec = f->video_codec;
	if (unit != NULL) {
		naming.episode = TRUE;
		naming.tvdb_id = target->tvdb_id;
		naming.season = unit->season;
		naming.episodes = unit->episodes;
		naming.n_episodes = unit->n_episodes;
		naming.episode_title = unit->episode_title;
		naming.air_date = unit->air_date;
	}
	if (axis_import_names_for(&naming, &name, NULL))
		result = g_strdup(name.file);
	axis_import_name_clear(&name);
	return result;
}

/* What @f would replace, with the person's ticks. */
static void
replaces_of(
	AxisImportAssignFile      *file,
	const GPtrArray           *existing,
	AxisQuality                quality,
	const AxisImportAssignAsk *ask,
	const GArray              *placed,
	const gchar               *label
){
	guint i;

	for (i = 0; existing != NULL && i < existing->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(existing, i);
		AxisImportAssignReplace *r = g_new0(AxisImportAssignReplace, 1);
		gint compare = axis_quality_compare_on_disk(quality, AXIS_RELEASE_RESOLUTION_NONE, e->quality, e->resolution);

		r->file_id = e->library_file_id;
		r->path = axis_library_name_display(e->rel_path != NULL ? e->rel_path : "");
		r->quality = g_strdup(e->quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(e->quality) :
			"unknown quality");
		r->compare = compare > 0 ? 1 : compare < 0 ? -1 : 0;
		r->kept = e->kept;
		r->ticked = ask != NULL && has_id(ask->replace, e->library_file_id);
		g_ptr_array_add(file->replaces, r);
		/* A file this very import placed is not replaced by another of its
		 * files. */
		if (has_id(placed, e->library_file_id) && file->problem == NULL)
			file->problem = g_strdup_printf("%s already got a file from this download, in this import.", label);
	}
}

static gint
compare_assigned(
	gconstpointer a,
	gconstpointer b
){
	const AxisImportAssigned *x = *(const AxisImportAssigned *const *)a;
	const AxisImportAssigned *y = *(const AxisImportAssigned *const *)b;

	return x->position < y->position ? -1 : x->position > y->position;
}

static const gchar *
last_reason(const AxisImportFound *f)
{
	if (f->reasons == NULL || f->reasons->len == 0)
		return NULL;
	return ((const AxisImportReason *)g_ptr_array_index(f->reasons, f->reasons->len - 1))->text;
}

/*
 * The review of @import_id with @ask, read on @conn.  @target and @store:
 * (out) (optional) the title and the assignments as they would be written
 * (each with the ticks the review needs, which a ready review has).
 */
static gboolean
review(
	AxisImport                 *self,
	AxisDbConn                 *conn,
	gint64                      import_id,
	const AxisImportAssignAsk  *ask,
	AxisImportAssignment      **out_p,
	AxisImportTarget          **target_out,
	GPtrArray                 **store_out,
	GError                    **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autoptr(AxisImportAssignment) out = g_new0(AxisImportAssignment, 1);
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
	g_autoptr(GPtrArray) assigned = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_assigned_free);
	g_autoptr(GArray) placed = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) digest = g_string_new("assign");
	g_autoptr(GString) missing = g_string_new(NULL);
	g_autoptr(AxisRelease) grab = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *problem = NULL;
	const AxisImportOwner *owner;
	AxisImportSettings settings;
	AxisImportExpect expect;
	AxisImportVerdict verdict;
	AxisImportUnitsVerdict units;
	const gchar *params[1];
	const gchar *state;
	const gchar *source_path;
	gint64 download_id = 0;
	gint64 owner_id = 0;
	guint i;
	guint k;

	*out_p = NULL;
	if (target_out != NULL)
		*target_out = NULL;
	if (store_out != NULL)
		*store_out = NULL;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT state, module, download_id, owner_id, title_label, source_path, release_name,"
	        " protocol FROM import WHERE id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		return FALSE;
	}
	state = axis_db_result_get_text(row, 0, 0);
	source_path = axis_db_result_get_text(row, 0, 5);
	axis_db_result_get_int64(row, 0, 2, &download_id);
	axis_db_result_get_int64(row, 0, 3, &owner_id);
	out->import_id = import_id;
	out->state = g_strdup(state);
	out->module = g_strdup(axis_db_result_get_text(row, 0, 1));
	out->title_label = g_strdup(axis_db_result_get_text(row, 0, 4));
	out->files = g_ptr_array_new_with_free_func((GDestroyNotify)file_free);
	owner = axis_import_find_owner(self, out->module);
	out->by_file = owner != NULL && owner->info->match != NULL;
	if (g_strcmp0(state, "blocked") != 0 && g_strcmp0(state, "partial") != 0) {
		out->refused = g_strdup_printf("This import is not waiting for a person (%s), so its files cannot be assigned "
			"now.", g_strcmp0(state, "imported") == 0 ? "it was imported" : g_strcmp0(state, "dismissed") == 0 ?
			"it was dismissed" : "it is waiting or being placed");
		*out_p = g_steal_pointer(&out);
		return TRUE;
	}
	if (owner == NULL || (out->by_file && (owner->info->assigned == NULL || owner->info->assign_read == NULL))) {
		out->refused = g_strdup("Nothing in this version of Axis can assign the files of this import.");
		*out_p = g_steal_pointer(&out);
		return TRUE;
	}
	if (!owner->info->resolve(owner->data, conn, download_id, owner_id, &target, &problem, error))
		return FALSE;
	if (target == NULL) {
		out->refused = g_strdup(problem != NULL ? problem : "The title this download was for is not managed any more.");
		*out_p = g_steal_pointer(&out);
		return TRUE;
	}
	g_free(out->title_label);
	out->title_label = g_strdup(target->label);
	if (!axis_db_conn_exec(conn, "SELECT position, path, kind, size, inode, duration_ms, width, height, video_codec,"
	        " library_file_id, step, reasons::text FROM import_file WHERE import_id = $1::bigint ORDER BY position", 1,
	        params, &rows, error))
		return FALSE;
	g_string_append_printf(digest, "|%" G_GINT64_FORMAT "|%s|%" G_GINT64_FORMAT "|%s", import_id, state,
		target->owner_id, target->stamp != NULL ? target->stamp : "");

	/* The files: a video file that no earlier run of this import placed can
	 * be chosen. */
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gboolean taken = !axis_db_result_is_null(rows, i, 9) || !axis_db_result_is_null(rows, i, 10);
		AxisImportFound *f;
		AxisImportAssignFile *file;

		if (!axis_db_result_is_null(rows, i, 9)) {
			gint64 library = 0;

			axis_db_result_get_int64(rows, i, 9, &library);
			g_array_append_val(placed, library);
		}
		if (g_strcmp0(axis_db_result_get_text(rows, i, 2), "video") != 0 && taken)
			continue;
		f = found_of_row(rows, i, source_path);
		if (f->kind != AXIS_IMPORT_FILE_VIDEO || f->type != AXIS_IMPORT_FOUND_FILE) {
			g_ptr_array_add(found, f);
			continue;
		}
		file = g_new0(AxisImportAssignFile, 1);
		file->position = f->position;
		file->path = g_strdup(f->path[0] != '\0' ? f->path : source_path);
		file->size = (gint64)f->size;
		file->duration_ms = f->probe_ok ? f->duration_ms : 0;
		file->imported = taken;
		file->replaces = g_ptr_array_new_with_free_func((GDestroyNotify)replace_free);
		g_string_append_printf(digest, "|V%d:%s:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT ":%d", f->position, f->path,
			f->size, f->inode, taken);
		/* What Axis reads from its name, to begin from. */
		if (out->by_file && !taken) {
			g_autoptr(GString) bytes = g_string_new(f->name);
			g_autofree gchar *ext = axis_import_extension(bytes->str, bytes->len);
			g_autofree gchar *stem = ext != NULL ? g_strndup(bytes->str, bytes->len - strlen(ext)) :
				g_strdup(bytes->str);
			g_autoptr(AxisRelease) parsed = axis_release_parse(stem);
			g_autofree gchar *why = NULL;
			AxisImportUnit *unit = NULL;

			if (owner->info->match(owner->data, target, parsed, f->name, &unit, &why) && unit != NULL)
				file->read = unit_said(unit);
			axis_import_unit_free(unit);
		}
		g_ptr_array_add(out->files, file);
		if (taken)
			axis_import_found_free(f);
		else
			g_ptr_array_add(found, f);
	}
	if (out->files->len == 0) {
		out->refused = g_strdup("Axis has not read any video file of this download yet, so there is nothing to assign. "
			"Retry the import first, so that it reads the files.");
		*out_p = g_steal_pointer(&out);
		return TRUE;
	}

	/* The choices, each read by the owner. */
	for (i = 0; ask != NULL && i < ask->positions->len && out->refused == NULL; i++) {
		gint position = g_array_index(ask->positions, gint, i);
		const gchar *said = i < ask->said->len ? g_ptr_array_index(ask->said, i) : "";
		AxisImportAssignFile *file = NULL;
		AxisImportAssigned *a;
		GArray *ids = NULL;

		for (k = 0; k < out->files->len; k++) {
			AxisImportAssignFile *one = g_ptr_array_index(out->files, k);

			if (one->position == position)
				file = one;
		}
		/* Only a video file of the import that is still to be imported: not
		 * an archive, a link, a subtitle, nor a file placed already. */
		if (file == NULL || file->imported || file->chosen) {
			out->refused = g_strdup(file != NULL && file->imported ? "A file that was chosen is in the library "
				"already, from this import: it cannot be assigned again." : "A file that was chosen is not a video "
				"file of this download. Only its video files can be assigned: an archive, a link or any other file "
				"cannot.");
			break;
		}
		/* A movie is one file: when this download's file is in place already
		 * (published or linked), another cannot be chosen. */
		for (k = 0; !out->by_file && k < out->files->len; k++) {
			const AxisImportAssignFile *one = g_ptr_array_index(out->files, k);

			if (one->imported)
				out->refused = g_strdup("The movie's file from this download is already in place, so another file of it "
					"cannot be chosen.");
		}
		if (out->refused != NULL)
			break;
		file->chosen = TRUE;
		file->said = g_strdup(said);
		file->runtime_ok = has_int(ask->runtime_ok, position);
		out->n_chosen++;
		if (out->by_file) {
			g_autofree gchar *why = NULL;

			if (!owner->info->assign_read(owner->data, target, said, &ids, &why) || ids == NULL || ids->len == 0) {
				file->problem = g_strdup_printf("%s.", why != NULL ? why : "Say which episodes it holds, each as "
					"S01E03, with a space between two");
				g_clear_pointer(&ids, g_array_unref);
				continue;
			}
			/* Two files cannot be given the same episode. */
			for (k = 0; k < assigned->len && file->problem == NULL; k++) {
				const AxisImportAssigned *other = g_ptr_array_index(assigned, k);
				guint n;

				for (n = 0; n < ids->len; n++) {
					if (has_id(other->ids, g_array_index(ids, gint64, n)))
						file->problem = g_strdup("Another file was given the same episode: two files cannot be "
							"assigned to one episode.");
				}
			}
			if (file->problem != NULL) {
				g_array_unref(ids);
				continue;
			}
		} else if (out->n_chosen > 1) {
			out->refused = g_strdup("Choose one file: a movie is one file. The other files are left alone.");
			break;
		}
		a = g_new0(AxisImportAssigned, 1);
		a->position = position;
		for (k = 0; k < found->len; k++) {
			const AxisImportFound *f = g_ptr_array_index(found, k);

			if (f->position == position)
				a->path = g_strdup(f->path);
		}
		a->ids = ids != NULL ? ids : g_array_new(FALSE, FALSE, sizeof(gint64));
		a->replaces = g_array_new(FALSE, FALSE, sizeof(gint64));
		a->runtime_ok = file->runtime_ok;
		a->all_ticks = TRUE;
		g_ptr_array_add(assigned, a);
	}
	for (i = 0; i < out->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(out->files, i);

		if (!file->chosen && !file->imported)
			out->n_left++;
	}
	if (out->refused != NULL || assigned->len == 0) {
		*out_p = g_steal_pointer(&out);
		return TRUE;
	}
	/* In the files' order, whatever order the choices were posted in: the
	 * digest is of the assignment, not of the post. */
	g_ptr_array_sort(assigned, compare_assigned);

	/* The import's own decision, with every tick counted as given: what each
	 * chosen file becomes, and what it would replace. */
	axis_import_settings_read(self, &settings);
	if (target->grab_release != NULL)
		grab = axis_release_parse(target->grab_release);
	else if (axis_db_result_get_text(row, 0, 6) != NULL)
		grab = axis_release_parse(axis_db_result_get_text(row, 0, 6));
	memset(&expect, 0, sizeof(expect));
	memset(&verdict, 0, sizeof(verdict));
	memset(&units, 0, sizeof(units));
	expect.target = target;
	expect.owner = owner->info;
	expect.owner_data = owner->data;
	expect.settings = &settings;
	expect.grab = grab;
	expect.grab_size = -1;
	expect.protocol = AXIS_DOWNLOAD_TORRENT;
	axis_download_protocol_from_string(axis_db_result_get_text(row, 0, 7) != NULL ?
		axis_db_result_get_text(row, 0, 7) : "", &expect.protocol);
	expect.chosen = -1;
	expect.now_us = g_get_real_time();
	expect.assigned = assigned;
	if (out->by_file)
		axis_import_decide_units(&expect, found, 0, &units);
	else
		axis_import_decide(&expect, found, 0, &verdict);
	for (i = 0; i < assigned->len; i++) {
		AxisImportAssigned *a = g_ptr_array_index(assigned, i);
		AxisImportAssignFile *file = NULL;
		const AxisImportFound *f = NULL;
		const GPtrArray *existing = NULL;
		const AxisImportUnit *unit = NULL;
		const AxisRelease *release = grab;
		const AxisRelease *own = NULL;
		AxisQuality quality = AXIS_QUALITY_UNKNOWN;
		gboolean proper = FALSE;
		gboolean decided = FALSE;
		gint runtime = 0;

		for (k = 0; k < out->files->len; k++) {
			AxisImportAssignFile *one = g_ptr_array_index(out->files, k);

			if (one->position == a->position)
				file = one;
		}
		for (k = 0; k < found->len; k++) {
			const AxisImportFound *one = g_ptr_array_index(found, k);

			if (one->position == a->position)
				f = one;
		}
		if (file == NULL || f == NULL)
			continue;
		if (out->by_file) {
			for (k = 0; k < units.plans->len; k++) {
				const AxisImportPlan *plan = g_ptr_array_index(units.plans, k);

				if (!plan->assigned || plan->found != f)
					continue;
				decided = TRUE;
				unit = plan->unit;
				existing = unit->existing;
				quality = plan->quality;
				proper = plan->proper;
				release = plan->fields;
				own = plan->file_release;
				runtime = unit->runtime_minutes;
				file->label = g_strdup(unit->label);
			}
		} else if (verdict.film == f && !verdict.blocked) {
			decided = TRUE;
			existing = target->existing;
			quality = verdict.quality;
			proper = verdict.proper;
			own = verdict.file_release;
			runtime = target->runtime_minutes;
			file->label = g_strdup(target->label);
		}
		if (!decided) {
			const gchar *why = out->by_file ? last_reason(f) : verdict.summary != NULL ? verdict.summary : last_reason(f);

			if (file->problem == NULL)
				file->problem = g_strdup(why != NULL ? why : "It cannot be imported as that.");
			continue;
		}
		file->quality = g_strdup(quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(quality) : "unknown quality");
		file->name = name_of(&settings, target, f, release, own, quality, proper, unit);
		file->expected_ms = (gint64)runtime * 60000;
		file->runtime_off = axis_import_runtime_off(&settings, file->duration_ms, runtime);
		replaces_of(file, existing, quality, ask, placed, file->label);
		g_string_append_printf(digest, "|C%d:%s:%s:%d", a->position, file->label, file->quality, file->runtime_off);
		for (k = 0; k < a->ids->len; k++)
			g_string_append_printf(digest, ",%" G_GINT64_FORMAT, g_array_index(a->ids, gint64, k));
		/* The ticks it needs, and what is written when they are all given. */
		a->replaces_quality = g_new0(gchar *, (existing != NULL ? existing->len : 0) + 1);
		for (k = 0; existing != NULL && k < existing->len; k++) {
			const AxisImportExisting *e = g_ptr_array_index(existing, k);
			const AxisImportAssignReplace *r = g_ptr_array_index(file->replaces, k);

			g_array_append_val(a->replaces, e->library_file_id);
			a->replaces_quality[k] = g_strdup(axis_import_quality_key(e->quality));
			g_string_append_printf(digest, "|R%" G_GINT64_FORMAT ":%s:%d", e->library_file_id,
				axis_import_quality_key(e->quality), e->kept);
			if (!r->ticked)
				g_string_append_printf(missing, "%sTick replacing %s (the file %s has now).", missing->len > 0 ? " " : "",
					r->path, file->label);
		}
		if (file->runtime_off && !file->runtime_ok) {
			g_autofree gchar *shown = axis_library_name_display(file->path);

			g_string_append_printf(missing, "%sTick the runtime of %s.", missing->len > 0 ? " " : "", shown);
		}
		a->all_ticks = FALSE;
	}
	axis_import_verdict_clear(&verdict);
	axis_import_units_verdict_clear(&units);
	axis_import_settings_clear(&settings);

	out->ready = out->n_chosen > 0;
	for (i = 0; i < out->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(out->files, i);

		if (file->chosen && file->problem != NULL)
			out->ready = FALSE;
	}
	if (missing->len > 0) {
		out->missing = g_strdup(missing->str);
		out->ready = FALSE;
	}
	out->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, digest->str, -1);
	if (target_out != NULL)
		*target_out = g_steal_pointer(&target);
	if (store_out != NULL)
		*store_out = g_steal_pointer(&assigned);
	*out_p = g_steal_pointer(&out);
	return TRUE;
}

gboolean
axis_import_assign_preview(
	AxisImport                 *self,
	gint64                      import_id,
	const AxisImportAssignAsk  *ask,
	AxisImportAssignment      **assignment,
	GError                    **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && assignment != NULL, FALSE);

	*assignment = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = review(self, conn, import_id, ask, assignment, NULL, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── Applying it ────────────────────────────────────────────────── */

static gchar *
ids_array(const GArray *ids)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gboolean
axis_import_assign(
	AxisImport                 *self,
	gint64                      import_id,
	const AxisImportAssignAsk  *ask,
	const gchar                *digest,
	const gchar                *user,
	gboolean                   *applied,
	gboolean                   *changed,
	AxisImportAssignment      **assignment,
	GError                    **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autoptr(GPtrArray) store = NULL;
	g_autoptr(GArray) all = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) text = g_string_new("Assigned by hand:");
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *claimed = NULL;
	const AxisImportOwner *owner;
	const gchar *params[8];
	gint64 owner_id = 0;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && ask != NULL && user != NULL && applied != NULL && changed != NULL, FALSE);

	*applied = FALSE;
	*changed = FALSE;
	if (assignment != NULL)
		*assignment = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	/* The import's row first: no run starts, and no other action is taken,
	 * while this holds it. */
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT state, module, owner_id FROM import WHERE id = $1::bigint FOR UPDATE", 1, params,
			&row, error);
	if (!ok)
		goto out;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		ok = FALSE;
		goto out;
	}
	owner = axis_import_find_owner(self, axis_db_result_get_text(row, 0, 1));
	axis_db_result_get_int64(row, 0, 2, &owner_id);
	/* Then the title, before it is read: its files and its grabs do not
	 * change under the review. */
	if (owner != NULL && owner->info->assign_lock != NULL && owner_id > 0 &&
	    !owner->info->assign_lock(owner->data, conn, owner_id, error)) {
		ok = FALSE;
		goto out;
	}
	ok = review(self, conn, import_id, ask, &out, &target, &store, error);
	if (!ok)
		goto out;
	if (out->refused != NULL || target == NULL || store == NULL)
		goto done;
	/* Out of date first: what the person confirmed is not what stands now,
	 * whatever ticks they gave. */
	if (g_strcmp0(out->digest, digest) != 0) {
		*changed = TRUE;
		goto done;
	}
	if (!out->ready)
		goto done;
	for (i = 0; i < store->len; i++) {
		const AxisImportAssigned *a = g_ptr_array_index(store, i);

		g_array_append_vals(all, a->ids->data, a->ids->len);
	}
	if (owner->info->assign_claimed != NULL &&
	    !owner->info->assign_claimed(owner->data, conn, target, all, &claimed, error)) {
		ok = FALSE;
		goto out;
	}
	if (claimed != NULL) {
		out->refused = g_steal_pointer(&claimed);
		out->ready = FALSE;
		goto done;
	}
	/* The rows of files already placed stay: what they were placed as is
	 * not said again. */
	ok = axis_db_conn_exec(conn, "DELETE FROM import_assignment WHERE import_id = $1::bigint AND position NOT IN"
		" (SELECT position FROM import_file WHERE import_id = $1::bigint AND (library_file_id IS NOT NULL OR step IS NOT NULL))", 1, params,
		NULL, error);
	for (i = 0; ok && i < store->len; i++) {
		const AxisImportAssigned *a = g_ptr_array_index(store, i);
		g_autofree gchar *position = axis_import_int_text(a->position);
		g_autofree gchar *episodes = ids_array(a->ids);
		g_autofree gchar *replaces = ids_array(a->replaces);
		g_autofree gchar *joined = g_strjoinv(",", a->replaces_quality);
		g_autofree gchar *qualities = g_strdup_printf("{%s}", joined);

		params[1] = position;
		params[2] = a->path;
		params[3] = episodes;
		params[4] = replaces;
		params[5] = qualities;
		params[6] = a->runtime_ok ? "true" : "false";
		params[7] = user;
		ok = axis_db_conn_exec(conn, "INSERT INTO import_assignment (import_id, position, path, episode_ids, replaces,"
			" replaces_quality, runtime_ok, by_user) VALUES ($1::bigint, $2::integer, $3, $4::bigint[], $5::bigint[],"
			" $6::text[], $7::boolean, $8) ON CONFLICT (import_id, position) DO NOTHING", 8, params, NULL, error);
	}
	for (i = 0; i < out->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(out->files, i);
		g_autofree gchar *shown = NULL;

		if (!file->chosen)
			continue;
		shown = axis_library_name_display(file->path);
		g_string_append_printf(text, " %s is %s%s.", shown, file->label, file->replaces->len > 0 ?
			", replacing the file it has (ticked)" : "");
	}
	if (out->n_left > 0)
		g_string_append_printf(text, " %u other video %s left alone.", out->n_left, out->n_left == 1 ? "file was" :
			"files were");
	if (ok) {
		GError *local = NULL;

		/* (A partly imported one is open again: its placed files stay.)
		 * Earlier overrides do not cover files assigned by hand. */
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'waiting', reason = NULL, reason_text = NULL,"
			" blockers = '[]', override_codes = '{}', attempts = 0, retry_after = NULL, finished_at = NULL, updated_at = now()"
			" WHERE id = $1::bigint", 1, params, NULL, &local);
		if (!ok && g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_error_free(local);
			axis_db_conn_rollback(conn, NULL);
			out->refused = g_strdup("Another import of the same title is open: deal with it first.");
			out->ready = FALSE;
			ok = TRUE;
			goto done;
		}
		if (!ok)
			g_propagate_error(error, local);
	}
	if (ok) {
		g_autofree gchar *cut = axis_import_text_cut(text->str, 2000);

		params[1] = user;
		params[2] = cut;
		ok = axis_db_conn_exec(conn, "INSERT INTO import_action (import_id, action, by_user, text)"
			" VALUES ($1::bigint, 'assign_files', $2, $3)", 3, params, NULL, error);
		if (ok && owner->info->note != NULL)
			ok = owner->info->note(owner->data, conn, target, "import_action", cut, user, error);
	}
	ok = ok && axis_import_enqueue(conn, import_id, 10, error) && axis_db_conn_commit(conn, error);
	*applied = ok;

done:
	if (ok && assignment != NULL)
		*assignment = g_steal_pointer(&out);
out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
