/*
 * axis-import.c - the import layer: lifetime, settings, owners, asking for
 * imports, what people do about them, and what the pages read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "import/axis-import-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-path.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

G_DEFINE_QUARK(axis-import-error-quark, axis_import_error)

/* ── Small helpers ──────────────────────────────────────────────── */

gchar *
axis_import_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gchar *
axis_import_text_cut(
	const gchar *text,
	guint        max
){
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;
	guint n = 0;

	if (text == NULL)
		return NULL;
	valid = g_utf8_make_valid(text, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0' && n < max; p = g_utf8_next_char(p), n++) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c))
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

static gchar *
row_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static gint64
row_int(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

static gboolean
row_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── Records ────────────────────────────────────────────────────── */

void
axis_import_existing_free(AxisImportExisting *existing)
{
	if (existing == NULL)
		return;
	g_free(existing->root_path);
	g_free(existing->rel_path);
	g_free(existing->release_name);
	g_free(existing);
}

AxisImportTarget *
axis_import_target_new(void)
{
	AxisImportTarget *target = g_new0(AxisImportTarget, 1);

	target->existing = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_existing_free);
	axis_quality_profile_init(&target->profile);
	return target;
}

GArray *
axis_import_file_ids(const gchar *list)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_auto(GStrv) parts = list != NULL ? g_strsplit(list, ",", 0) : NULL;
	guint i;

	for (i = 0; parts != NULL && parts[i] != NULL; i++) {
		guint64 value = 0;
		gint64 id;

		if (!g_ascii_string_to_unsigned(parts[i], 10, 1, G_MAXINT64, &value, NULL))
			continue;
		id = (gint64)value;
		g_array_append_val(ids, id);
	}
	return ids;
}

void
axis_import_target_free(AxisImportTarget *target)
{
	if (target == NULL)
		return;
	g_free(target->label);
	g_free(target->title);
	g_free(target->original_title);
	g_free(target->tmdb_id);
	g_free(target->imdb_id);
	g_free(target->tvdb_id);
	g_free(target->grab_release);
	g_free(target->grab_by);
	g_free(target->hold);
	g_free(target->stamp);
	g_clear_pointer(&target->grab_ticked, g_array_unref);
	g_strfreev(target->languages);
	g_clear_pointer(&target->existing, g_ptr_array_unref);
	if (target->owner_free != NULL && target->owner_data != NULL)
		target->owner_free(target->owner_data);
	g_free(target);
}

AxisImportUnit *
axis_import_unit_new(void)
{
	AxisImportUnit *unit = g_new0(AxisImportUnit, 1);

	unit->existing = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_existing_free);
	return unit;
}

void
axis_import_unit_free(AxisImportUnit *unit)
{
	if (unit == NULL)
		return;
	g_free(unit->label);
	g_free(unit->episode_title);
	g_free(unit->air_date);
	g_free(unit->note);
	g_clear_pointer(&unit->existing, g_ptr_array_unref);
	if (unit->owner_free != NULL && unit->owner_data != NULL)
		unit->owner_free(unit->owner_data);
	g_free(unit);
}

gboolean
axis_import_unit_overlaps(
	const AxisImportUnit *a,
	const AxisImportUnit *b
){
	guint i;
	guint k;

	if (a == NULL || b == NULL || a->season != b->season)
		return FALSE;
	for (i = 0; i < a->n_episodes; i++) {
		for (k = 0; k < b->n_episodes; k++) {
			if (a->episodes[i] == b->episodes[k])
				return TRUE;
		}
	}
	return FALSE;
}

void
axis_import_choice_free(AxisImportChoice *choice)
{
	if (choice == NULL)
		return;
	g_free(choice->label);
	g_free(choice);
}

void
axis_import_reason_free(AxisImportReason *reason)
{
	if (reason == NULL)
		return;
	g_free(reason->code);
	g_free(reason->text);
	g_free(reason);
}

AxisImportReason *
axis_import_reason_new(
	const gchar *code,
	const gchar *text,
	gboolean     overridable
){
	AxisImportReason *reason = g_new0(AxisImportReason, 1);

	reason->code = g_strdup(code);
	reason->text = axis_import_text_cut(text, 600);
	reason->overridable = overridable;
	return reason;
}

gchar *
axis_import_reasons_to_json(const GPtrArray *reasons)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; reasons != NULL && i < reasons->len; i++) {
		const AxisImportReason *reason = g_ptr_array_index(reasons, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "c");
		json_builder_add_string_value(builder, reason->code);
		json_builder_set_member_name(builder, "t");
		json_builder_add_string_value(builder, reason->text != NULL ? reason->text : "");
		json_builder_set_member_name(builder, "o");
		json_builder_add_boolean_value(builder, reason->overridable);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

GPtrArray *
axis_import_reasons_from_json(const gchar *json)
{
	GPtrArray *reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *array;
	guint i;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
		return reasons;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return reasons;
	array = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(array) && i < 64; i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		g_ptr_array_add(reasons, axis_import_reason_new(json_object_get_string_member_with_default(object, "c", ""),
			json_object_get_string_member_with_default(object, "t", ""),
			json_object_get_boolean_member_with_default(object, "o", FALSE)));
	}
	return reasons;
}

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const layout_choices[] = { "tmdb", "jellyfin", "plain", NULL };
static const gchar *const hardlink_choices[] = { "hardlink_or_copy", "hardlink_only", NULL };
static const gchar *const torrents_choices[] = { AXIS_IMPORT_TORRENTS_REMOVE, AXIS_IMPORT_TORRENTS_SEED, NULL };
static const gchar *const take_choices[] = { AXIS_IMPORT_TAKE_MOVE, AXIS_IMPORT_TAKE_HARDLINK, NULL };

/* The words the Settings page shows for the values above, in the same order. */
static const gchar *const layout_labels[] = {
	"For Plex and Jellyfin {tmdb-12345}", "For Jellyfin only [tmdbid-12345]", "None", NULL
};
static const gchar *const hardlink_labels[] = { "Copy and verify", "Never copy: block the import", NULL };
static const gchar *const torrents_labels[] = { "Remove it from the download client", "Leave it seeding", NULL };
static const gchar *const take_labels[] = { "Move into the library", "Always hardlink", NULL };

#define AXIS_IMPORT_TORRENTS_HELP_REMOVE \
	"Axis is not a seedbox; the torrent stops as soon as every wanted file of it is in the library. Its client is " \
	"asked to let go of it, without its files, and its files are then moved into the library (with \"Files taken " \
	"from a download\" on Always hardlink, its client removes it with its downloaded files instead). What else it left " \
	"(an .nfo, a sample) is deleted by Axis inside the client's folder for Axis's label or category; without such " \
	"a folder it stays, and the import's page says why and where."
#define AXIS_IMPORT_TORRENTS_HELP_SEED \
	"Needed for private trackers that require a ratio; the download's files then stay in the " \
	"client's folder and are hardlinked into the library (they take no extra space)."
#define AXIS_IMPORT_TAKE_HELP \
	"Move into the library: a download's files are moved into the library, so anything still in a download " \
	"folder afterwards was not imported. On the library's disk that is one rename per file; from another disk, a " \
	"copy that is verified, and the download's file deleted once the import is recorded. A Usenet download's " \
	"files are moved at once; a torrent's once its client has let go of it (\"After a torrent is imported\" is " \
	"Remove it from the download client), and a torrent left seeding is hardlinked instead. What the import did " \
	"not take (an .nfo, a sample, par2 files) is deleted by Axis with the download's folder inside the client's " \
	"folder for Axis, by strict rules. Always hardlink: every file is hardlinked (copied when the disks differ) " \
	"and the download's files stay where the client put them, as Axis did before; they take no room while both " \
	"names exist."

