/*
 * axis-import-decide.c - the decision for every file an import found, the
 * quality of the film's file, and the tokens of its name
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: what the walk found and the probe read, what the title is expected
 * to be, the settings and what a person decided, in; every file's role and
 * every reason, and the import's verdict, out.
 *
 * Whose download it is was settled by the grab.  The checks here only
 * confirm it or block the import with a named reason: a name that reads as
 * another title, a runtime far from the title's, a quality the profile does
 * not allow, a file no better than the one in the library.  Those a person
 * can override ("import anyway"); what cannot be decided by overriding (no
 * film, several films, a film in parts, an archive) needs the file chosen.
 */

#include "import/axis-import-private.h"

#include <string.h>

/* A file shorter than this and a quarter of the longest is clearly not the
 * film. */
#define SHORT_MS            (20 * 60 * 1000)
/* The runtime check allows at least this much either way. */
#define RUNTIME_SLACK_MS    (10 * 60 * 1000)

gboolean
axis_import_overridden(
	gchar      **overrides,
	const gchar *code
){
	guint i;

	for (i = 0; overrides != NULL && overrides[i] != NULL; i++) {
		if (g_strcmp0(overrides[i], code) == 0)
			return TRUE;
	}
	return FALSE;
}

void
axis_import_verdict_clear(AxisImportVerdict *verdict)
{
	if (verdict == NULL)
		return;
	g_clear_pointer(&verdict->blockers, g_ptr_array_unref);
	g_clear_pointer(&verdict->summary, g_free);
	g_clear_pointer(&verdict->quality_text, g_free);
	g_clear_pointer(&verdict->replace, g_ptr_array_unref);
	g_clear_pointer(&verdict->file_release, axis_release_free);
	memset(verdict, 0, sizeof(*verdict));
}