static const AxisSettingInfo setting_table[] = {
	{ AXIS_IMPORT_SETTING_RENAME, AXIS_SETTING_BOOLEAN, "Rename imported files",
	  "On: a movie's file is named from its title, year and identifier, then what its release says (the format "
	  "below). Off: the release's own file name is kept, inside the movie's folder, which is always named from the "
	  "title.", 0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_LAYOUT, AXIS_SETTING_CHOICE, "Identifier in folder and file names",
	  "For Plex and Jellyfin: Title (Year) {tmdb-12345}, which both Plex and Jellyfin read. For Jellyfin only: "
	  "Title (Year) [tmdbid-12345], read by Jellyfin only. None: Title (Year), with no identifier. A change "
	  "names new imports only.", 0,
	  "tmdb", 0, 0, 0, 0, layout_choices },
	{ AXIS_IMPORT_SETTING_FORMAT, AXIS_SETTING_STRING, "What follows the title in a file's name",
	  "Tokens: edition, quality, hdr, audio, codec and group, each in braces with what should surround it "
	  "({[quality]}, {-group}); a token with nothing to say is left out with what surrounds it. The Naming page "
	  "shows what it gives. A change names new imports only: files already imported keep their names.", 0,
	  AXIS_IMPORT_DEFAULT_FORMAT, 0, AXIS_IMPORT_FORMAT_MAX, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_TAKE, AXIS_SETTING_CHOICE, "Files taken from a download", AXIS_IMPORT_TAKE_HELP, 0,
	  AXIS_IMPORT_TAKE_MOVE, 0, 0, 0, 0, take_choices },
	{ AXIS_IMPORT_SETTING_HARDLINK, AXIS_SETTING_CHOICE, "Download and library on different disks",
	  "Copy and verify: a file is moved or hardlinked where it can be (the same disk) and otherwise copied and "
	  "verified (and, when it is moved, the download's file deleted afterwards); the copy is recorded and shown. "
	  "Never copy: such an import is blocked, never copied.", 0,
	  "hardlink_or_copy", 0, 0, 0, 0, hardlink_choices },
	{ AXIS_IMPORT_SETTING_RETENTION, AXIS_SETTING_DURATION, "Keep files replaced by an upgrade for",
	  "When an upgrade replaces a movie's file, the old one is moved into .axis-recycle at the top of its disk "
	  "and kept this long, so that it can be put back by hand. Its space stays used until then. 0: removed as soon "
	  "as the upgrade is complete and recorded. The default is two days. A file removed on the Duplicates page is "
	  "kept the same way and as long; with 0 it is deleted for good at the next clean-up.", 0, "2d", 0, 365 * 86400,
	  0, 0, NULL },
	{ AXIS_IMPORT_SETTING_DIR_MODE, AXIS_SETTING_STRING, "Permissions of new folders (octal)",
	  "Octal, as chmod takes it (0755), and at least 0700. Only folders Axis makes get it; files, and anything "
	  "hardlinked from a download, are never changed.", 0, "0755", 3, 5, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_FREE_FLOOR, AXIS_SETTING_BYTE_SIZE, "Free space to keep when copying",
	  "A copy is made only when the destination has room for the file and this much besides. A hardlink takes no "
	  "room and is not checked.", 0, "100MiB", 0, 0, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_SUBTITLES, AXIS_SETTING_BOOLEAN, "Import subtitles",
	  "Subtitle files of the movie (.srt, .ass, .ssa, .sub with .idx, .sup, .vtt) are placed beside it with its "
	  "name, their language and their forced or SDH marks.", 0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_EXTRAS, AXIS_SETTING_BOOLEAN, "Import named extras",
	  "Trailers, featurettes and the like, when their names end in the standard suffixes (-trailer, -featurette, "
	  "...) or they are in the standard folders (Trailers, Featurettes, ...). Nothing else is imported, and .nfo "
	  "files never are.", 0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_RUNTIME, AXIS_SETTING_INTEGER, "Runtime difference that blocks an import, in percent",
	  "A file that runs further than this from the movie's runtime (and at least 10 minutes either way) blocks its "
	  "import until a person decides.", 0, "25", 5, 200, 0, 0, NULL },
	{ AXIS_IMPORT_SETTING_TORRENTS, AXIS_SETTING_CHOICE, "After a torrent is imported",
	  "Remove it from the download client: " AXIS_IMPORT_TORRENTS_HELP_REMOVE " Leave it seeding: "
	  AXIS_IMPORT_TORRENTS_HELP_SEED, 0,
	  AXIS_IMPORT_TORRENTS_REMOVE, 0, 0, 0, 0, torrents_choices },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

gboolean
axis_import_parse_mode(
	const gchar *text,
	guint       *mode
){
	guint value = 0;
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n < 3 || n > 5)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (text[i] < '0' || text[i] > '7')
			return FALSE;
		value = value * 8 + (guint)(text[i] - '0');
	}
	if (value > 07777 || (value & 0700) != 0700)
		return FALSE;
	*mode = value;
	return TRUE;
}

static gboolean
validate_format(
	const gchar  *text,
	gchar       **reason
){
	g_autofree gchar *why = NULL;

	if (axis_import_format_check(text, &why))
		return TRUE;
	*reason = g_strdup(why != NULL ? why : "is not a valid file format");
	return FALSE;
}

static gboolean
validate_mode(
	const gchar  *text,
	gchar       **reason
){
	guint mode;

	if (axis_import_parse_mode(text, &mode))
		return TRUE;
	*reason = g_strdup("must be three or four octal digits (such as 0755) that give the owner read, write and search");
	return FALSE;
}

const AxisSettingInfo *
axis_import_get_setting_table(void)
{
	axis_setting_set_validator(AXIS_IMPORT_SETTING_FORMAT, validate_format);
	axis_setting_set_validator(AXIS_IMPORT_SETTING_DIR_MODE, validate_mode);
	axis_setting_set_choice_labels(AXIS_IMPORT_SETTING_LAYOUT, layout_labels);
	axis_setting_set_choice_labels(AXIS_IMPORT_SETTING_TAKE, take_labels);
	axis_setting_set_choice_labels(AXIS_IMPORT_SETTING_HARDLINK, hardlink_labels);
	axis_setting_set_choice_labels(AXIS_IMPORT_SETTING_TORRENTS, torrents_labels);
	return setting_table;
}

static gboolean
declared(
	AxisImport  *self,
	const gchar *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

void
axis_import_settings_read(
	AxisImport         *self,
	AxisImportSettings *s
){
	memset(s, 0, sizeof(*s));
	s->rename = declared(self, AXIS_IMPORT_SETTING_RENAME) ?
		axis_settings_get_boolean(self->settings, AXIS_IMPORT_SETTING_RENAME) : TRUE;
	s->layout = AXIS_IMPORT_LAYOUT_TMDB;
	if (declared(self, AXIS_IMPORT_SETTING_LAYOUT)) {
		g_autofree gchar *layout = axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_LAYOUT);

		axis_import_layout_from_name(layout, &s->layout);
	}
	s->format = declared(self, AXIS_IMPORT_SETTING_FORMAT) ?
		axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_FORMAT) : g_strdup(AXIS_IMPORT_DEFAULT_FORMAT);
	if (!axis_import_format_check(s->format, NULL)) {
		g_free(s->format);
		s->format = g_strdup(AXIS_IMPORT_DEFAULT_FORMAT);
	}
	if (declared(self, AXIS_IMPORT_SETTING_HARDLINK)) {
		g_autofree gchar *policy = axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_HARDLINK);

		s->hardlink_only = g_strcmp0(policy, "hardlink_only") == 0;
	}
	s->retention_s = declared(self, AXIS_IMPORT_SETTING_RETENTION) ?
		axis_settings_get_duration(self->settings, AXIS_IMPORT_SETTING_RETENTION) : 2 * 86400;
	s->dir_mode = 0755;
	if (declared(self, AXIS_IMPORT_SETTING_DIR_MODE)) {
		g_autofree gchar *mode = axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_DIR_MODE);

		if (!axis_import_parse_mode(mode, &s->dir_mode))
			s->dir_mode = 0755;
	}
	s->free_floor = declared(self, AXIS_IMPORT_SETTING_FREE_FLOOR) ?
		axis_settings_get_byte_size(self->settings, AXIS_IMPORT_SETTING_FREE_FLOOR) : 100 * 1024 * 1024;
	s->subtitles = declared(self, AXIS_IMPORT_SETTING_SUBTITLES) ?
		axis_settings_get_boolean(self->settings, AXIS_IMPORT_SETTING_SUBTITLES) : TRUE;
	s->extras = declared(self, AXIS_IMPORT_SETTING_EXTRAS) ?
		axis_settings_get_boolean(self->settings, AXIS_IMPORT_SETTING_EXTRAS) : TRUE;
	s->runtime_tolerance = declared(self, AXIS_IMPORT_SETTING_RUNTIME) ?
		(gint)axis_settings_get_integer(self->settings, AXIS_IMPORT_SETTING_RUNTIME) : 25;
	s->torrents_remove = TRUE;
	if (declared(self, AXIS_IMPORT_SETTING_TORRENTS)) {
		g_autofree gchar *after = axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_TORRENTS);

		s->torrents_remove = g_strcmp0(after, AXIS_IMPORT_TORRENTS_SEED) != 0;
	}
	s->move = TRUE;
	if (declared(self, AXIS_IMPORT_SETTING_TAKE)) {
		g_autofree gchar *take = axis_settings_get_string(self->settings, AXIS_IMPORT_SETTING_TAKE);

		s->move = g_strcmp0(take, AXIS_IMPORT_TAKE_HARDLINK) != 0;
	}
}

void
axis_import_settings_clear(AxisImportSettings *settings)
{
	g_clear_pointer(&settings->format, g_free);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisImport *
axis_import_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisStorage   *storage,
	AxisDownloads *downloads,
	AxisProbe     *probe
){
	AxisImport *self;

	g_return_val_if_fail(pool != NULL && storage != NULL && downloads != NULL && probe != NULL, NULL);

	self = g_new0(AxisImport, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->storage = axis_storage_ref(storage);
	self->downloads = axis_downloads_ref(downloads);
	self->probe = axis_probe_ref(probe);
	self->owners = g_array_new(FALSE, TRUE, sizeof(AxisImportOwner));
	g_mutex_init(&self->hook_lock);
	return self;
}

AxisImport *
axis_import_ref(AxisImport *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_import_unref(AxisImport *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	g_array_unref(self->owners);
	axis_probe_unref(self->probe);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->hook_lock);
	g_free(self);
}

void
axis_import_add_owner(
	AxisImport                *self,
	const AxisImportOwnerInfo *info,
	gpointer                   data
){
	AxisImportOwner owner;

	g_return_if_fail(self != NULL && info != NULL && info->module != NULL && info->resolve != NULL);

	owner.info = info;
	owner.data = data;
	g_array_append_val(self->owners, owner);
}

const AxisImportOwner *
axis_import_find_owner(
	AxisImport  *self,
	const gchar *module
){
	guint i;

	for (i = 0; module != NULL && i < self->owners->len; i++) {
		const AxisImportOwner *owner = &g_array_index(self->owners, AxisImportOwner, i);

		if (strcmp(owner->info->module, module) == 0)
			return owner;
	}
	return NULL;
}

AxisStorage *
axis_import_get_storage(AxisImport *self)
{
	return self->storage;
}

AxisDbPool *
axis_import_get_pool(AxisImport *self)
{
	return self->pool;
}

void
axis_import_set_hook(
	AxisImport     *self,
	AxisImportHook  hook,
	gpointer        data
){
	g_mutex_lock(&self->hook_lock);
	self->hook = hook;
	self->hook_data = data;
	g_mutex_unlock(&self->hook_lock);
}

AxisImportHookVerdict
axis_import_hook(
	AxisImport  *self,
	const gchar *step,
	gint64       import_id,
	gint64       file_id
){
	AxisImportHook hook;
	gpointer data;

	g_mutex_lock(&self->hook_lock);
	hook = self->hook;
	data = self->hook_data;
	g_mutex_unlock(&self->hook_lock);
	return hook != NULL ? hook(step, import_id, file_id, data) : AXIS_IMPORT_HOOK_GO;
}

/* ── Asking for imports ─────────────────────────────────────────── */

gboolean
axis_import_enqueue(
	AxisDbConn  *conn,
	gint64       import_id,
	gint         priority,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *key = g_strdup_printf(AXIS_IMPORT_KEY_PREFIX "%" G_GINT64_FORMAT, import_id);
	AxisJobSpec spec;

	json_object_set_int_member(payload, "import", import_id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_IMPORT_KIND;
	spec.queue = AXIS_DOWNLOAD_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = key;
	spec.max_attempts = 3;
	return axis_job_enqueue(conn, &spec, NULL, NULL, error);
}

gboolean
axis_import_request(
	AxisImport   *self,
	AxisDbConn   *conn,
	const gchar  *module,
	gint64        download_id,
	gint64       *import_id,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autofree gchar *download = axis_import_int_text(download_id);
	const gchar *params[2];
	gint64 id = 0;

	g_return_val_if_fail(self != NULL && conn != NULL, FALSE);

	if (import_id != NULL)
		*import_id = 0;
	if (created != NULL)
		*created = FALSE;
	params[0] = download;
	if (!axis_db_conn_exec(conn, "SELECT id, state, retry_after IS NULL OR retry_after <= now() FROM import"
	        " WHERE download_id = $1::bigint ORDER BY id DESC LIMIT 1", 1, params, &found, error))
		return FALSE;
	if (axis_db_result_get_n_rows(found) > 0) {
		const gchar *state = axis_db_result_get_text(found, 0, 1);

		id = row_int(found, 0, 0, 0);
		if (import_id != NULL)
			*import_id = id;
		/* Done, dismissed, or for a person to decide: nothing to queue. */
		if (g_strcmp0(state, "waiting") != 0 && g_strcmp0(state, "running") != 0)
			return TRUE;
		if (!row_bool(found, 0, 2))
			return TRUE;
		return axis_import_enqueue(conn, id, 0, error);
	}
	params[1] = module;
	if (!axis_db_conn_exec(conn, "INSERT INTO import (module, download_id, source_volume_id, source_path,"
	        " release_name, protocol, title_id, title_label, kind, state)"
	        " SELECT $2, d.id, d.volume_id, d.rel_path, d.release_name, d.protocol, d.title_id,"
	        "  left(coalesce(d.title_label, d.release_name), 600), 'auto', 'waiting'"
	        " FROM download d WHERE d.id = $1::bigint AND d.state = 'completed' AND d.rel_path IS NOT NULL"
	        "  AND d.volume_id IS NOT NULL"
	        " ON CONFLICT DO NOTHING RETURNING id", 2, params, &inserted, error))
		return FALSE;
	if (axis_db_result_get_n_rows(inserted) == 0)
		return TRUE;
	id = row_int(inserted, 0, 0, 0);
	if (import_id != NULL)
		*import_id = id;
	if (created != NULL)
		*created = TRUE;
	return axis_import_enqueue(conn, id, 0, error);
}

gboolean
axis_import_requeue_due(
	AxisImport  *self,
	AxisDbConn  *conn,
	guint       *queued,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	g_return_val_if_fail(self != NULL && conn != NULL, FALSE);

	if (queued != NULL)
		*queued = 0;
	if (!axis_db_conn_exec(conn, "SELECT id FROM import WHERE (state = 'waiting'"
	        " AND (retry_after IS NULL OR retry_after <= now()))"
	        " OR (state = 'running' AND updated_at < now() - interval '10 minutes') ORDER BY id LIMIT 200", 0, NULL,
	        &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		if (!axis_import_enqueue(conn, row_int(rows, i, 0, 0), 0, error))
			return FALSE;
		if (queued != NULL)
			(*queued)++;
	}
	return TRUE;
}

/* ── Acting ─────────────────────────────────────────────────────── */

static const gchar *const action_names[] = { "import_anyway", "choose_file", "assign", "dismiss", "retry" };

const gchar *
axis_import_action_name(AxisImportAction action)
{
	return (guint)action < G_N_ELEMENTS(action_names) ? action_names[action] : "retry";
}

gboolean
axis_import_action_from_name(
	const gchar      *name,
	AxisImportAction *action
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(action_names); i++) {
		if (strcmp(name, action_names[i]) == 0) {
			*action = (AxisImportAction)i;
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean
record_action(
	AxisDbConn   *conn,
	gint64        import_id,
	const gchar  *action,
	const gchar  *user,
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *cut = axis_import_text_cut(text, 2000);
	const gchar *params[4];

	params[0] = id;
	params[1] = action;
	params[2] = user;
	params[3] = cut;
	return axis_db_conn_exec(conn, "INSERT INTO import_action (import_id, action, by_user, text)"
		" VALUES ($1::bigint, $2, $3, $4)", 4, params, NULL, error);
}

/* The owner's target of an import, for its history (NULL when it cannot be
 * resolved, as for a title that was removed). */
static AxisImportTarget *
target_of(
	AxisImport             *self,
	AxisDbConn             *conn,
	const AxisImportOwner  *owner,
	gint64                  download_id,
	gint64                  owner_id,
	GError                **error
){
	g_autofree gchar *problem = NULL;
	AxisImportTarget *target = NULL;

	if (owner == NULL)
		return NULL;
	if (!owner->info->resolve(owner->data, conn, download_id, owner_id, &target, &problem, error))
		return NULL;
	/* One its owner holds (a series' numbering changed since the grab) has
	 * no target for these actions, as before it could be assigned by hand. */
	if (target != NULL && target->hold != NULL) {
		axis_import_target_free(target);
		return NULL;
	}
	return target;
}

static gchar **
codes_from_array(const gchar *text)
{
	g_autofree gchar *inner = NULL;
	gsize n;

	if (text == NULL)
		return g_new0(gchar *, 1);
	n = strlen(text);
	if (n < 2 || text[0] != '{' || text[n - 1] != '}')
		return g_new0(gchar *, 1);
	inner = g_strndup(text + 1, n - 2);
	if (inner[0] == '\0')
		return g_new0(gchar *, 1);
	return g_strsplit(inner, ",", 32);
}

/* {a,b} for PostgreSQL from codes (they are [a-z_] only). */
static gchar *
codes_to_array(gchar **codes)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; codes != NULL && codes[i] != NULL; i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append(out, codes[i]);
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static gboolean
open_manual(
	AxisImport                 *self,
	AxisDbConn                 *conn,
	const AxisImportManualSpec *spec,
	gint64                     *import_id,
	GError                    **error
);

gboolean
axis_import_act(
	AxisImport        *self,
	gint64             import_id,
	AxisImportAction   action,
	gint               position,
	gint64             owner_id,
	const gchar       *user,
	gint64            *new_import,
	GError           **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisDbResult) file = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *text = NULL;
	g_auto(GStrv) codes = NULL;
	const AxisImportOwner *owner;
	const gchar *state;
	const gchar *params[5];
	gint64 download_id;
	gint64 current_owner;
	AxisDbConn *conn;
	gboolean ok;
	guint put_back = 0;
	guint i;

	g_return_val_if_fail(self != NULL && user != NULL, FALSE);

	if (new_import != NULL)
		*new_import = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT state, module, download_id, owner_id, blockers::text, override_codes::text,"
			" title_label FROM import WHERE id = $1::bigint FOR UPDATE", 1, params, &row, error);
	if (!ok)
		goto out;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		ok = FALSE;
		goto out;
	}
	state = axis_db_result_get_text(row, 0, 0);
	owner = axis_import_find_owner(self, axis_db_result_get_text(row, 0, 1));
	download_id = row_int(row, 0, 2, 0);
	current_owner = row_int(row, 0, 3, 0);
	/* Left 'running' by a job that died and gave up: as waiting, unless a
	 * job is placing it now. */
	if (g_strcmp0(state, "running") == 0) {
		g_autoptr(AxisDbResult) job = NULL;

		if (!axis_db_conn_exec(conn, "SELECT 1 FROM job WHERE dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || $1"
		        " AND state = 'running'", 1, params, &job, error)) {
			ok = FALSE;
			goto out;
		}
		if (axis_db_result_get_n_rows(job) > 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
				"A job is placing that import's files now; wait for it to finish.");
			ok = FALSE;
			goto out;
		}
		state = "waiting";
	}
	if (g_strcmp0(state, "blocked") != 0 && !(g_strcmp0(state, "waiting") == 0 &&
	    (action == AXIS_IMPORT_ACT_DISMISS || action == AXIS_IMPORT_ACT_RETRY || action == AXIS_IMPORT_ACT_ASSIGN)) &&
	    !(g_strcmp0(state, "partial") == 0 && (action == AXIS_IMPORT_ACT_DISMISS || action == AXIS_IMPORT_ACT_RETRY))) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
			"That import is not waiting for a decision.");
		ok = FALSE;
		goto out;
	}
	target = target_of(self, conn, owner, download_id, current_owner, error);
	if (target == NULL && error != NULL && *error != NULL) {
		ok = FALSE;
		goto out;
	}
	/* Nothing an interrupted run placed may be left under its temporary name
	 * with nothing that will ever move it (no run takes up an import that is
	 * dismissed): a file moved out of the download is put back first (the row
	 * lock held keeps a run from starting meanwhile); one that cannot be, or
	 * that may be its file's only name, keeps the import as it is. */
	if (action == AXIS_IMPORT_ACT_DISMISS || action == AXIS_IMPORT_ACT_ASSIGN) {
		g_autofree gchar *why = NULL;
		AxisImportSettle settled = AXIS_IMPORT_SETTLED;

		/* On this connection, inside this transaction: no second one is
		 * taken while this one is held. */
		if (!axis_import_settle_temporaries(self, conn, import_id, &put_back, &settled, &why, error)) {
			ok = FALSE;
			goto out;
		}
		if (settled == AXIS_IMPORT_UNSETTLED) {
			g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "That import cannot be %s now: %s. Select "
				"\"Retry this import\", so that Axis puts the file where it belongs, and dismiss it afterwards if you still want to.",
				action == AXIS_IMPORT_ACT_DISMISS ? "dismissed" : "assigned to another title", why);
			ok = FALSE;
			goto out;
		}
		if (settled == AXIS_IMPORT_UNKNOWN) {
			g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "That import cannot be %s now: %s. Try "
				"again once it can be looked at, or select \"Retry this import\" so that Axis puts the file where it belongs.",
				action == AXIS_IMPORT_ACT_DISMISS ? "dismissed" : "assigned to another title", why);
			ok = FALSE;
			goto out;
		}
	}

	switch (action) {
	case AXIS_IMPORT_ACT_ANYWAY: {
		g_autoptr(GPtrArray) merged = g_ptr_array_new_with_free_func(g_free);
		g_auto(GStrv) before = codes_from_array(axis_db_result_get_text(row, 0, 5));
		g_autofree gchar *array = NULL;

		blockers = axis_import_reasons_from_json(axis_db_result_get_text(row, 0, 4));
		for (i = 0; i < blockers->len; i++) {
			const AxisImportReason *reason = g_ptr_array_index(blockers, i);

			if (!reason->overridable) {
				g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "\"Import anyway\" cannot override "
					"this: %s", reason->text);
				ok = FALSE;
				goto out;
			}
		}
		if (blockers->len == 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
				"Nothing that \"import anyway\" can override blocks this import.");
			ok = FALSE;
			goto out;
		}
		for (i = 0; before[i] != NULL; i++)
			g_ptr_array_add(merged, g_strdup(before[i]));
		for (i = 0; i < blockers->len; i++) {
			const AxisImportReason *reason = g_ptr_array_index(blockers, i);

			if (!axis_import_overridden(before, reason->code) && merged->len < 16)
				g_ptr_array_add(merged, g_strdup(reason->code));
		}
		g_ptr_array_add(merged, NULL);
		array = codes_to_array((gchar **)merged->pdata);
		params[1] = array;
		params[2] = user;
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'waiting', reason = NULL, reason_text = NULL,"
			" blockers = '[]', override_codes = $2::text[], override_by = $3, override_at = now(), attempts = 0,"
			" retry_after = NULL, updated_at = now() WHERE id = $1::bigint", 3, params, NULL, error);
		{
			g_autoptr(GString) list = g_string_new(NULL);

			for (i = 0; i < blockers->len; i++)
				g_string_append_printf(list, "%s%s", i > 0 ? " " : "",
					((AxisImportReason *)g_ptr_array_index(blockers, i))->text);
			text = g_strdup_printf("Imported anyway, overriding: %s", list->str);
		}
		break;
	}
	case AXIS_IMPORT_ACT_CHOOSE: {
		g_autofree gchar *pos = axis_import_int_text(position);

		params[1] = pos;
		ok = axis_db_conn_exec(conn, "SELECT path FROM import_file WHERE import_id = $1::bigint"
			" AND position = $2::integer AND kind = 'video'", 2, params, &file, error);
		if (!ok)
			goto out;
		if (axis_db_result_get_n_rows(file) == 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
				"That is not a video file of this import.");
			ok = FALSE;
			goto out;
		}
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'waiting', reason = NULL, reason_text = NULL,"
			" blockers = '[]', chosen_position = $2::integer, attempts = 0, retry_after = NULL, updated_at = now(),"
			" assign_first = false WHERE id = $1::bigint", 2, params, NULL, error) &&
			/* The later decision stands: what was assigned by hand before,
			 * with its ticks, is let go. */
			axis_db_conn_exec(conn, "DELETE FROM import_assignment WHERE import_id = $1::bigint AND position NOT IN"
			" (SELECT position FROM import_file WHERE import_id = $1::bigint AND (library_file_id IS NOT NULL OR step IS NOT NULL))", 1, params,
			NULL, error);
		{
			g_autofree gchar *shown = axis_library_name_display(axis_db_result_get_text(file, 0, 0));

			text = g_strdup_printf("Chose %s as the file to import.", shown[0] != '\0' ? shown : "the download");
		}
		break;
	}
	case AXIS_IMPORT_ACT_ASSIGN: {
		AxisImportManualSpec spec;
		gint64 opened = 0;

		if (owner == NULL || owner_id <= 0 || owner_id == current_owner) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "Choose another title.");
			ok = FALSE;
			goto out;
		}
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'dismissed', finished_at = now(), updated_at = now(),"
			" reason = 'assigned', reason_text = 'Assigned to another title by a person.' WHERE id = $1::bigint", 1,
			params, NULL, error);
		if (ok && target != NULL && owner->info->dismissed != NULL)
			ok = owner->info->dismissed(owner->data, conn, target, user, error);
		memset(&spec, 0, sizeof(spec));
		spec.module = owner->info->module;
		spec.owner_id = owner_id;
		spec.download_id = download_id;
		spec.position = -1;
		spec.user = user;
		ok = ok && open_manual(self, conn, &spec, &opened, error);
		if (new_import != NULL)
			*new_import = opened;
		text = g_strdup_printf("Assigned to another %s (import %" G_GINT64_FORMAT ").", owner->info->noun != NULL ?
			owner->info->noun : "title", opened);
		break;
	}
	case AXIS_IMPORT_ACT_DISMISS:
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'dismissed', finished_at = now(), updated_at = now(),"
			" reason = 'dismissed', reason_text = 'Dismissed by a person; the download was left where it is.'"
			" WHERE id = $1::bigint", 1, params, NULL, error);
		if (ok && target != NULL && owner != NULL && owner->info->dismissed != NULL)
			ok = owner->info->dismissed(owner->data, conn, target, user, error);
		text = put_back > 0 ? g_strdup_printf("Dismissed; %u %s that an interrupted run had moved into the library %s "
			"put back in the download first, and the download was left where it is, at its client.", put_back,
			put_back == 1 ? "file" : "files", put_back == 1 ? "was" : "were") :
			g_strdup("Dismissed; the download was left where it is, at its client.");
		break;
	case AXIS_IMPORT_ACT_RETRY:
	default: {
		g_autoptr(GError) local = NULL;
		g_autoptr(AxisDbResult) queued = NULL;

		/* Waiting for its turn already: another click makes nothing, and
		 * writes nothing (add-activity-feedback).  One held until later
		 * (storage read-only, a retry's wait) is looked at again now. */
		if (g_strcmp0(state, "waiting") == 0) {
			if (!axis_db_conn_exec(conn, "SELECT 1 FROM job j JOIN import i ON i.id = $1::bigint"
			        " WHERE j.dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || $1 AND j.state = 'queued'"
			        " AND j.run_at <= now() AND (i.retry_after IS NULL OR i.retry_after <= now())", 1, params, &queued,
			        error)) {
				ok = FALSE;
				goto out;
			}
			if (axis_db_result_get_n_rows(queued) > 0) {
				g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_IN_HAND, "That import is waiting for "
					"its turn already: Axis looks at it again as soon as its turn comes.");
				ok = FALSE;
				goto out;
			}
		}
		/* (A partly imported one is open again: its placed files stay, and
		 * the run takes up the others.) */
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'waiting', reason = NULL, reason_text = NULL,"
			" blockers = '[]', attempts = 0, retry_after = NULL, finished_at = NULL, updated_at = now()"
			" WHERE id = $1::bigint", 1, params, NULL, &local);
		if (!ok && g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_BUSY, "Another import of the same title "
				"is open: retry this one when it has finished.");
			goto out;
		}
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&local));
			goto out;
		}
		text = g_strdup(g_strcmp0(state, "partial") == 0 ? "Asked to take up the files that were not imported." :
			"Asked to retry the import.");
		break;
	}
	}
	ok = ok && record_action(conn, import_id, axis_import_action_name(action), user, text, error);
	if (ok && target != NULL && owner != NULL && owner->info->note != NULL)
		ok = owner->info->note(owner->data, conn, target, "import_action", text, user, error);
	if (ok && action != AXIS_IMPORT_ACT_DISMISS && action != AXIS_IMPORT_ACT_ASSIGN)
		ok = axis_import_enqueue(conn, import_id, 10, error);
	ok = ok && axis_db_conn_commit(conn, error);
	(void)codes;