static gchar *
minutes_text(gint64 ms)
{
	gint64 minutes = (ms + 30000) / 60000;

	if (ms > 0 && ms < 60000)
		return g_strdup_printf("%" G_GINT64_FORMAT " seconds", (ms + 500) / 1000);
	if (minutes >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", minutes / 60, minutes % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " min", minutes);
}

/* A blocker of the import, on its file too when there is one. */
static void
block(
	AxisImportVerdict *verdict,
	AxisImportFound   *found,
	const gchar       *code,
	gboolean           overridable,
	const gchar       *text
){
	if (found != NULL)
		axis_import_found_reason(found, code, overridable, "%s", text);
	g_ptr_array_add(verdict->blockers, axis_import_reason_new(code, text, overridable));
}

/* The stem of a found name (without its extension). */
static gchar *
stem_of(const gchar *name)
{
	g_autofree gchar *ext = axis_import_extension(name, strlen(name));

	return ext != NULL ? g_strndup(name, strlen(name) - strlen(ext)) : g_strdup(name);
}

static gboolean
sample_small(
	const AxisImportFound *found,
	gint                   runtime_minutes,
	guint64                largest_size
){
	if (found->probe_ok && found->duration_ms > 0) {
		gint64 limit_s = runtime_minutes <= 0 || runtime_minutes > 30 ? 600 : runtime_minutes > 10 ? 300 :
			runtime_minutes > 3 ? 90 : 15;

		return found->duration_ms < limit_s * 1000;
	}
	return found->size < (G_GUINT64_CONSTANT(1) << 30) && found->size * 10 < largest_size;
}

/* A name kept as it is (an extra in its folder): cleaned, fitted. */
static gchar *
kept_name(const gchar *name)
{
	g_autofree gchar *clean = axis_import_name_clean(name, -1);
	g_autofree gchar *ext = axis_import_extension(clean, strlen(clean));
	AxisImportNameInput input;
	AxisImportName built;
	gchar *result = NULL;

	if (ext == NULL)
		return NULL;
	memset(&input, 0, sizeof(input));
	memset(&built, 0, sizeof(built));
	input.title = "Extra";
	input.extension = ext;
	input.keep = clean;
	input.layout = AXIS_IMPORT_LAYOUT_PLAIN;
	if (axis_import_name_build(&input, &built, NULL))
		result = g_steal_pointer(&built.file);
	axis_import_name_clear(&built);
	return result;
}

/* ── Quality ────────────────────────────────────────────────────── */

/* The film's quality: the source from the names (the file's own first),
 * the resolution from the picture (else the names); disagreements noted.
 * @combined: the release's fields with those put in. */
static AxisQuality
derive_quality(
	const AxisImportExpect *expect,
	const AxisImportFound  *film,
	const AxisRelease      *file,
	GString                *text,
	AxisRelease           **combined
){
	const AxisRelease *grab = expect->grab;
	AxisReleaseSource source = AXIS_RELEASE_SOURCE_NONE;
	AxisReleaseResolution resolution = AXIS_RELEASE_RESOLUTION_NONE;
	AxisReleaseResolution picture = film->probe_ok ? axis_import_resolution_of(film->width, film->height) :
		AXIS_RELEASE_RESOLUTION_NONE;
	const gchar *source_from = NULL;
	const gchar *resolution_from = NULL;
	AxisRelease *fields;
	AxisQuality quality;

	if (file != NULL && file->source != AXIS_RELEASE_SOURCE_NONE) {
		source = file->source;
		source_from = "the file's own name";
	} else if (grab != NULL && grab->source != AXIS_RELEASE_SOURCE_NONE) {
		source = grab->source;
		source_from = "the release name";
	}
	if (picture != AXIS_RELEASE_RESOLUTION_NONE) {
		resolution = picture;
		resolution_from = "the picture";
	} else if (file != NULL && file->resolution != AXIS_RELEASE_RESOLUTION_NONE) {
		resolution = file->resolution;
		resolution_from = "the file's own name";
	} else if (grab != NULL && grab->resolution != AXIS_RELEASE_RESOLUTION_NONE) {
		resolution = grab->resolution;
		resolution_from = "the release name";
	}
	quality = axis_quality_of(source, resolution);
	g_string_append(text, axis_quality_label(quality));
	if (source_from != NULL || resolution_from != NULL) {
		g_string_append(text, ":");
		if (source_from != NULL)
			g_string_append_printf(text, " the source from %s", source_from);
		if (resolution_from != NULL)
			g_string_append_printf(text, "%s the resolution from %s", source_from != NULL ? "," : "",
				resolution_from);
		if (picture != AXIS_RELEASE_RESOLUTION_NONE)
			g_string_append_printf(text, " (%dx%d)", film->width, film->height);
	}
	g_string_append_c(text, '.');
	if (grab != NULL && grab->resolution != AXIS_RELEASE_RESOLUTION_NONE && picture != AXIS_RELEASE_RESOLUTION_NONE &&
	    grab->resolution != picture)
		g_string_append_printf(text, " The release name says %s; the picture is %s.",
			axis_release_resolution_name(grab->resolution), axis_release_resolution_name(picture));
	if (grab != NULL && file != NULL && grab->source != AXIS_RELEASE_SOURCE_NONE &&
	    file->source != AXIS_RELEASE_SOURCE_NONE && grab->source != file->source)
		g_string_append_printf(text, " The release name says %s, the file's own name %s.",
			axis_release_source_name(grab->source), axis_release_source_name(file->source));

	/* The fields an upgrade decision and a name read: the release's, with
	 * what was decided above. */
	fields = axis_release_parse(grab != NULL && expect->target->grab_release != NULL ? expect->target->grab_release :
		film->name);
	fields->source = source;
	fields->resolution = resolution;
	if (file != NULL) {
		fields->editions |= file->editions;
		if (fields->group == NULL && file->group != NULL)
			fields->group = g_strdup(file->group);
	}
	*combined = fields;
	return quality;
}

/* What a release a person grabbed by hand makes of one of the profile's
 * reasons. */
typedef enum
{
	HAND_KEPT,	/* it stands */
	HAND_WAIVED,	/* set aside */
	HAND_NOT_BETTER	/* the profile's policy set aside, but the file is not better */
} HandVerdict;

/* Whether the person ticked replacing @file_id when they grabbed it. */
static gboolean
ticked(
	const AxisImportTarget *target,
	gint64                  file_id
){
	guint i;

	for (i = 0; target->grab_ticked != NULL && i < target->grab_ticked->len; i++) {
		if (g_array_index(target->grab_ticked, gint64, i) == file_id)
			return TRUE;
	}
	return FALSE;
}

/* ── Assigned by hand (add-force-grab-and-assign, slice B) ──────── */

const gchar *
axis_import_quality_key(AxisQuality quality)
{
	return quality != AXIS_QUALITY_UNKNOWN ? axis_quality_name(quality) : "unknown";
}

gboolean
axis_import_runtime_off(
	const AxisImportSettings *settings,
	gint64                    duration_ms,
	gint                      runtime_minutes
){
	gint64 runtime_ms = (gint64)runtime_minutes * 60000;

	if (runtime_minutes <= 0 || duration_ms <= 0)
		return FALSE;
	return ABS(duration_ms - runtime_ms) > MAX(runtime_ms * settings->runtime_tolerance / 100, (gint64)RUNTIME_SLACK_MS);
}

/* What a person assigned @f to be, when they did: the very file the review
 * showed (its place in the download and its path). */
static const AxisImportAssigned *
assigned_at(
	const AxisImportExpect *expect,
	const AxisImportFound  *f
){
	guint i;

	for (i = 0; expect->assigned != NULL && i < expect->assigned->len; i++) {
		const AxisImportAssigned *a = g_ptr_array_index(expect->assigned, i);

		if (a->position == f->position && g_strcmp0(a->path, f->path) == 0)
			return a;
	}
	return NULL;
}

/* The files of @existing the person ticked for @hand's file, each only while
 * it has the quality it had when it was ticked (a file that changed since
 * was not the one they agreed to replace). */
static GArray *
assigned_ticks(
	const AxisImportAssigned *hand,
	const GPtrArray          *existing
){
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;
	guint k;

	for (i = 0; existing != NULL && i < existing->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(existing, i);

		if (hand->all_ticks) {
			g_array_append_val(ids, e->library_file_id);
			continue;
		}
		for (k = 0; hand->replaces != NULL && k < hand->replaces->len; k++) {
			if (g_array_index(hand->replaces, gint64, k) != e->library_file_id)
				continue;
			if (hand->replaces_quality != NULL && k < g_strv_length(hand->replaces_quality) &&
			    strcmp(hand->replaces_quality[k], axis_import_quality_key(e->quality)) == 0)
				g_array_append_val(ids, e->library_file_id);
		}
	}
	return ids;
}

/* What is overridden for @hand's file: what was for the import, and the
 * runtime when its tick was given. */
static gchar **
assigned_overrides(
	const AxisImportExpect   *expect,
	const AxisImportAssigned *hand
){
	GPtrArray *codes = g_ptr_array_new();
	guint i;

	for (i = 0; expect->overrides != NULL && expect->overrides[i] != NULL; i++)
		g_ptr_array_add(codes, g_strdup(expect->overrides[i]));
	if (hand->runtime_ok || hand->all_ticks)
		g_ptr_array_add(codes, g_strdup("runtime"));
	g_ptr_array_add(codes, NULL);
	return (gchar **)g_ptr_array_free(codes, FALSE);
}

/* The target as an assigned file is decided against: bound to what the
 * person named, the profile set aside, and replacing only what they ticked
 * (@ticks, borrowed). */
static void
assigned_target(
	const AxisImportTarget *target,
	GArray                 *ticks,
	AxisImportTarget       *bound
){
	*bound = *target;
	bound->grab_bound = TRUE;
	bound->grab_set_aside = TRUE;
	bound->grab_ticked = ticks;
	bound->assigned = TRUE;
}

static const gchar assigned_not_ticked[] = "has a file in the library that was not ticked to be replaced when the files "
	"were assigned by hand (or that changed since). Nothing was replaced: assign the files again, and tick it there.";

/* Whether every file of @existing a file of @quality is not better than (by
 * the order of qualities on disk; with @every, every file) was ticked. */
static gboolean
all_ticked(
	const AxisImportTarget *target,
	const GPtrArray        *existing,
	AxisQuality             quality,
	gboolean                every
){
	guint i;

	for (i = 0; existing != NULL && i < existing->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(existing, i);

		if (!every && quality != AXIS_QUALITY_UNKNOWN && axis_quality_compare_on_disk(quality,
		        AXIS_RELEASE_RESOLUTION_NONE, e->quality, e->resolution) > 0)
			continue;
		if (!ticked(target, e->library_file_id))
			return FALSE;
	}
	return TRUE;
}

/* A grab with the profile set aside (@set_aside: and, for an episode file,
 * every episode it holds claimed by the grab) has the profile's objections
 * and its upgrade policy (no upgrades, its cutoff) waived.  Not being better
 * than a file on disk protects that file: waived only against the files
 * the person ticked; where the policy hid the comparison (a file at the
 * cutoff), it is made here by the order of qualities on disk. */
static HandVerdict
by_hand(
	gboolean                 set_aside,
	const AxisImportTarget  *target,
	const AxisQualityReason *reason,
	AxisQuality              quality,
	const GPtrArray         *existing
){
	if (!set_aside)
		return HAND_KEPT;
	if (reason->kind == AXIS_QUALITY_REASON_PROFILE)
		return g_strcmp0(reason->code, "profile_invalid") != 0 ? HAND_WAIVED : HAND_KEPT;
	if (reason->kind != AXIS_QUALITY_REASON_UPGRADE)
		return HAND_KEPT;
	if (g_strcmp0(reason->code, "not_better") == 0)
		return all_ticked(target, existing, quality, TRUE) ? HAND_WAIVED : HAND_KEPT;
	return all_ticked(target, existing, quality, FALSE) ? HAND_WAIVED : HAND_NOT_BETTER;
}

static const gchar hand_not_better[] = "Not better than the file on disk, which was not ticked to be replaced when "
	"the release was grabbed by hand: import it anyway to replace it.";

/* A file a person keeps by hand is replaced only with the tick for that very
 * file, given when the release was grabbed, whatever kind of grab it was (a
 * grab Axis made, "Grab it anyway", a grab by hand or by force), and whatever
 * the profile or the name says. */
static void
check_kept(
	const AxisImportTarget *target,
	AxisImportFound        *film,
	AxisImportVerdict      *verdict
){
	guint i;

	/* Assigned by hand: a file that was not ticked on the review is never
	 * replaced, and "import anyway" does not change that. */
	if (target->assigned && !all_ticked(target, target->existing, AXIS_QUALITY_UNKNOWN, TRUE)) {
		g_autofree gchar *why = g_strdup_printf("The movie %s", assigned_not_ticked);

		block(verdict, film, "not_ticked", FALSE, why);
		return;
	}
	/* A grab by force replaces only the files the person ticked on its
	 * confirmation, whether or not this one is better: when Axis and the
	 * person disagree about what a release is, "better" is not enough. */
	if (target->grab_bound && !all_ticked(target, target->existing, AXIS_QUALITY_UNKNOWN, TRUE)) {
		block(verdict, film, "not_upgrade", TRUE, "The movie has a file in the library that was not ticked to be "
			"replaced when this release was grabbed by force (or that changed since): import it anyway to replace "
			"it.");
		return;
	}
	for (i = 0; target->existing != NULL && i < target->existing->len; i++) {
		const AxisImportExisting *existing = g_ptr_array_index(target->existing, i);

		if (existing->kept && !ticked(target, existing->library_file_id)) {
			/* Kept because it was brought over: no person keeps it by hand. */
			if (existing->kept_from != NULL) {
				g_autofree gchar *why = g_strdup_printf("The movie's file in the library is kept as it was migrated "
					"from %s, and this release was not grabbed with the tick to replace it: import it anyway to "
					"replace it, or dismiss this import.", existing->kept_from);

				block(verdict, film, "kept_by_hand", TRUE, why);
			} else {
				block(verdict, film, "kept_by_hand", TRUE, "The movie's file in the library is kept by hand, and this "
					"release was not grabbed with the tick to replace it: import it anyway to replace it, or dismiss "
					"this import.");
			}
			break;
		}
	}
}

/* The profile's verdict on the film's file: its objections as blockers. */
static void
check_quality(
	const AxisImportExpect *expect,
	AxisImportFound        *film,
	const AxisRelease      *fields,
	AxisImportVerdict      *verdict
){
	static const gchar *const quality_codes[] = {
		"not_allowed", "unknown_quality", "unwanted", "too_large", "too_small", "over_limit", "profile_invalid", NULL
	};
	const AxisImportTarget *target = expect->target;
	g_autoptr(AxisRelease) disk_fields = NULL;
	g_autoptr(AxisQualityVerdict) result = NULL;
	AxisQualityContext context;
	AxisQualityTitle title;
	AxisQualityCandidate candidate;
	AxisQualityOnDisk disk;
	const AxisImportExisting *best = NULL;
	guint i;

	/* First, so that nothing the profile says (not even an invalid one) lets a
	 * kept file go. */
	check_kept(target, film, verdict);
	if (!target->profile_ok) {
		block(verdict, film, "profile", TRUE, "The movie's quality profile cannot be used (its definition does not "
			"validate), so the file's quality could not be checked.");
		return;
	}
	for (i = 0; target->existing != NULL && i < target->existing->len; i++) {
		const AxisImportExisting *existing = g_ptr_array_index(target->existing, i);

		if (best == NULL || axis_quality_compare_on_disk(existing->quality, existing->resolution, best->quality,
		        best->resolution) > 0)
			best = existing;
	}
	memset(&title, 0, sizeof(title));
	title.noun = "movie";
	title.runtime_minutes = target->runtime_minutes;
	memset(&context, 0, sizeof(context));
	context.profile = &target->profile;
	context.title = &title;
	context.protocol = AXIS_QUALITY_PROTOCOL_ANY;
	context.now_us = expect->now_us;
	if (best != NULL) {
		memset(&disk, 0, sizeof(disk));
		disk.quality = best->quality;
		disk.resolution = best->resolution;
		if (best->release_name != NULL)
			disk_fields = axis_release_parse(best->release_name);
		disk.fields = disk_fields;
		disk.name = best->release_name;
		context.disk = &disk;
	}
	memset(&candidate, 0, sizeof(candidate));
	candidate.release = fields;
	candidate.name = target->grab_release != NULL ? target->grab_release : film->name;
	candidate.match = AXIS_RELEASE_MATCH_YES;
	candidate.size_bytes = (gint64)film->size;
	candidate.protocol = expect->protocol;
	candidate.seeders = -1;
	result = axis_quality_decide_one(&context, &candidate);
	for (i = 0; i < result->reasons->len; i++) {
		const AxisQualityReason *reason = g_ptr_array_index(result->reasons, i);
		guint k;

		/* Bound: what it replaces is decided by the ticks alone, below. */
		if (target->grab_bound && reason->kind == AXIS_QUALITY_REASON_UPGRADE)
			continue;
		switch (by_hand(target->grab_set_aside, target, reason, result->quality, target->existing)) {
		case HAND_WAIVED:
			continue;
		case HAND_NOT_BETTER:
			block(verdict, film, "not_upgrade", TRUE, hand_not_better);
			continue;
		default:
			break;
		}
		if (reason->kind == AXIS_QUALITY_REASON_UPGRADE) {
			block(verdict, film, "not_upgrade", TRUE, reason->text);
			continue;
		}
		if (reason->kind != AXIS_QUALITY_REASON_PROFILE)
			continue;
		for (k = 0; quality_codes[k] != NULL; k++) {
			if (g_strcmp0(reason->code, quality_codes[k]) == 0) {
				block(verdict, film, "quality", TRUE, reason->text);
				break;
			}
		}
	}
	if (best != NULL)
		verdict->upgrade = TRUE;
}

/* ── The decision ───────────────────────────────────────────────── */

static AxisImportFound *
at_position(
	GPtrArray *found,
	gint       position
){
	guint i;

	for (i = 0; i < found->len; i++) {
		AxisImportFound *item = g_ptr_array_index(found, i);

		if (item->position == position)
			return item;
	}
	return NULL;
}

/* The longest by its duration, the largest when that is not known. */
static AxisImportFound *
longest_of(GPtrArray *candidates)
{
	AxisImportFound *best = NULL;
	guint i;

	for (i = 0; i < candidates->len; i++) {
		AxisImportFound *c = g_ptr_array_index(candidates, i);

		if (best == NULL)
			best = c;
		else if (c->duration_ms > 0 && best->duration_ms > 0 ? c->duration_ms > best->duration_ms :
		         c->size > best->size)
			best = c;
	}
	return best;
}

/* @take_extras: extras are a film's (file by file, an extra is left). */
static void
classify(
	const AxisImportExpect *expect,
	GPtrArray              *found,
	GPtrArray              *candidates,
	gboolean               *archives,
	gboolean                take_extras
){
	const AxisImportSettings *settings = expect->settings;
	guint i;

	*archives = FALSE;
	for (i = 0; i < found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(found, i);
		const gchar *type = NULL;

		switch (f->type) {
		case AXIS_IMPORT_FOUND_LINK:
			axis_import_found_reason(f, "link", FALSE, "A symbolic link: Axis does not follow it, and does not "
				"import it.");
			continue;
		case AXIS_IMPORT_FOUND_SPECIAL:
			axis_import_found_reason(f, "special", FALSE, "Not a regular file: not imported.");
			continue;
		case AXIS_IMPORT_FOUND_FOLDER:
			axis_import_found_reason(f, "folder", FALSE, "A folder %s.", f->folder_note != NULL ? f->folder_note :
				"not looked into");
			continue;
		default:
			break;
		}
		switch (f->kind) {
		case AXIS_IMPORT_FILE_ARCHIVE:
			*archives = TRUE;
			axis_import_found_reason(f, "archive", FALSE, "An archive: Axis does not unpack archives, and the "
				"download client did not.");
			continue;
		case AXIS_IMPORT_FILE_SUBTITLE:
			continue;
		case AXIS_IMPORT_FILE_VIDEO:
			break;
		default:
			axis_import_found_reason(f, "other", FALSE, "Not a video or a subtitle: not imported.");
			continue;
		}
		/* (What a person assigned by hand is what they said, whatever its
		 * name or its folder says.) */
		if (assigned_at(expect, f) == NULL &&
		    (f->folder == AXIS_IMPORT_FOLDER_EXTRA || axis_import_extra_suffix(f->name, strlen(f->name), &type))) {
			if (!take_extras) {
				axis_import_found_reason(f, "extra", FALSE, "An extra: a series' extras are not imported.");
				continue;
			}
			if (!settings->extras) {
				axis_import_found_reason(f, "extra", FALSE, "An extra; \"Import named extras\" is off "
					"(Settings, Imports).");
				continue;
			}
			f->role = "extra";
			if (f->folder == AXIS_IMPORT_FOLDER_EXTRA) {
				f->dest_subdir = g_strdup(f->extra_folder);
				f->dest_name = kept_name(f->name);
				axis_import_found_reason(f, "extra", FALSE, "An extra in a %s folder: imported into the movie's %s "
					"folder.", f->extra_folder, f->extra_folder);
			} else {
				f->dest_name = axis_import_extra_name(f->name, strlen(f->name));
				axis_import_found_reason(f, "extra", FALSE, "An extra (its name ends in \"-%s\"): imported beside "
					"the movie.", type);
			}
			f->accepted = f->dest_name != NULL;
			continue;
		}
		if (f->probed && f->probe_ok && f->video_streams == 0) {
			axis_import_found_reason(f, "no_video_stream", FALSE, "ffprobe found no video stream in it: it is "
				"not the movie.");
			continue;
		}
		g_ptr_array_add(candidates, f);
	}
}

static void
take_subtitles(
	const AxisImportExpect *expect,
	GPtrArray              *found
){
	guint i;
	guint k;

	for (i = 0; i < found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(found, i);
		AxisImportSubtitle subtitle;
		gboolean of_extra = FALSE;

		if (f->type != AXIS_IMPORT_FOUND_FILE || f->kind != AXIS_IMPORT_FILE_SUBTITLE)
			continue;
		if (!expect->settings->subtitles) {
			axis_import_found_reason(f, "subtitle", FALSE, "A subtitle; \"Import subtitles\" is off "
				"(Settings, Imports).");
			continue;
		}
		/* An extra's subtitle is not the film's. */
		for (k = 0; k < found->len; k++) {
			AxisImportFound *video = g_ptr_array_index(found, k);
			g_autofree gchar *stem = NULL;

			if (g_strcmp0(video->role, "extra") != 0)
				continue;
			stem = stem_of(video->name);
			if (g_str_has_prefix(f->name, stem) && strcmp(f->dir, video->dir) == 0)
				of_extra = TRUE;
		}
		if (of_extra) {
			axis_import_found_reason(f, "subtitle", FALSE, "A subtitle of an extra: not imported.");
			continue;
		}
		axis_import_subtitle_read(f->name, strlen(f->name), &subtitle);
		f->role = "subtitle";
		f->accepted = TRUE;
		axis_import_found_reason(f, "subtitle", FALSE, "A subtitle%s%s%s%s: imported beside the movie.",
			subtitle.language != NULL ? " in " : " whose language cannot be read",
			subtitle.language != NULL ? subtitle.language : "", subtitle.forced ? ", forced" : "",
			subtitle.sdh ? ", for the deaf and hard of hearing" : "");
	}
}

/* Whether the film file's own name reads as another title. */
static void
check_name(
	const AxisImportExpect *expect,
	AxisImportFound        *film,
	AxisImportVerdict      *verdict
){
	g_autofree gchar *stem = stem_of(film->name);
	g_autofree gchar *reason = NULL;
	AxisRelease *parsed = axis_release_parse(stem);

	verdict->file_release = parsed;
	/* Nothing to compare: no title and year read, or an obfuscated name. */
	if (parsed->title == NULL || parsed->year == 0 || (parsed->stop & AXIS_RELEASE_STOP_OBFUSCATED) != 0)
		return;
	/* The name the grab was matched by is this title's. */
	if (expect->grab != NULL && expect->grab->title != NULL) {
		g_autofree gchar *a = axis_release_title_key(parsed->title);
		g_autofree gchar *b = axis_release_title_key(expect->grab->title);

		if (a != NULL && b != NULL && a[0] != '\0' && strcmp(a, b) == 0 &&
		    (expect->grab->year == 0 || ABS(expect->grab->year - parsed->year) <= 1))
			return;
	}
	/* A grab by force is bound to the title the person named: the name is
	 * not judged again (it is why the release was forced). */
	if (expect->target->grab_bound)
		return;
	if (expect->owner->name_matches == NULL ||
	    expect->owner->name_matches(expect->owner_data, expect->target, parsed, &reason))
		return;
	{
		g_autofree gchar *text = g_strdup_printf("Its name reads as another %s: %s.", expect->owner->noun != NULL ?
			expect->owner->noun : "title", reason != NULL ? reason : "it does not match this one");

		block(verdict, film, "wrong_name", TRUE, text);
	}
}

static void
check_film(
	const AxisImportExpect *expect,
	AxisImportFound        *film,
	gint                    attempts,
	AxisImportVerdict      *verdict
){
	const AxisImportTarget *target = expect->target;
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(AxisRelease) fields = NULL;

	film->role = "film";
	if (!film->probed || !film->probe_ok) {
		gboolean missing = !film->probed && expect->probe_tool_missing;

		if (attempts < AXIS_IMPORT_PROBE_ATTEMPTS) {
			verdict->retry_probe = TRUE;
			axis_import_found_reason(film, missing ? "probe_tool" : "unreadable", TRUE, "%s It is tried again.",
				missing ? "ffprobe is not installed, so the file could not be checked." :
				film->probe_text != NULL ? film->probe_text : "ffprobe could not read the file.");
		} else {
			g_autofree gchar *why = g_strdup_printf("%s Axis tried %d times; import it anyway if it plays, or "
				"choose another file.", missing ? "ffprobe is not installed, so the file could not be checked." :
				film->probe_text != NULL ? film->probe_text : "ffprobe could not read the file.", attempts);

			block(verdict, film, missing ? "probe_tool" : "unreadable", TRUE, why);
		}
	} else if (target->runtime_minutes > 0 && film->duration_ms > 0) {
		gint64 runtime_ms = (gint64)target->runtime_minutes * 60000;
		gint64 off = ABS(film->duration_ms - runtime_ms);
		gint64 allowed = MAX(runtime_ms * expect->settings->runtime_tolerance / 100, (gint64)RUNTIME_SLACK_MS);

		if (off > allowed) {
			g_autofree gchar *runs = minutes_text(film->duration_ms);
			g_autofree gchar *should = minutes_text(runtime_ms);
			g_autofree gchar *why = g_strdup_printf("The file runs %s; the %s runs %s: more than \"Runtime "
				"difference that blocks an import\" allows (%d percent, and at least 10 minutes; Settings, Imports).", runs,
				expect->owner->noun != NULL ? expect->owner->noun : "title", should,
				expect->settings->runtime_tolerance);

			block(verdict, film, "runtime", TRUE, why);
		}
	}
	check_name(expect, film, verdict);
	verdict->quality = derive_quality(expect, film, verdict->file_release, text, &fields);
	verdict->quality_text = g_strdup(text->str);
	verdict->proper = (fields->revision & (AXIS_RELEASE_REVISION_PROPER | AXIS_RELEASE_REVISION_REPACK)) != 0;
	check_quality(expect, film, fields, verdict);
	if (verdict->upgrade) {
		guint i;

		for (i = 0; i < target->existing->len; i++)
			g_ptr_array_add(verdict->replace, g_ptr_array_index(target->existing, i));
	}
}

void
axis_import_decide(
	const AxisImportExpect *expect,
	GPtrArray              *found,
	gint                    attempts,
	AxisImportVerdict      *verdict
){
	const AxisImportTarget *target = expect->target;
	g_autoptr(GPtrArray) candidates = g_ptr_array_new();
	g_autoptr(GPtrArray) plausible = g_ptr_array_new();
	/* The file a person assigned by hand as the title's, if they did: it is
	 * decided as a bound grab's chosen file is, with their ticks alone. */
	const AxisImportAssigned *hand = expect->assigned != NULL && expect->assigned->len > 0 ?
		g_ptr_array_index(expect->assigned, 0) : NULL;
	g_autoptr(GArray) hand_ticks = NULL;
	g_auto(GStrv) hand_overrides = NULL;
	AxisImportTarget bound;
	AxisImportExpect with;
	AxisImportFound *film = NULL;
	gboolean archives = FALSE;
	guint64 largest = 0;
	guint i;

	memset(verdict, 0, sizeof(*verdict));
	verdict->blockers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);
	verdict->replace = g_ptr_array_new();
	if (hand != NULL) {
		hand_ticks = assigned_ticks(hand, target->existing);
		hand_overrides = assigned_overrides(expect, hand);
		assigned_target(target, hand_ticks, &bound);
		with = *expect;
		with.target = &bound;
		with.overrides = hand_overrides;
		with.chosen = hand->position;
		expect = &with;
		target = &bound;
	}
	classify(expect, found, candidates, &archives, TRUE);

	/* Samples: marked as one (not by the title's own words) and small. */
	for (i = 0; i < candidates->len; i++)
		largest = MAX(largest, ((AxisImportFound *)g_ptr_array_index(candidates, i))->size);
	for (i = candidates->len; i > 0; i--) {
		AxisImportFound *c = g_ptr_array_index(candidates, i - 1);

		if ((gint)c->position == expect->chosen)
			continue;
		if (axis_import_sample_marked(c->name, strlen(c->name), target->title) &&
		    sample_small(c, target->runtime_minutes, largest)) {
			g_autofree gchar *how = c->probe_ok && c->duration_ms > 0 ? minutes_text(c->duration_ms) :
				axis_size_format((guint64)c->size);

			axis_import_found_reason(c, "sample", FALSE, "A sample: its name says so, and it is only %s.", how);
			g_ptr_array_remove_index(candidates, i - 1);
		}
	}

	if (hand != NULL) {
		guint parts = 0;

		/* Only the very file the review showed, and only a video the import
		 * takes (not an archive, a link, a file with no picture). */
		film = at_position(found, hand->position);
		if (film != NULL && (g_strcmp0(film->path, hand->path) != 0 || !g_ptr_array_find(candidates, film, NULL)))
			film = NULL;
		/* A video that was not chosen is not imported, an extra neither. */
		for (i = 0; i < found->len; i++) {
			AxisImportFound *f = g_ptr_array_index(found, i);

			if (g_strcmp0(f->role, "extra") != 0)
				continue;
			f->role = NULL;
			f->accepted = FALSE;
			g_clear_pointer(&f->dest_name, g_free);
			g_clear_pointer(&f->dest_subdir, g_free);
			axis_import_found_reason(f, "left_alone", FALSE, "Left alone when the files were assigned by hand.");
		}
		for (i = 0; i < candidates->len; i++) {
			AxisImportFound *c = g_ptr_array_index(candidates, i);

			if (axis_import_part_marked(c->name, strlen(c->name), NULL))
				parts++;
		}
		if (film == NULL) {
			block(verdict, NULL, "assign", FALSE, "The file that was assigned by hand is not a video of this download "
				"that can be imported (it is gone, changed, or not a readable video): each file says what it is. Assign "
				"the files again.");
		} else if (parts >= 2 && axis_import_part_marked(film->name, strlen(film->name), NULL)) {
			axis_import_found_reason(film, "multi_part", FALSE, "One part of a movie split into several.");
			block(verdict, NULL, "multi_part", FALSE, "The file that was assigned by hand is one part of a movie split "
				"into parts (cd1, cd2, ...): Axis does not join or stack parts. Join them first.");
			film = NULL;
		} else {
			for (i = 0; i < candidates->len; i++) {
				AxisImportFound *c = g_ptr_array_index(candidates, i);

				if (c != film)
					axis_import_found_reason(c, "left_alone", FALSE, "Left alone when the files were assigned by hand.");
			}
		}
	} else if (expect->chosen >= 0) {
		film = at_position(found, expect->chosen);
		if (film == NULL || film->type != AXIS_IMPORT_FOUND_FILE || film->kind != AXIS_IMPORT_FILE_VIDEO) {
			block(verdict, NULL, "chosen_missing", FALSE, "The file that was chosen is not a video of this "
				"download any more: choose again.");
			film = NULL;
		} else {
			g_free(film->dest_name);
			film->dest_name = NULL;
			film->accepted = FALSE;
			for (i = 0; i < candidates->len; i++) {
				AxisImportFound *c = g_ptr_array_index(candidates, i);

				if (c != film)
					axis_import_found_reason(c, "not_chosen", FALSE, "Another file was chosen as the %s.",
						expect->owner->noun != NULL ? expect->owner->noun : "title");
			}
		}
	} else if (candidates->len == 0) {
		if (archives)
			block(verdict, NULL, "archive", FALSE, "The download holds an archive that its client did not unpack, "
				"and no video: Axis does not unpack archives. Unpack it, then choose the file.");
		else
			block(verdict, NULL, "no_video", FALSE, "No video file was found in the download.");
	} else {
		guint parts = 0;

		for (i = 0; i < candidates->len; i++) {
			AxisImportFound *c = g_ptr_array_index(candidates, i);

			if (axis_import_part_marked(c->name, strlen(c->name), NULL))
				parts++;
		}
		film = longest_of(candidates);
		for (i = 0; i < candidates->len; i++) {
			AxisImportFound *c = g_ptr_array_index(candidates, i);

			if (c == film)
				continue;
			if (c->probe_ok && film->probe_ok && c->duration_ms < SHORT_MS && c->duration_ms * 4 < film->duration_ms) {
				g_autofree gchar *runs = minutes_text(c->duration_ms);

				axis_import_found_reason(c, "short", FALSE, "Too short to be the %s (%s), and not named as an "
					"extra: not imported.", expect->owner->noun != NULL ? expect->owner->noun : "title", runs);
				continue;
			}
			g_ptr_array_add(plausible, c);
		}
		if (parts >= 2) {
			for (i = 0; i < candidates->len; i++) {
				AxisImportFound *c = g_ptr_array_index(candidates, i);

				if (axis_import_part_marked(c->name, strlen(c->name), NULL))
					axis_import_found_reason(c, "multi_part", FALSE, "One part of a movie split into several.");
			}
			block(verdict, NULL, "multi_part", FALSE, "The movie is split into parts (cd1, cd2, ...): Axis does not "
				"join or stack parts. Join them, or choose one file.");
			film = NULL;
		} else if (plausible->len > 0) {
			g_autofree gchar *text = g_strdup_printf("The download holds %u files that could each be the %s; choose "
				"the one that is.", plausible->len + 1, expect->owner->noun != NULL ? expect->owner->noun : "title");

			axis_import_found_reason(film, "several", FALSE, "One of several files that could be it.");
			for (i = 0; i < plausible->len; i++)
				axis_import_found_reason(g_ptr_array_index(plausible, i), "several", FALSE, "One of several files "
					"that could be it.");
			block(verdict, NULL, "several", FALSE, text);
			film = NULL;
		}
	}

	if (film != NULL)
		check_film(expect, film, attempts, verdict);
	if (film != NULL)
		take_subtitles(expect, found);

	/* What a person overrode no longer blocks. */
	for (i = verdict->blockers->len; i > 0; i--) {
		const AxisImportReason *reason = g_ptr_array_index(verdict->blockers, i - 1);

		if (reason->overridable && axis_import_overridden(expect->overrides, reason->code))
			g_ptr_array_remove_index(verdict->blockers, i - 1);
	}
	verdict->film = film;
	verdict->blocked = verdict->blockers->len > 0;
	if (verdict->blocked) {
		const AxisImportReason *first = g_ptr_array_index(verdict->blockers, 0);

		verdict->code = verdict->blockers->len == 1 ? first->code : "several_reasons";
		verdict->summary = verdict->blockers->len == 1 ? g_strdup(first->text) :
			g_strdup_printf("%s (and %u more reasons)", first->text, verdict->blockers->len - 1);
		/* Not blocked by what a probe that is tried again says. */
		verdict->retry_probe = FALSE;
	} else if (verdict->retry_probe) {
		verdict->code = expect->probe_tool_missing ? "probe_tool" : "probe_retry";
		verdict->summary = g_strdup(expect->probe_tool_missing ? "ffprobe is not installed, so the file could not be "
			"checked; Axis tries again." : "ffprobe could not read the file; Axis tries again before it asks.");
	}
	if (film != NULL && !verdict->blocked && !verdict->retry_probe)
		film->accepted = TRUE;
}

/* ── File by file (a series' episodes) ──────────────────────────── */

void
axis_import_plan_free(AxisImportPlan *plan)
{
	if (plan == NULL)
		return;
	axis_import_unit_free(plan->unit);
	g_free(plan->quality_text);
	axis_release_free(plan->file_release);
	axis_release_free(plan->fields);
	g_free(plan);
}

void
axis_import_units_verdict_clear(AxisImportUnitsVerdict *verdict)
{
	if (verdict == NULL)
		return;
	g_clear_pointer(&verdict->plans, g_ptr_array_unref);
	g_clear_pointer(&verdict->blockers, g_ptr_array_unref);
	g_clear_pointer(&verdict->summary, g_free);
	memset(verdict, 0, sizeof(*verdict));
}

/* An objection to one file a person can override ("import anyway"), unless
 * they did, or the file was taken already by an earlier run. */
typedef struct
{
	const gchar *code;
	gchar       *text;
} Objection;

static gboolean
object(
	const AxisImportExpect *expect,
	AxisImportFound        *f,
	Objection              *objection,
	const gchar            *code,
	const gchar            *text
){
	if (axis_import_overridden(expect->overrides, code) ||
	    (expect->kept != NULL && g_hash_table_contains(expect->kept, f->path)))
		return FALSE;
	axis_import_found_reason(f, code, TRUE, "%s", text);
	if (objection->code == NULL) {
		objection->code = code;
		objection->text = g_strdup(text);
	}
	return TRUE;
}