out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_import_grab_of(
	AxisImport   *self,
	const gchar  *module,
	gint64        download_id,
	gint64        owner_id,
	gint64       *grab_id,
	GError      **error
){
	g_autoptr(AxisImportTarget) target = NULL;
	g_autofree gchar *problem = NULL;
	const AxisImportOwner *owner = axis_import_find_owner(self, module);
	AxisDbConn *conn;
	gboolean ok;

	*grab_id = 0;
	if (owner == NULL)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* One its owner holds (a series' numbering changed since the grab) has
	 * its grab all the same. */
	ok = owner->info->resolve(owner->data, conn, download_id, owner_id, &target, &problem, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && target != NULL)
		*grab_id = target->grab_id;
	return ok;
}

gboolean
axis_import_close_deleted(
	AxisImport   *self,
	gint64        import_id,
	const gchar  *expect_state,
	const gchar  *user,
	gboolean      block,
	gint64       *download_out,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *reason_text = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *why = NULL;
	const AxisImportOwner *owner;
	const gchar *state;
	const gchar *params[2];
	AxisImportSettle settled = AXIS_IMPORT_SETTLED;
	gint64 download_id;
	gint64 current_owner;
	gint64 locked_owner = 0;
	AxisDbConn *conn;
	guint put_back = 0;
	gboolean ok;

	g_return_val_if_fail(self != NULL && user != NULL, FALSE);

	if (download_out != NULL)
		*download_out = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	ok = axis_db_conn_begin(conn, error);
	/* The owner's title first, then the import's row: the order in which a
	 * grab's failure takes them (axis_acquire_handle_failure(): the title, the
	 * grab, the imports of its download), so that the two never wait for each
	 * other, and the lock under which the owner closes the grab and writes its
	 * blocklist.  The title is read before the import's row is locked, and
	 * read again under it below. */
	if (ok) {
		g_autoptr(AxisDbResult) first = NULL;
		g_autoptr(AxisImportTarget) early = NULL;
		g_autofree gchar *problem = NULL;
		const AxisImportOwner *whose;

		ok = axis_db_conn_exec(conn, "SELECT module, download_id, owner_id FROM import WHERE id = $1::bigint", 1, params,
			&first, error);
		whose = ok && axis_db_result_get_n_rows(first) > 0 ?
			axis_import_find_owner(self, axis_db_result_get_text(first, 0, 0)) : NULL;
		if (whose != NULL && whose->info->assign_lock != NULL && row_int(first, 0, 1, 0) > 0) {
			ok = whose->info->resolve(whose->data, conn, row_int(first, 0, 1, 0), row_int(first, 0, 2, 0), &early,
				&problem, error);
			if (ok && early != NULL && early->owner_id > 0) {
				locked_owner = early->owner_id;
				ok = whose->info->assign_lock(whose->data, conn, locked_owner, error);
			}
		}
	}
	ok = ok && axis_db_conn_exec(conn, "SELECT state, module, download_id, owner_id FROM import WHERE id = $1::bigint"
		" FOR UPDATE", 1, params, &row, error);
	if (!ok)
		goto out;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		ok = FALSE;
		goto out;
	}
	state = axis_db_result_get_text(row, 0, 0);
	owner = axis_import_find_owner(self, axis_db_result_get_text(row, 0, 1));
	download_id = row_int(row, 0, 2, 0);
	current_owner = row_int(row, 0, 3, 0);
	/* As a dismissal: one left 'running' by a job that died is as waiting,
	 * and one a job is placing now is refused. */
	if (g_strcmp0(state, "running") == 0) {
		g_autoptr(AxisDbResult) job = NULL;

		if (!axis_db_conn_exec(conn, "SELECT 1 FROM job WHERE dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || $1"
		        " AND state = 'running'", 1, params, &job, error)) {
			ok = FALSE;
			goto out;
		}
		if (axis_db_result_get_n_rows(job) > 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
				"A job is placing that import's files now; wait for it to finish.");
			ok = FALSE;
			goto out;
		}
		state = "waiting";
	}
	if (g_strcmp0(state, "blocked") != 0 && g_strcmp0(state, "waiting") != 0 && g_strcmp0(state, "partial") != 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED,
			"That import is not waiting for a decision any more.");
		ok = FALSE;
		goto out;
	}
	if (g_strcmp0(state, expect_state) != 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED,
			"That import changed since the confirmation was shown.");
		ok = FALSE;
		goto out;
	}
	if (download_id <= 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
			"No download of Axis's brought that import's files, so there is no download to delete.");
		ok = FALSE;
		goto out;
	}
	/* The owner's target, one it holds too (a series' numbering changed since
	 * the grab): its grab is closed all the same, for its download goes. */
	if (owner != NULL) {
		g_autofree gchar *problem = NULL;

		if (!owner->info->resolve(owner->data, conn, download_id, current_owner, &target, &problem, error)) {
			ok = FALSE;
			goto out;
		}
		/* The title that was locked is the one whose grab is closed: the import
		 * was given to another title in between otherwise. */
		if (target != NULL && owner->info->assign_lock != NULL && target->owner_id > 0 &&
		    target->owner_id != locked_owner) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED,
				"That import changed since the confirmation was shown.");
			ok = FALSE;
			goto out;
		}
	}
	/* As a dismissal: nothing an interrupted run placed may be left under its
	 * temporary name; a file moved out of the download is put back first, and
	 * one that cannot be keeps the import, and the download, as they are. */
	if (!axis_import_settle_temporaries(self, conn, import_id, &put_back, &settled, &why, error)) {
		ok = FALSE;
		goto out;
	}
	if (settled == AXIS_IMPORT_UNSETTLED) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "That import's download cannot be deleted "
			"now: %s. Select \"Retry this import\", so that Axis puts the file where it belongs, and delete the "
			"download afterwards if you still want to.", why);
		ok = FALSE;
		goto out;
	}
	if (settled == AXIS_IMPORT_UNKNOWN) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "That import's download cannot be deleted "
			"now: %s. Try again once it can be looked at, or select \"Retry this import\" so that Axis puts the file "
			"where it belongs.", why);
		ok = FALSE;
		goto out;
	}
	reason_text = g_strdup_printf("The download was deleted by %s.", user);
	params[1] = reason_text;
	/* The request, recorded before anything is asked of a client: the step
	 * (axis_import_deleted_downloads()) takes it from here, now and, for what
	 * could not be finished, every hour. */
	ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'dismissed', finished_at = now(), updated_at = now(),"
		" reason = 'download_deleted', reason_text = left($2, 2000), blockers = '[]', retry_after = NULL,"
		" client_removal = 'pending', client_removal_text = NULL, folder_removal = NULL, folder_removal_text = NULL"
		" WHERE id = $1::bigint", 2, params, NULL, error);
	if (ok && target != NULL && owner != NULL && owner->info->download_deleted != NULL)
		ok = owner->info->download_deleted(owner->data, conn, target, user, block, error);
	else if (ok && target != NULL && owner != NULL && owner->info->dismissed != NULL)
		ok = owner->info->dismissed(owner->data, conn, target, user, error);
	text = g_strdup_printf("Asked to delete the download, on disk and at its client.%s%s", block ?
		" The release was put on the blocklist too." : "", put_back > 0 ? " Files that an interrupted run had moved "
		"into the library were put back in the download first." : "");
	ok = ok && record_action(conn, import_id, "delete_download", user, text, error);
	ok = ok && axis_db_conn_commit(conn, error);
	if (ok && download_out != NULL)
		*download_out = download_id;