/* The profile's verdict on one episode file, against the worst file of the
 * episodes it holds (an upgrade must be better than every one it replaces):
 * %TRUE when something stands against it. */
static gboolean
unit_quality(
	const AxisImportExpect *expect,
	AxisImportPlan         *plan,
	Objection              *objection
){
	static const gchar *const quality_codes[] = {
		"not_allowed", "unknown_quality", "unwanted", "too_large", "too_small", "over_limit", "profile_invalid", NULL
	};
	const AxisImportTarget *target = expect->target;
	const AxisImportUnit *unit = plan->unit;
	g_autoptr(AxisRelease) disk_fields = NULL;
	g_autoptr(AxisQualityVerdict) result = NULL;
	AxisQualityContext context;
	AxisQualityTitle title;
	AxisQualityCandidate candidate;
	AxisQualityOnDisk disk;
	const AxisImportExisting *worst = NULL;
	gboolean objected = FALSE;
	guint i;

	/* First, so that nothing the profile says lets a file go that a person
	 * did not tick: a grab by force replaces only the files ticked on its
	 * confirmation, better or not; and a file kept by hand is replaced only
	 * with the tick for that very file, whatever the kind of grab. */
	if (target->assigned && expect->kept != NULL && g_hash_table_contains(expect->kept, plan->found->path)) {
		/* Taken by an earlier run of this import, which checked the ticks
		 * before it placed the file (the file is in the library now). */
	} else if (target->assigned && !all_ticked(target, unit->existing, AXIS_QUALITY_UNKNOWN, TRUE)) {
		/* Assigned by hand: never replaced without its tick, and "import
		 * anyway" does not change that (the caller sees the file left). */
		axis_import_found_reason(plan->found, "not_ticked", FALSE, "%s %s", unit->label, assigned_not_ticked);
		objected = TRUE;
	} else if (target->grab_bound && unit->by_hand && !all_ticked(target, unit->existing, AXIS_QUALITY_UNKNOWN, TRUE)) {
		g_autofree gchar *why = g_strdup_printf("%s has a file in the library that was not ticked to be replaced when "
			"this release was grabbed by force (or that changed since): import it anyway to replace it.", unit->label);

		objected = object(expect, plan->found, objection, "not_upgrade", why) || objected;
	} else {
		for (i = 0; unit->existing != NULL && i < unit->existing->len; i++) {
			const AxisImportExisting *existing = g_ptr_array_index(unit->existing, i);

			if (existing->kept && !ticked(target, existing->library_file_id)) {
				g_autofree gchar *how = existing->kept_from != NULL ? g_strdup_printf("kept as it was migrated "
					"from %s", existing->kept_from) : g_strdup("kept by hand");
				g_autofree gchar *why = g_strdup_printf("The file of %s in the library is %s, and this "
					"release was not grabbed with the tick to replace it: import it anyway to replace it, or dismiss "
					"this import.", unit->label, how);

				objected = object(expect, plan->found, objection, "kept_by_hand", why) || objected;
				break;
			}
		}
	}
	if (!target->profile_ok)
		return object(expect, plan->found, objection, "profile", "The series' quality profile cannot be used (its "
			"definition does not validate), so the file's quality could not be checked.") || objected;
	for (i = 0; unit->existing != NULL && i < unit->existing->len; i++) {
		const AxisImportExisting *existing = g_ptr_array_index(unit->existing, i);

		if (worst == NULL || axis_quality_compare_on_disk(existing->quality, existing->resolution, worst->quality,
		        worst->resolution) < 0)
			worst = existing;
	}
	memset(&title, 0, sizeof(title));
	title.noun = "episode";
	title.runtime_minutes = unit->runtime_minutes;
	memset(&context, 0, sizeof(context));
	context.profile = &target->profile;
	context.title = &title;
	context.protocol = AXIS_QUALITY_PROTOCOL_ANY;
	context.now_us = expect->now_us;
	if (worst != NULL) {
		memset(&disk, 0, sizeof(disk));
		disk.quality = worst->quality;
		disk.resolution = worst->resolution;
		if (worst->release_name != NULL)
			disk_fields = axis_release_parse(worst->release_name);
		disk.fields = disk_fields;
		disk.name = worst->release_name;
		context.disk = &disk;
	}
	memset(&candidate, 0, sizeof(candidate));
	candidate.release = plan->fields;
	candidate.name = plan->found->name;
	candidate.match = AXIS_RELEASE_MATCH_YES;
	candidate.size_bytes = (gint64)plan->found->size;
	candidate.protocol = expect->protocol;
	candidate.seeders = -1;
	result = axis_quality_decide_one(&context, &candidate);
	for (i = 0; i < result->reasons->len; i++) {
		const AxisQualityReason *reason = g_ptr_array_index(result->reasons, i);
		guint k;

		/* Bound: what it replaces is decided by the ticks alone, above. */
		if (target->grab_bound && unit->by_hand && reason->kind == AXIS_QUALITY_REASON_UPGRADE)
			continue;
		/* By hand, for the episodes the grab claimed alone. */
		switch (by_hand(target->grab_set_aside && unit->by_hand, target, reason, result->quality, unit->existing)) {
		case HAND_WAIVED:
			continue;
		case HAND_NOT_BETTER:
			objected = object(expect, plan->found, objection, "not_upgrade", hand_not_better) || objected;
			continue;
		default:
			break;
		}
		if (reason->kind == AXIS_QUALITY_REASON_UPGRADE) {
			objected = object(expect, plan->found, objection, "not_upgrade", reason->text) || objected;
			continue;
		}
		if (reason->kind != AXIS_QUALITY_REASON_PROFILE)
			continue;
		for (k = 0; quality_codes[k] != NULL; k++) {
			if (g_strcmp0(reason->code, quality_codes[k]) == 0) {
				objected = object(expect, plan->found, objection, "quality", reason->text) || objected;
				break;
			}
		}
	}
	plan->upgrade = worst != NULL;
	return objected;
}

/* One video file: what it holds, its checks and its quality; %NULL (the
 * reasons on the file) when it is left in the download. */
static AxisImportPlan *
plan_file(
	const AxisImportExpect *expect,
	AxisImportFound        *f,
	gint                    attempts,
	AxisImportUnitsVerdict *verdict,
	Objection              *objection,
	gboolean               *hard
){
	g_autofree gchar *stem = stem_of(f->name);
	g_autofree gchar *reason = NULL;
	g_autoptr(GString) text = g_string_new(NULL);
	AxisImportUnit *unit = NULL;
	AxisImportPlan *plan;
	gboolean kept = expect->kept != NULL && g_hash_table_contains(expect->kept, f->path);
	gboolean left = FALSE;
	const AxisImportAssigned *hand = assigned_at(expect, f);
	g_autoptr(GArray) hand_ticks = NULL;
	g_auto(GStrv) hand_overrides = NULL;
	AxisImportTarget bound;
	AxisImportExpect with;
	AxisRelease *parsed;

	/* Files were assigned by hand, and this one was not: the person saw it
	 * and left it, whatever its name says.  (One an earlier run took stays
	 * taken is left exactly as it is: its name is not read to find an episode.) */
	if (expect->assigned != NULL && hand == NULL) {
		axis_import_found_reason(f, "left_alone", FALSE, "Left alone when the files were assigned by hand.");
		return NULL;
	}
	parsed = axis_release_parse(stem);
	if (hand != NULL) {
		/* What the person said it holds: its name is not read. */
		if (expect->owner->assigned == NULL ||
		    !expect->owner->assigned(expect->owner_data, expect->target, hand->ids, &unit, &reason) || unit == NULL) {
			axis_import_found_reason(f, "assign", FALSE, "Not imported as assigned by hand: %s. Assign the files again.",
				reason != NULL ? reason : "what it was assigned to is not there any more");
			axis_import_unit_free(unit);
			axis_release_free(parsed);
			*hard = TRUE;
			return NULL;
		}
		unit->by_hand = TRUE;
		/* Decided as a bound grab's file is, with the person's ticks alone. */
		hand_ticks = assigned_ticks(hand, unit->existing);
		hand_overrides = assigned_overrides(expect, hand);
		assigned_target(expect->target, hand_ticks, &bound);
		with = *expect;
		with.target = &bound;
		with.overrides = hand_overrides;
		expect = &with;
	} else if (!expect->owner->match(expect->owner_data, expect->target, parsed, f->name, &unit, &reason) ||
	           unit == NULL) {
		/* What it holds is read from its own name only, and only as episodes
		 * of the series the import is for. */
		axis_import_found_reason(f, "unmatched", FALSE, "Not imported: %s.", reason != NULL ? reason :
			"it holds no wanted episode");
		axis_import_unit_free(unit);
		axis_release_free(parsed);
		return NULL;
	}
	plan = g_new0(AxisImportPlan, 1);
	plan->found = f;
	plan->unit = unit;
	plan->file_release = parsed;
	plan->assigned = hand != NULL;
	/* Its probe: tried again a few times, then a person may import it
	 * anyway. */
	if (!kept && (!f->probed || !f->probe_ok)) {
		gboolean missing = !f->probed && expect->probe_tool_missing;

		if (attempts < AXIS_IMPORT_PROBE_ATTEMPTS) {
			verdict->retry_probe = TRUE;
			axis_import_found_reason(f, missing ? "probe_tool" : "unreadable", TRUE, "%s It is tried again.",
				missing ? "ffprobe is not installed, so the file could not be checked." :
				f->probe_text != NULL ? f->probe_text : "ffprobe could not read the file.");
		} else {
			g_autofree gchar *why = g_strdup_printf("%s Axis tried %d times; import it anyway if it plays.",
				missing ? "ffprobe is not installed, so the file could not be checked." :
				f->probe_text != NULL ? f->probe_text : "ffprobe could not read the file.", attempts);

			left = object(expect, f, objection, missing ? "probe_tool" : "unreadable", why) || left;
		}
	} else if (!kept && unit->runtime_minutes > 0 && f->duration_ms > 0) {
		gint64 runtime_ms = (gint64)unit->runtime_minutes * 60000;
		gint64 off = ABS(f->duration_ms - runtime_ms);
		gint64 allowed = MAX(runtime_ms * expect->settings->runtime_tolerance / 100, (gint64)RUNTIME_SLACK_MS);

		if (off > allowed) {
			g_autofree gchar *runs = minutes_text(f->duration_ms);
			g_autofree gchar *should = minutes_text(runtime_ms);
			g_autofree gchar *why = g_strdup_printf("The file runs %s; %s runs %s: more than \"Runtime difference "
				"that blocks an import\" allows (%d percent, and at least 10 minutes; Settings, Imports).", runs, unit->label, should,
				expect->settings->runtime_tolerance);

			left = object(expect, f, objection, "runtime", why) || left;
		}
	}
	plan->quality = derive_quality(expect, f, plan->file_release, text, &plan->fields);
	plan->quality_text = g_strdup(text->str);
	plan->proper = (plan->fields->revision & (AXIS_RELEASE_REVISION_PROPER | AXIS_RELEASE_REVISION_REPACK)) != 0;
	left = unit_quality(expect, plan, objection) || left;
	if (hand != NULL) {
		guint i;

		for (i = 0; f->reasons != NULL && i < f->reasons->len; i++) {
			if (g_strcmp0(((const AxisImportReason *)g_ptr_array_index(f->reasons, i))->code, "not_ticked") == 0)
				*hard = TRUE;
		}
	}
	if (left) {
		axis_import_plan_free(plan);
		return NULL;
	}
	return plan;
}