out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gboolean
open_manual(
	AxisImport                 *self,
	AxisDbConn                 *conn,
	const AxisImportManualSpec *spec,
	gint64                     *import_id,
	GError                    **error
){
	g_autoptr(AxisImportTarget) target = NULL;
	g_autoptr(AxisDbResult) source = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(AxisDbResult) open = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *owner_text = axis_import_int_text(spec->owner_id);
	g_autofree gchar *download_text = axis_import_int_text(spec->download_id);
	g_autofree gchar *grab_text = NULL;
	g_autofree gchar *position = spec->position >= 0 ? axis_import_int_text(spec->position) : NULL;
	g_autofree gchar *volume_text = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *title_text = NULL;
	const gchar *release = NULL;
	const gchar *protocol = NULL;
	const AxisImportOwner *owner = axis_import_find_owner(self, spec->module);
	const gchar *params[13];

	*import_id = 0;
	if (owner == NULL) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "Nothing can be imported for that.");
		return FALSE;
	}
	if (!owner->info->resolve(owner->data, conn, spec->download_id, spec->owner_id, &target, &problem, error))
		return FALSE;
	if (target == NULL || target->hold != NULL) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "%s", target != NULL ? target->hold :
			problem != NULL ? problem : "There is no such title.");
		return FALSE;
	}
	params[0] = spec->module;
	params[1] = owner_text;
	if (!axis_db_conn_exec(conn, "SELECT id FROM import WHERE module = $1 AND owner_id = $2::bigint"
	        " AND state IN ('waiting', 'running', 'blocked')", 2, params, &open, error))
		return FALSE;
	if (axis_db_result_get_n_rows(open) > 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_BUSY,
			"Another import of that title is open: deal with it first.");
		return FALSE;
	}
	if (spec->download_id > 0) {
		params[0] = download_text;
		if (!axis_db_conn_exec(conn, "SELECT volume_id, rel_path, release_name, protocol FROM download"
		        " WHERE id = $1::bigint AND state IN ('completed', 'imported') AND rel_path IS NOT NULL"
		        " AND volume_id IS NOT NULL", 1, params, &source, error))
			return FALSE;
		if (axis_db_result_get_n_rows(source) == 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID,
				"That download has not completed at a known place.");
			return FALSE;
		}
		volume_text = row_text(source, 0, 0);
		path = row_text(source, 0, 1);
		release = axis_db_result_get_text(source, 0, 2);
		protocol = axis_db_result_get_text(source, 0, 3);
		params[0] = download_text;
		params[1] = spec->module;
		/* Another open import of the download stands. */
		g_clear_pointer(&open, axis_db_result_free);
		if (!axis_db_conn_exec(conn, "SELECT id FROM import WHERE download_id = $1::bigint"
		        " AND state IN ('waiting', 'running', 'blocked')", 1, params, &open, error))
			return FALSE;
		if (axis_db_result_get_n_rows(open) > 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_BUSY,
				"Another import of that download is open: deal with it first.");
			return FALSE;
		}
	} else {
		g_autoptr(AxisRootInfo) root = NULL;
		g_autofree gchar *normal = NULL;
		const gchar *why = NULL;

		if (!axis_storage_get_root(self->storage, spec->root_id, &root, NULL) ||
		    root->media_type != AXIS_MEDIA_DOWNLOADS) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID,
				"Choose a folder of a downloads root.");
			return FALSE;
		}
		if (!axis_storage_path_normalize(spec->path, FALSE, &normal, &why) || normal[0] == '\0') {
			g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "That folder cannot be used: %s",
				why != NULL ? why : "it is the root itself");
			return FALSE;
		}
		path = root->rel_path[0] != '\0' ? g_strconcat(root->rel_path, "/", normal, NULL) : g_strdup(normal);
		volume_text = axis_import_int_text(root->volume_id);
		release = NULL;
	}
	if (target->grab_id > 0)
		grab_text = axis_import_int_text(target->grab_id);
	title_text = target->title_id > 0 ? axis_import_int_text(target->title_id) : NULL;
	label = axis_import_text_cut(target->label != NULL ? target->label : "Untitled", 600);
	params[0] = spec->module;
	params[1] = owner_text;
	params[2] = grab_text;
	params[3] = title_text;
	params[4] = label;
	params[5] = spec->download_id > 0 ? download_text : NULL;
	params[6] = volume_text;
	params[7] = path;
	params[8] = release;
	params[9] = protocol;
	params[10] = position;
	params[11] = spec->user;
	/* Opened to be assigned by hand: its run reads the files and waits. */
	params[12] = spec->assign ? "true" : "false";
	if (!axis_db_conn_exec(conn, "INSERT INTO import (module, owner_id, grab_id, title_id, title_label, download_id,"
	        " source_volume_id, source_path, release_name, protocol, kind, state, chosen_position, requested_by,"
	        " assign_first)"
	        " VALUES ($1, $2::bigint, $3::bigint, $4::bigint, $5, $6::bigint, $7::bigint, $8, $9, $10, 'manual', 'waiting',"
	        " $11::integer, $12, $13::boolean) RETURNING id", 13, params, &inserted, error))
		return FALSE;
	*import_id = row_int(inserted, 0, 0, 0);
	return record_action(conn, *import_id, "manual", spec->user != NULL ? spec->user : "root",
		spec->assign ? "Opened by hand, to be assigned by hand." : "Opened by hand.", error) &&
		axis_import_enqueue(conn, *import_id, 10, error);
}

gboolean
axis_import_manual(
	AxisImport                 *self,
	const AxisImportManualSpec *spec,
	gint64                     *import_id,
	GError                    **error
){
	AxisDbConn *conn;
	gint64 id = 0;
	gboolean ok;

	g_return_val_if_fail(self != NULL && spec != NULL && spec->user != NULL, FALSE);

	if (import_id != NULL)
		*import_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && open_manual(self, conn, spec, &id, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && import_id != NULL)
		*import_id = id;
	return ok;
}

/* ── Reading ────────────────────────────────────────────────────── */

void
axis_import_file_info_free(AxisImportFileInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->path);
	g_free(info->kind);
	g_free(info->role);
	g_clear_pointer(&info->reasons, g_ptr_array_unref);
	g_free(info->video_codec);
	g_free(info->probe_text);
	g_free(info->dest_dir);
	g_free(info->dest_name);
	g_free(info->step);
	g_free(info->mode);
	g_free(info->checksum);
	g_free(info->note);
	g_free(info);
}

void
axis_import_action_info_free(AxisImportActionInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->action);
	g_free(info->by_user);
	g_free(info->text);
	g_free(info);
}

void
axis_import_recycled_info_free(AxisImportRecycledInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->original_path);
	g_free(info->recycle_path);
	g_free(info->how);
	g_free(info->state);
	g_free(info->note);
	g_free(info->label);
	g_free(info->by_user);
	g_free(info->volume_name);
	g_free(info);
}