void
axis_import_decide_units(
	const AxisImportExpect *expect,
	GPtrArray              *found,
	gint                    attempts,
	AxisImportUnitsVerdict *verdict
){
	const AxisImportTarget *target = expect->target;
	g_autoptr(GPtrArray) candidates = g_ptr_array_new();
	Objection objection;
	gboolean archives = FALSE;
	/* A file assigned by hand cannot be taken as assigned, for a reason that
	 * "import anyway" does not override. */
	gboolean hard = FALSE;
	gboolean undone = FALSE;	/* an assigned file is not to be imported */
	guint64 largest = 0;
	guint videos = 0;
	guint i;
	guint k;

	memset(verdict, 0, sizeof(*verdict));
	memset(&objection, 0, sizeof(objection));
	verdict->plans = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_plan_free);
	verdict->blockers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);
	for (i = 0; i < found->len; i++)
		((AxisImportFound *)g_ptr_array_index(found, i))->unit_of = -1;
	classify(expect, found, candidates, &archives, FALSE);

	/* Samples: marked as one (not by the title's own words) and small. */
	for (i = 0; i < candidates->len; i++)
		largest = MAX(largest, ((AxisImportFound *)g_ptr_array_index(candidates, i))->size);
	for (i = candidates->len; i > 0; i--) {
		AxisImportFound *c = g_ptr_array_index(candidates, i - 1);

		/* (Not what a person assigned by hand: they said what it is.) */
		if (assigned_at(expect, c) != NULL)
			continue;
		if (axis_import_sample_marked(c->name, strlen(c->name), target->title) &&
		    sample_small(c, target->runtime_minutes, largest)) {
			g_autofree gchar *how = c->probe_ok && c->duration_ms > 0 ? minutes_text(c->duration_ms) :
				axis_size_format((guint64)c->size);

			axis_import_found_reason(c, "sample", FALSE, "A sample: its name says so, and it is only %s.", how);
			g_ptr_array_remove_index(candidates, i - 1);
		}
	}
	videos = candidates->len;

	/* Each video for itself: what it holds, and whether it is wanted.  The
	 * owner's match() is told how many there are: a bound grab's one video
	 * file is what the person named. */
	{
		AxisImportTarget counted = *target;
		AxisImportExpect with = *expect;
		g_autoptr(GPtrArray) names = g_ptr_array_new();

		for (i = 0; i < candidates->len; i++)
			g_ptr_array_add(names, ((AxisImportFound *)g_ptr_array_index(candidates, i))->name);
		counted.grab_videos = videos;
		counted.grab_names = names;
		with.target = &counted;
		for (i = 0; i < candidates->len; i++) {
			AxisImportPlan *plan = plan_file(&with, g_ptr_array_index(candidates, i), attempts, verdict, &objection,
				&hard);

			if (plan != NULL)
				g_ptr_array_add(verdict->plans, plan);
		}
	}
	/* A grab by force: two files that give the same episode are both left
	 * (and a third with them): which of them it is would be a guess.  One an
	 * earlier run took stays taken. */
	if (target->grab_bound) {
		g_autoptr(GPtrArray) twice = g_ptr_array_new();

		for (i = 0; i < verdict->plans->len; i++) {
			AxisImportPlan *a = g_ptr_array_index(verdict->plans, i);
			gboolean clash = FALSE;

			if (expect->kept != NULL && g_hash_table_contains(expect->kept, a->found->path))
				continue;
			for (k = 0; k < verdict->plans->len && !clash; k++) {
				AxisImportPlan *b = g_ptr_array_index(verdict->plans, k);

				clash = k != i && axis_import_unit_overlaps(a->unit, b->unit);
			}
			if (clash)
				g_ptr_array_add(twice, a);
		}
		for (i = 0; i < twice->len; i++) {
			AxisImportPlan *a = g_ptr_array_index(twice, i);

			axis_import_found_reason(a->found, "unmatched", FALSE, "Not imported: another file of the download gives "
				"%s too, and the release was grabbed by force, so which of them it is is not guessed. Assign it by "
				"hand.", a->unit->label);
			g_ptr_array_remove(verdict->plans, a);
		}
	}
	/* Two files for one episode: the one taken by an earlier run, else the
	 * better, else the larger; the other is left. */
	for (;;) {
		AxisImportPlan *loser = NULL;

		for (i = 0; i < verdict->plans->len && loser == NULL; i++) {
			for (k = i + 1; k < verdict->plans->len && loser == NULL; k++) {
				AxisImportPlan *a = g_ptr_array_index(verdict->plans, i);
				AxisImportPlan *b = g_ptr_array_index(verdict->plans, k);
				gboolean a_kept = expect->kept != NULL && g_hash_table_contains(expect->kept, a->found->path);
				gboolean b_kept = expect->kept != NULL && g_hash_table_contains(expect->kept, b->found->path);
				gint compare;

				if (!axis_import_unit_overlaps(a->unit, b->unit))
					continue;
				compare = a_kept != b_kept ? (a_kept ? 1 : -1) : axis_quality_compare(a->quality, b->quality);
				if (compare == 0)
					compare = a->found->size >= b->found->size ? 1 : -1;
				loser = compare > 0 ? b : a;
			}
		}
		if (loser == NULL)
			break;
		axis_import_found_reason(loser->found, "same_episode", FALSE, "Not imported: another file of the download "
			"holds %s too, and it is taken instead.", loser->unit->label);
		g_ptr_array_remove(verdict->plans, loser);
	}
	/* Assigned by hand: all of what the person assigned, or nothing.  A file
	 * of theirs that cannot be taken as they said (it is gone or not a video
	 * any more, its tick does not hold, another file has its episode) leaves
	 * the others too, so that nothing half of an assignment is imported. */
	for (i = 0; expect->assigned != NULL && i < expect->assigned->len; i++) {
		const AxisImportAssigned *a = g_ptr_array_index(expect->assigned, i);
		gboolean planned = FALSE;
		gboolean objected = FALSE;

		for (k = 0; k < verdict->plans->len && !planned; k++) {
			const AxisImportPlan *plan = g_ptr_array_index(verdict->plans, k);

			planned = plan->assigned && plan->found->position == a->position;
		}
		if (planned)
			continue;
		undone = TRUE;
		/* Left for something a person may still override (it cannot be read,
		 * its runtime): that is the import's blocker, below.  Anything else
		 * is not. */
		for (k = 0; k < found->len; k++) {
			const AxisImportFound *f = g_ptr_array_index(found, k);
			guint r;

			if (f->position != a->position || g_strcmp0(f->path, a->path) != 0)
				continue;
			for (r = 0; f->reasons != NULL && r < f->reasons->len; r++)
				objected = objected || ((const AxisImportReason *)g_ptr_array_index(f->reasons, r))->overridable;
		}
		if (!objected)
			hard = TRUE;
		for (k = verdict->plans->len; k > 0; k--) {
			AxisImportPlan *plan = g_ptr_array_index(verdict->plans, k - 1);

			if (expect->kept != NULL && g_hash_table_contains(expect->kept, plan->found->path))
				continue;
			g_ptr_array_remove_index(verdict->plans, k - 1);
		}
	}
	verdict->left = videos - verdict->plans->len;

	/* Subtitles go with the video whose name they begin with, beside it. */
	for (i = 0; i < found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(found, i);
		gboolean placed = FALSE;

		if (f->type != AXIS_IMPORT_FOUND_FILE || f->kind != AXIS_IMPORT_FILE_SUBTITLE)
			continue;
		if (!expect->settings->subtitles) {
			axis_import_found_reason(f, "subtitle", FALSE, "A subtitle; \"Import subtitles\" is off "
				"(Settings, Imports).");
			continue;
		}
		for (k = 0; k < verdict->plans->len && !placed; k++) {
			const AxisImportPlan *plan = g_ptr_array_index(verdict->plans, k);
			g_autofree gchar *stem = stem_of(plan->found->name);

			if (strcmp(f->dir, plan->found->dir) != 0 || !g_str_has_prefix(f->name, stem))
				continue;
			f->role = "subtitle";
			f->accepted = TRUE;
			f->unit_of = plan->found->position;
			axis_import_found_reason(f, "subtitle", FALSE, "A subtitle of %s: imported beside it.", plan->unit->label);
			placed = TRUE;
		}
		if (!placed)
			axis_import_found_reason(f, "subtitle", FALSE, "A subtitle of no file that is imported: not imported.");
	}

	if (verdict->plans->len == 0 || undone) {
		/* Nothing to import: a person decides (what can be overridden is
		 * said, file by file). */
		if (hard) {
			g_ptr_array_add(verdict->blockers, axis_import_reason_new("assign", "A file that was assigned by hand "
				"cannot be imported as it was assigned: that file says why. Nothing of the assignment was imported, and "
				"nothing in the library was replaced. Assign the files again.", FALSE));
		} else if (objection.code != NULL) {
			g_ptr_array_add(verdict->blockers, axis_import_reason_new(objection.code, objection.text, TRUE));
		} else if (verdict->retry_probe) {
			/* The probe is tried again first. */
		} else if (videos == 0) {
			g_ptr_array_add(verdict->blockers, axis_import_reason_new(archives ? "archive" : "no_video", archives ?
				"The download holds an archive that its client did not unpack, and no video: Axis does not unpack "
				"archives." : "No video file was found in the download.", FALSE));
		} else if (target->grab_bound) {
			/* Bound, and no file could be placed on what the person named:
			 * held for them, the grab open; never guessed. */
			g_ptr_array_add(verdict->blockers, axis_import_reason_new("assign", "No video file of the download could "
				"be placed on the episodes this release was grabbed for by force: each file says why, and nothing was "
				"guessed. Select \"Assign by hand\" on this page to say which episodes each file holds, or dismiss this "
				"import and search again.", FALSE));
		} else {
			g_ptr_array_add(verdict->blockers, axis_import_reason_new("nothing_wanted", "No file in the download "
				"holds a wanted episode of this series: each says why.", FALSE));
		}
	}
	g_free(objection.text);
	verdict->blocked = verdict->blockers->len > 0;
	if (verdict->blocked) {
		const AxisImportReason *first = g_ptr_array_index(verdict->blockers, 0);

		verdict->code = first->code;
		verdict->summary = g_strdup(first->text);
		verdict->retry_probe = FALSE;
	} else if (verdict->retry_probe) {
		verdict->code = expect->probe_tool_missing ? "probe_tool" : "probe_retry";
		verdict->summary = g_strdup(expect->probe_tool_missing ? "ffprobe is not installed, so the files could not "
			"be checked; Axis tries again." : "ffprobe could not read a file; Axis tries again before it asks.");
	}
	if (!verdict->blocked && !verdict->retry_probe) {
		for (i = 0; i < verdict->plans->len; i++) {
			AxisImportPlan *plan = g_ptr_array_index(verdict->plans, i);

			plan->found->role = "episode";
			plan->found->accepted = TRUE;
			axis_import_found_reason(plan->found, "episode", FALSE, "Holds %s%s%s%s: imported.%s%s", plan->unit->label,
				plan->unit->episode_title != NULL ? " (" : "", plan->unit->episode_title != NULL ?
				plan->unit->episode_title : "", plan->unit->episode_title != NULL ? ")" : "",
				plan->unit->note != NULL ? " " : "", plan->unit->note != NULL ? plan->unit->note : "");
		}
	}
}