void
axis_import_info_free(AxisImportInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->module);
	g_free(info->title_label);
	g_free(info->source_volume);
	g_free(info->source_path);
	g_free(info->release_name);
	g_free(info->protocol);
	g_free(info->kind);
	g_free(info->state);
	g_free(info->reason);
	g_free(info->reason_text);
	g_free(info->root_name);
	g_free(info->dest_volume);
	g_free(info->dest_dir);
	g_free(info->dest_file);
	g_free(info->mode);
	g_free(info->mode_text);
	g_free(info->quality);
	g_free(info->quality_text);
	g_strfreev(info->override_codes);
	g_free(info->override_by);
	g_free(info->client_removal);
	g_free(info->client_removal_text);
	g_free(info->folder_removal);
	g_free(info->folder_removal_text);
	g_free(info->requested_by);
	g_clear_pointer(&info->files, g_ptr_array_unref);
	g_clear_pointer(&info->actions, g_ptr_array_unref);
	g_clear_pointer(&info->recycled, g_ptr_array_unref);
	g_free(info);
}

static const gchar import_select[] =
	"SELECT i.id, i.module, i.owner_id, i.grab_id, i.title_id, i.title_label, i.download_id, i.source_volume_id,"
	" sv.name, i.source_path, i.release_name, i.protocol, i.kind, i.state, i.reason, i.reason_text, i.root_id,"
	" r.name, i.dest_volume_id, dv.name, i.dest_dir,"
	" (SELECT f.dest_name FROM import_file f WHERE f.import_id = i.id AND f.role = 'film' ORDER BY f.position LIMIT 1),"
	" i.mode, i.mode_text, i.quality, i.quality_text, i.upgrade, i.chosen_position, i.override_codes::text,"
	" i.override_by, i.client_removal, i.client_removal_text, i.attempts, " US("i.retry_after") ", i.requested_by, "
	US("i.created_at") ", " US("i.updated_at") ", " US("i.finished_at") ","
	" (SELECT count(*) FROM import_recycle c WHERE c.import_id = i.id),"
	/* An import whose download a person deleted ("Delete this download") was
	 * not imported, so what became of the download's files is recorded on
	 * the download (axis_import_download_folder_removals()), and shown here. */
	" CASE WHEN i.reason = 'download_deleted' THEN (SELECT dd.folder_removal FROM download dd"
	"  WHERE dd.id = i.download_id) ELSE i.folder_removal END,"
	" CASE WHEN i.reason = 'download_deleted' THEN (SELECT dd.folder_removal_text FROM download dd"
	"  WHERE dd.id = i.download_id) ELSE i.folder_removal_text END"
	" FROM import i LEFT JOIN volume sv ON sv.id = i.source_volume_id LEFT JOIN library_root r ON r.id = i.root_id"
	" LEFT JOIN volume dv ON dv.id = i.dest_volume_id";

static AxisImportInfo *
info_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisImportInfo *info = g_new0(AxisImportInfo, 1);

	info->id = row_int(result, row, 0, 0);
	info->module = row_text(result, row, 1);
	info->owner_id = row_int(result, row, 2, 0);
	info->grab_id = row_int(result, row, 3, 0);
	info->title_id = row_int(result, row, 4, 0);
	info->title_label = row_text(result, row, 5);
	info->download_id = row_int(result, row, 6, 0);
	info->source_volume_id = row_int(result, row, 7, 0);
	info->source_volume = row_text(result, row, 8);
	info->source_path = row_text(result, row, 9);
	info->release_name = row_text(result, row, 10);
	info->protocol = row_text(result, row, 11);
	info->kind = row_text(result, row, 12);
	info->state = row_text(result, row, 13);
	info->reason = row_text(result, row, 14);
	info->reason_text = row_text(result, row, 15);
	info->root_id = row_int(result, row, 16, 0);
	info->root_name = row_text(result, row, 17);
	info->dest_volume_id = row_int(result, row, 18, 0);
	info->dest_volume = row_text(result, row, 19);
	info->dest_dir = row_text(result, row, 20);
	info->dest_file = row_text(result, row, 21);
	info->mode = row_text(result, row, 22);
	info->mode_text = row_text(result, row, 23);
	info->quality = row_text(result, row, 24);
	info->quality_text = row_text(result, row, 25);
	info->upgrade = row_bool(result, row, 26);
	info->chosen_position = (gint)row_int(result, row, 27, -1);
	info->override_codes = codes_from_array(axis_db_result_get_text(result, row, 28));
	info->override_by = row_text(result, row, 29);
	info->client_removal = row_text(result, row, 30);
	info->client_removal_text = row_text(result, row, 31);
	info->attempts = (gint)row_int(result, row, 32, 0);
	info->retry_after_us = row_int(result, row, 33, 0);
	info->requested_by = row_text(result, row, 34);
	info->created_us = row_int(result, row, 35, 0);
	info->updated_us = row_int(result, row, 36, 0);
	info->finished_us = row_int(result, row, 37, 0);
	info->n_recycled = (guint)row_int(result, row, 38, 0);
	info->folder_removal = row_text(result, row, 39);
	info->folder_removal_text = row_text(result, row, 40);
	return info;
}