/* ── The tokens of a name ───────────────────────────────────────── */

gchar *
axis_import_edition_text(guint editions)
{
	static const struct
	{
		guint        flag;
		const gchar *text;
	} words[] = {
		{ AXIS_RELEASE_EDITION_DIRECTORS, "Director's Cut" }, { AXIS_RELEASE_EDITION_EXTENDED, "Extended" },
		{ AXIS_RELEASE_EDITION_THEATRICAL, "Theatrical" }, { AXIS_RELEASE_EDITION_UNRATED, "Unrated" },
		{ AXIS_RELEASE_EDITION_UNCUT, "Uncut" }, { AXIS_RELEASE_EDITION_FINAL_CUT, "Final Cut" },
		{ AXIS_RELEASE_EDITION_ALTERNATE, "Alternate Cut" }, { AXIS_RELEASE_EDITION_IMAX, "IMAX" },
		{ AXIS_RELEASE_EDITION_OPEN_MATTE, "Open Matte" }, { AXIS_RELEASE_EDITION_CRITERION, "Criterion" },
		{ AXIS_RELEASE_EDITION_REMASTERED, "Remastered" }, { AXIS_RELEASE_EDITION_RESTORED, "Restored" },
		{ AXIS_RELEASE_EDITION_SPECIAL, "Special Edition" }, { AXIS_RELEASE_EDITION_COLLECTORS, "Collector's Edition" },
		{ AXIS_RELEASE_EDITION_ANNIVERSARY, "Anniversary Edition" },
		{ AXIS_RELEASE_EDITION_ULTIMATE, "Ultimate Edition" }, { AXIS_RELEASE_EDITION_DEFINITIVE, "Definitive Edition" }
	};
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(words); i++) {
		if ((editions & words[i].flag) == 0)
			continue;
		if (text->len > 0)
			g_string_append_c(text, ' ');
		g_string_append(text, words[i].text);
	}
	if (text->len == 0) {
		g_string_free(text, TRUE);
		return NULL;
	}
	return g_string_free(text, FALSE);
}

static const gchar *
channels_text(
	gint         channels,
	const gchar *stated
){
	if (channels == 1)
		return "1.0";
	if (channels == 2)
		return "2.0";
	if (channels == 6)
		return "5.1";
	if (channels == 8)
		return "7.1";
	return stated != NULL && stated[0] != '\0' ? stated : NULL;
}

gchar *
axis_import_audio_text(
	const AxisRelease *release,
	const gchar       *codec,
	const gchar       *profile,
	gint               channels
){
	static const struct
	{
		guint        flag;
		const gchar *text;
	} flags[] = {
		{ AXIS_RELEASE_AUDIO_TRUEHD, "TrueHD" }, { AXIS_RELEASE_AUDIO_DTS_X, "DTS-X" },
		{ AXIS_RELEASE_AUDIO_DTS_HD_MA, "DTS-HD MA" }, { AXIS_RELEASE_AUDIO_DTS_HD, "DTS-HD" },
		{ AXIS_RELEASE_AUDIO_DTS, "DTS" }, { AXIS_RELEASE_AUDIO_EAC3, "EAC3" }, { AXIS_RELEASE_AUDIO_AC3, "AC3" },
		{ AXIS_RELEASE_AUDIO_AAC, "AAC" }, { AXIS_RELEASE_AUDIO_FLAC, "FLAC" }, { AXIS_RELEASE_AUDIO_OPUS, "Opus" },
		{ AXIS_RELEASE_AUDIO_MP3, "MP3" }, { AXIS_RELEASE_AUDIO_PCM, "PCM" }
	};
	const gchar *name = NULL;
	const gchar *layout = NULL;
	gboolean atmos = release != NULL && release->atmos;
	guint i;

	if (codec != NULL) {
		if (strcmp(codec, "truehd") == 0)
			name = "TrueHD";
		else if (strcmp(codec, "dts") == 0)
			name = profile != NULL && strstr(profile, "MA") != NULL ? "DTS-HD MA" :
				profile != NULL && strstr(profile, "HRA") != NULL ? "DTS-HD HRA" :
				profile != NULL && strstr(profile, ":X") != NULL ? "DTS-X" : "DTS";
		else if (strcmp(codec, "eac3") == 0)
			name = "EAC3";
		else if (strcmp(codec, "ac3") == 0)
			name = "AC3";
		else if (strcmp(codec, "aac") == 0)
			name = "AAC";
		else if (strcmp(codec, "flac") == 0)
			name = "FLAC";
		else if (strcmp(codec, "opus") == 0)
			name = "Opus";
		else if (strcmp(codec, "mp3") == 0)
			name = "MP3";
		else if (g_str_has_prefix(codec, "pcm_"))
			name = "PCM";
		if (profile != NULL && strstr(profile, "Atmos") != NULL)
			atmos = TRUE;
		layout = channels_text(channels, release != NULL ? release->channels : NULL);
	}
	if (name == NULL && release != NULL) {
		for (i = 0; i < G_N_ELEMENTS(flags); i++) {
			if (release->audio & flags[i].flag) {
				name = flags[i].text;
				break;
			}
		}
		layout = release->channels[0] != '\0' ? release->channels : NULL;
	}
	if (name == NULL)
		return NULL;
	return g_strdup_printf("%s%s%s%s", name, atmos ? " Atmos" : "", layout != NULL ? " " : "",
		layout != NULL ? layout : "");
}