gboolean
axis_import_list(
	AxisImport      *self,
	AxisImportList   which,
	guint            limit,
	GPtrArray      **imports,
	GError         **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *n = axis_import_int_text(limit > 0 ? limit : 50);
	const gchar *params[1];
	guint i;

	*imports = NULL;
	switch (which) {
	case AXIS_IMPORT_LIST_WAITING:
		sql = g_strconcat(import_select, " WHERE i.state IN ('waiting', 'running') ORDER BY i.id LIMIT $1::integer",
			NULL);
		break;
	case AXIS_IMPORT_LIST_BLOCKED:
		/* A partly imported one waits for a person too ("retry"). */
		sql = g_strconcat(import_select, " WHERE i.state IN ('blocked', 'partial') ORDER BY i.id LIMIT $1::integer",
			NULL);
		break;
	default:
		sql = g_strconcat(import_select, " WHERE i.state IN ('imported', 'dismissed')"
			" ORDER BY i.finished_at DESC, i.id DESC LIMIT $1::integer", NULL);
		break;
	}
	params[0] = n;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &result, error))
		return FALSE;
	*imports = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*imports, info_from_row(result, i));
	return TRUE;
}

gboolean
axis_import_get(
	AxisImport      *self,
	gint64           import_id,
	AxisImportInfo **info_out,
	GError         **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisDbResult) actions = NULL;
	g_autoptr(AxisDbResult) recycled = NULL;
	g_autofree gchar *sql = g_strconcat(import_select, " WHERE i.id = $1::bigint", NULL);
	g_autofree gchar *id = axis_import_int_text(import_id);
	AxisImportInfo *info;
	const gchar *params[1];
	guint i;

	*info_out = NULL;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		return FALSE;
	}
	if (!axis_db_pool_exec(self->pool, "SELECT id, position, path, kind, size, role, accepted, reasons::text,"
	        " duration_ms, width, height, video_codec, probe_text, dest_dir, dest_name, step, mode, checksum, note"
	        " FROM import_file WHERE import_id = $1::bigint ORDER BY position", 1, params, &files, error) ||
	    !axis_db_pool_exec(self->pool, "SELECT action, by_user, text, " US("created_at") " FROM import_action"
	        " WHERE import_id = $1::bigint ORDER BY id", 1, params, &actions, error) ||
	    !axis_db_pool_exec(self->pool, "SELECT original_path, recycle_path, how, state, size, " US("retired_at") ", "
	        US("remove_after") ", note FROM import_recycle WHERE import_id = $1::bigint ORDER BY id", 1, params,
	        &recycled, error))
		return FALSE;
	info = info_from_row(result, 0);
	info->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_file_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		AxisImportFileInfo *file = g_new0(AxisImportFileInfo, 1);

		file->id = row_int(files, i, 0, 0);
		file->position = (gint)row_int(files, i, 1, 0);
		file->path = row_text(files, i, 2);
		file->kind = row_text(files, i, 3);
		file->size = row_int(files, i, 4, -1);
		file->role = row_text(files, i, 5);
		file->accepted = row_bool(files, i, 6);
		file->reasons = axis_import_reasons_from_json(axis_db_result_get_text(files, i, 7));
		file->duration_ms = row_int(files, i, 8, 0);
		file->width = (gint)row_int(files, i, 9, 0);
		file->height = (gint)row_int(files, i, 10, 0);
		file->video_codec = row_text(files, i, 11);
		file->probe_text = row_text(files, i, 12);
		file->dest_dir = row_text(files, i, 13);
		file->dest_name = row_text(files, i, 14);
		file->step = row_text(files, i, 15);
		file->mode = row_text(files, i, 16);
		file->checksum = row_text(files, i, 17);
		file->note = row_text(files, i, 18);
		g_ptr_array_add(info->files, file);
	}
	info->actions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_action_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(actions); i++) {
		AxisImportActionInfo *action = g_new0(AxisImportActionInfo, 1);

		action->action = row_text(actions, i, 0);
		action->by_user = row_text(actions, i, 1);
		action->text = row_text(actions, i, 2);
		action->created_us = row_int(actions, i, 3, 0);
		g_ptr_array_add(info->actions, action);
	}
	info->recycled = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_recycled_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(recycled); i++) {
		AxisImportRecycledInfo *item = g_new0(AxisImportRecycledInfo, 1);

		item->original_path = row_text(recycled, i, 0);
		item->recycle_path = row_text(recycled, i, 1);
		item->how = row_text(recycled, i, 2);
		item->state = row_text(recycled, i, 3);
		item->size = row_int(recycled, i, 4, 0);
		item->retired_us = row_int(recycled, i, 5, 0);
		item->remove_after_us = row_int(recycled, i, 6, 0);
		item->note = row_text(recycled, i, 7);
		g_ptr_array_add(info->recycled, item);
	}
	*info_out = info;
	return TRUE;
}

gboolean
axis_import_list_choices(
	AxisImport   *self,
	const gchar  *module,
	GPtrArray   **choices,
	GError      **error
){
	const AxisImportOwner *owner = axis_import_find_owner(self, module);
	AxisDbConn *conn;
	gboolean ok;

	*choices = NULL;
	if (owner == NULL || owner->info->list == NULL) {
		*choices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_choice_free);
		return TRUE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = owner->info->list(owner->data, conn, choices, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_import_count(
	AxisImport  *self,
	guint       *waiting,
	guint       *blocked,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*waiting = 0;
	*blocked = 0;
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FILTER (WHERE state IN ('waiting', 'running')),"
	        " count(*) FILTER (WHERE state IN ('blocked', 'partial')) FROM import"
	        " WHERE state IN ('waiting', 'running', 'blocked', 'partial')", 0, NULL, &result, error))
		return FALSE;
	*waiting = (guint)row_int(result, 0, 0, 0);
	*blocked = (guint)row_int(result, 0, 1, 0);
	return TRUE;
}

/* ── Volumes that make every import a copy ──────────────────────── */

void
axis_import_volume_warning_free(AxisImportVolumeWarning *warning)
{
	if (warning == NULL)
		return;
	g_free(warning->client_name);
	g_free(warning->client_prefix);
	g_free(warning->download_volume);
	g_free(warning->root_name);
	g_free(warning->root_volume);
	g_free(warning);
}

gboolean
axis_import_volume_warnings(
	AxisDbPool  *pool,
	GPtrArray  **warnings,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	*warnings = NULL;
	if (!axis_db_pool_exec(pool, "SELECT c.id, c.name, m.client_prefix, m.volume_id, mv.name, r.id, r.name,"
	        " r.volume_id, rv.name FROM download_path_map m JOIN download_client c ON c.id = m.client_id"
	        " JOIN volume mv ON mv.id = m.volume_id JOIN library_root r ON r.media_type = 'movies'"
	        " AND r.volume_id <> m.volume_id JOIN volume rv ON rv.id = r.volume_id"
	        " ORDER BY c.name, m.client_prefix, r.name LIMIT 100", 0, NULL, &result, error))
		return FALSE;
	*warnings = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_volume_warning_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisImportVolumeWarning *warning = g_new0(AxisImportVolumeWarning, 1);

		warning->client_id = row_int(result, i, 0, 0);
		warning->client_name = row_text(result, i, 1);
		warning->client_prefix = row_text(result, i, 2);
		warning->download_volume_id = row_int(result, i, 3, 0);
		warning->download_volume = row_text(result, i, 4);
		warning->root_id = row_int(result, i, 5, 0);
		warning->root_name = row_text(result, i, 6);
		warning->root_volume_id = row_int(result, i, 7, 0);
		warning->root_volume = row_text(result, i, 8);
		g_ptr_array_add(*warnings, warning);
	}
	return TRUE;
}

gchar *
axis_import_volume_warning_text(const AxisImportVolumeWarning *warning)
{
	return g_strdup_printf("%s puts its downloads (%s) on disk %s, and the Movies library folder %s is on disk %s. A "
		"hardlink cannot cross disks, so every movie imported from %s into %s is copied: it takes the time to copy "
		"it, and as much space again while the download is kept (a torrent that seeds, its whole life). Map the "
		"download client's folder to a folder on the disk of %s, or, to block such imports instead, change "
		"\"Download and library on different disks\" in Settings, under Imports.", warning->client_name,
		warning->client_prefix, warning->download_volume,
		warning->root_name, warning->root_volume, warning->client_name, warning->root_name, warning->root_name);
}