const gchar *
axis_import_codec_text(
	const AxisRelease *release,
	const gchar       *probe_codec
){
	if (probe_codec != NULL) {
		if (strcmp(probe_codec, "h264") == 0)
			return "x264";
		if (strcmp(probe_codec, "hevc") == 0)
			return "x265";
		if (strcmp(probe_codec, "av1") == 0)
			return "AV1";
		if (strcmp(probe_codec, "vc1") == 0)
			return "VC-1";
		if (strcmp(probe_codec, "mpeg2video") == 0)
			return "MPEG2";
		if (strcmp(probe_codec, "mpeg4") == 0)
			return "XviD";
		if (strcmp(probe_codec, "vp9") == 0)
			return "VP9";
	}
	if (release == NULL)
		return NULL;
	switch (release->codec) {
	case AXIS_RELEASE_CODEC_H264:
		return "x264";
	case AXIS_RELEASE_CODEC_H265:
		return "x265";
	case AXIS_RELEASE_CODEC_AV1:
		return "AV1";
	case AXIS_RELEASE_CODEC_VC1:
		return "VC-1";
	case AXIS_RELEASE_CODEC_MPEG2:
		return "MPEG2";
	case AXIS_RELEASE_CODEC_XVID:
		return "XviD";
	case AXIS_RELEASE_CODEC_VP9:
		return "VP9";
	default:
		return NULL;
	}
}

static gchar *
hdr_text(
	const AxisRelease *release,
	const gchar       *probe_hdr
){
	guint flags = release != NULL ? release->hdr : 0;

	if (probe_hdr != NULL) {
		if (strcmp(probe_hdr, "DV") == 0 && (flags & (AXIS_RELEASE_HDR_HDR10 | AXIS_RELEASE_HDR_HDR)) != 0)
			return g_strdup("DV HDR10");
		return g_strdup(probe_hdr);
	}
	if ((flags & AXIS_RELEASE_HDR_DOLBY_VISION) && (flags & (AXIS_RELEASE_HDR_HDR10 | AXIS_RELEASE_HDR_HDR)))
		return g_strdup("DV HDR10");
	if (flags & AXIS_RELEASE_HDR_DOLBY_VISION)
		return g_strdup("DV");
	if (flags & AXIS_RELEASE_HDR_HDR10PLUS)
		return g_strdup("HDR10Plus");
	if (flags & AXIS_RELEASE_HDR_HDR10)
		return g_strdup("HDR10");
	if (flags & AXIS_RELEASE_HDR_HLG)
		return g_strdup("HLG");
	if (flags & AXIS_RELEASE_HDR_HDR)
		return g_strdup("HDR");
	return NULL;
}

gboolean
axis_import_names_for(
	const AxisImportNaming  *naming,
	AxisImportName          *name,
	const gchar            **problem
){
	const AxisRelease *release = naming->release != NULL ? naming->release : naming->file;
	g_autofree gchar *edition = NULL;
	g_autofree gchar *quality = NULL;
	g_autofree gchar *hdr = NULL;
	g_autofree gchar *audio = NULL;
	AxisImportNameInput input;
	guint editions = 0;

	memset(&input, 0, sizeof(input));
	input.title = naming->title;
	input.year = naming->year;
	input.tmdb_id = naming->tmdb_id;
	input.imdb_id = naming->imdb_id;
	input.layout = naming->layout;
	input.format = naming->format;
	input.extension = naming->extension;
	if (!naming->rename)
		input.keep = naming->file_name;
	if (naming->release != NULL)
		editions |= naming->release->editions;
	if (naming->file != NULL)
		editions |= naming->file->editions;
	edition = axis_import_edition_text(editions);
	if (naming->quality != AXIS_QUALITY_UNKNOWN)
		quality = g_strdup_printf("%s%s", axis_quality_label(naming->quality), naming->proper ? " Proper" : "");
	hdr = hdr_text(release, naming->hdr);
	audio = axis_import_audio_text(release, naming->audio_codec, naming->audio_profile, naming->audio_channels);
	input.edition = edition;
	input.quality = quality;
	input.hdr = hdr;
	input.audio = audio;
	input.codec = axis_import_codec_text(release, naming->video_codec);
	input.group = release != NULL && release->group != NULL ? release->group :
		naming->file != NULL ? naming->file->group : NULL;
	input.episode = naming->episode;
	input.tvdb_id = naming->tvdb_id;
	input.season = naming->season;
	input.episodes = naming->episodes;
	input.n_episodes = naming->n_episodes;
	input.episode_title = naming->episode_title;
	input.air_date = naming->air_date;
	return axis_import_name_build(&input, name, problem);
}

/* ── The preview ────────────────────────────────────────────────── */

void
axis_import_preview_free(AxisImportPreview *preview)
{
	if (preview == NULL)
		return;
	g_free(preview->label);
	g_free(preview->release);
	g_free(preview->folder);
	g_free(preview->file);
	g_free(preview);
}

static const struct
{
	const gchar *title;
	gint         year;
	const gchar *tmdb;
	const gchar *imdb;
	const gchar *release;
	const gchar *file;
} samples[] = {
	{ "Hearth", 1995, "6001", "tt0600001", "Hearth.1995.REMASTERED.1080p.BluRay.DTS-HD.MA.5.1.x264-QXR",
	  "hearth.1995.remastered.1080p.bluray.x264-qxr.mkv" },
	{ "Aurélie", 2001, "6022", "tt0690000",
	  "Le.Curieux.Destin.d.Aurelie.Marchand.2001.2160p.UHD.BluRay.x265.10bit.HDR.DTS-HD.MA.5.1-ZPLNKT",
	  "Aurelie.2001.2160p.UHD.BluRay.x265-ZPLNKT.mkv" },
	{ "Mission: Improbable - Cold Reckoning Part One", 2023, "6023", "tt0500001",
	  "Mission.Improbable.Cold.Reckoning.Part.One.2023.IMAX.2160p.WEB-DL.DDP5.1.Atmos.DV.HDR10.H.265-VRTX",
	  "Mission.Improbable.Cold.Reckoning.Part.One.2023.IMAX.2160p.WEB-DL.DDP5.1.Atmos.DV.HDR10.H.265-VRTX.mkv" }
};

gboolean
axis_import_preview(
	gboolean      rename,
	const gchar  *layout,
	const gchar  *format,
	GPtrArray   **previews,
	gchar       **problem
){
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_preview_free);
	AxisImportLayout chosen = AXIS_IMPORT_LAYOUT_TMDB;
	guint i;

	*previews = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (!axis_import_format_check(format, problem))
		return FALSE;
	if (layout != NULL && !axis_import_layout_from_name(layout, &chosen)) {
		if (problem != NULL)
			*problem = g_strdup("The layout is not one of tmdb, jellyfin and plain.");
		return FALSE;
	}
	for (i = 0; i < G_N_ELEMENTS(samples); i++) {
		g_autoptr(AxisRelease) release = axis_release_parse(samples[i].release);
		g_autoptr(AxisRelease) file = NULL;
		g_autofree gchar *stem = stem_of(samples[i].file);
		g_autofree gchar *ext = axis_import_extension(samples[i].file, strlen(samples[i].file));
		AxisImportNaming naming;
		AxisImportName name;
		AxisImportPreview *preview;

		file = axis_release_parse(stem);
		memset(&naming, 0, sizeof(naming));
		naming.rename = rename;
		naming.layout = chosen;
		naming.format = format;
		naming.title = samples[i].title;
		naming.year = samples[i].year;
		naming.tmdb_id = samples[i].tmdb;
		naming.imdb_id = samples[i].imdb;
		naming.release = release;
		naming.file = file;
		naming.file_name = samples[i].file;
		naming.extension = ext;
		naming.quality = axis_quality_of_release(release);
		naming.proper = (release->revision & (AXIS_RELEASE_REVISION_PROPER | AXIS_RELEASE_REVISION_REPACK)) != 0;
		memset(&name, 0, sizeof(name));
		if (!axis_import_names_for(&naming, &name, NULL))
			continue;
		preview = g_new0(AxisImportPreview, 1);
		preview->label = g_strdup_printf("%s (%d)", samples[i].title, samples[i].year);
		preview->release = g_strdup(samples[i].release);
		preview->folder = g_steal_pointer(&name.folder);
		preview->file = g_steal_pointer(&name.file);
		axis_import_name_clear(&name);
		g_ptr_array_add(out, preview);
	}
	*previews = g_steal_pointer(&out);
	return TRUE;
}
