/*
 * axis-import-duplicates.c - movies and episodes that hold more than one
 * linked file: what the disks say of each file, which one Axis would keep
 * and why, and the removal of the files a person ticked (slice D of
 * add-force-grab-and-assign)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The owners say which titles hold several files (dup_list) and guard their
 * own rules (dup_guard: the title and the files' links locked, no grab open,
 * and which files would be left to every other episode a removed file holds).
 * Everything else is here, once for every owner.  This code removes the
 * owner's media files, so every doubt ends in a refusal:
 *
 *  - A file is never offered for removal when it is pinned (another name on
 *    its disk, a download that still holds it), when its disk cannot be
 *    written now (read-only storage, a disk that is offline, mismatched or
 *    mounted read-only), or when its owner holds it.
 *  - The recommendation is words.  Nothing is recommended, and no tick is
 *    set, unless every file of the group has a readable analysis of the file
 *    as it is now, their running times agree, and no name states a part of a
 *    set: two files linked to one movie need not be copies of it (CD1 and
 *    CD2), and a file cut short can have the better name.  Then a tick is set
 *    only for a file Axis would remove that no person keeps or linked by
 *    hand, and none when Axis cannot tell which file is better or cannot see
 *    the better one.
 *  - A removal keeps at least one file of the group that can stay: on a disk
 *    that is online, seen there at that moment as the library records it,
 *    not empty, and read by the analysis as it is now.  Every other episode
 *    a removed file holds keeps such a file too.  The index alone
 *    (library_file.state) never counts: it is what the last scan saw.
 *  - A file leaves the library in the one way it ever does: retired into the
 *    recycle folder of its own disk by the import's step for a replaced file
 *    (axis_import_retire_duplicate() in axis-import-run.c), from where the
 *    cleanup deletes it after import.recycle_retention.  Nothing here unlinks
 *    or deletes a file.
 *  - What is said afterwards is what happened: a file that was moved but
 *    whose record could not be written is said to be in the recycle folder,
 *    not "not removed".
 */

#include "import/axis-import-private.h"

#include <string.h>

#include "library/axis-library-name.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── The records ────────────────────────────────────────────────── */

AxisImportDupFile *
axis_import_dup_file_new(void)
{
	AxisImportDupFile *file = g_new0(AxisImportDupFile, 1);

	file->file = g_new0(AxisImportExisting, 1);
	return file;
}

void
axis_import_dup_file_free(AxisImportDupFile *file)
{
	if (file == NULL)
		return;
	axis_import_existing_free(file->file);
	g_free(file->hand_how);
	g_free(file->hand_by);
	g_free(file->held);
	g_free(file->also);
	g_free(file->probe);
	g_free(file->recycle_path);
	g_free(file->volume_name);
	g_free(file->fixed);
	g_free(file->advice_text);
	g_free(file->failure);
	g_free(file);
}

void
axis_import_dup_file_set_probe(
	AxisImportDupFile *file,
	const gchar       *status,
	gint64             duration_ms,
	gboolean           current
){
	g_return_if_fail(file != NULL);

	g_clear_pointer(&file->probe, g_free);
	file->duration_ms = 0;
	/* An analysis of the file as it was says nothing of the file there now. */
	if (status == NULL || !current)
		return;
	file->probe = g_strdup(status);
	if (strcmp(status, "ok") == 0 && duration_ms > 0)
		file->duration_ms = duration_ms;
}

AxisImportDupGroup *
axis_import_dup_group_new(void)
{
	AxisImportDupGroup *group = g_new0(AxisImportDupGroup, 1);

	group->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_dup_file_free);
	group->keep = -1;
	return group;
}

void
axis_import_dup_group_free(AxisImportDupGroup *group)
{
	if (group == NULL)
		return;
	g_free(group->title_label);
	g_free(group->label);
	g_free(group->href);
	g_clear_pointer(&group->files, g_ptr_array_unref);
	g_free(group->summary);
	g_free(group->busy);
	g_free(group->digest);
	g_free(group);
}

/* ── The recommendation ─────────────────────────────────────────── */

static gboolean
by_hand(const AxisImportDupFile *f)
{
	return f->file->kept || f->hand_how != NULL;
}

/* The file's last path component, for a sentence. */
static gchar *
file_name(const AxisImportDupFile *f)
{
	g_autofree gchar *shown = axis_library_name_display(f->file->rel_path);
	const gchar *slash = shown != NULL ? strrchr(shown, '/') : NULL;

	return g_strdup(slash != NULL ? slash + 1 : shown != NULL ? shown : "a file");
}

static const gchar *
quality_text(const AxisImportDupFile *f)
{
	if (f->file->quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(f->file->quality);
	return axis_quality_unknown_source_label(f->file->resolution);
}

/* Why the analysis does not vouch for the file as it is now, or %NULL when
 * it does: it read the file there now, as a video with a running time. */
static const gchar *
unreadable_words(const AxisImportDupFile *f)
{
	if (f->file->size <= 0)
		return "is empty (0 bytes)";
	if (f->probe == NULL)
		return "has not been analysed since it was added or last changed";
	if (strcmp(f->probe, "ok") != 0)
		return "could not be read as a video when it was analysed";
	if (f->duration_ms <= 0)
		return "has no running time that the analysis could read";
	return NULL;
}

static gchar *
duration_words(gint64 ms)
{
	gint64 seconds = ms / 1000;

	if (seconds >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %" G_GINT64_FORMAT " min", seconds / 3600, seconds % 3600 / 60);
	if (seconds >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " min", seconds / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " s", seconds);
}

/* The part of a set the file's name states ("cd1", "part2"), or %NULL: one
 * of cd, disc, disk, part, pt with a number, as a word of its own. */
static gchar *
part_marker(const AxisImportDupFile *f)
{
	static const gchar *const words[] = { "cd", "disc", "disk", "part", "pt", NULL };
	g_autofree gchar *name = file_name(f);
	g_autofree gchar *lower = g_ascii_strdown(name, -1);
	const gchar *p = lower;

	while (*p != '\0') {
		const gchar *start;
		guint w;

		/* The start of a word. */
		while (*p != '\0' && !g_ascii_isalnum(*p))
			p++;
		start = p;
		for (w = 0; words[w] != NULL; w++) {
			gsize len = strlen(words[w]);
			const gchar *digits;
			guint n = 0;

			if (strncmp(start, words[w], len) != 0)
				continue;
			digits = start + len;
			/* At most one separator between the word and its number. */
			if (*digits == ' ' || *digits == '.' || *digits == '_' || *digits == '-')
				digits++;
			while (g_ascii_isdigit(digits[n]))
				n++;
			if (n >= 1 && n <= 2 && !g_ascii_isalnum(digits[n]))
				return g_strdup_printf("%s%.*s", words[w], (gint)n, digits);
		}
		while (*p != '\0' && g_ascii_isalnum(*p))
			p++;
	}
	return NULL;
}

/* Why Axis does not take the files of @group for copies of one thing, in a
 * sentence, or %NULL when nothing speaks against it. */
static gchar *
copies_doubt(const AxisImportDupGroup *group)
{
	guint i;
	guint j;

	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		const gchar *why = unreadable_words(f);

		if (why != NULL) {
			g_autofree gchar *name = file_name(f);

			return g_strdup_printf("Axis could not read one of these files: %s %s. It may be cut short or damaged, or "
				"not what its name says, so Axis does not say which file to keep.", name, why);
		}
	}
	for (i = 0; i < group->files->len; i++) {
		for (j = i + 1; j < group->files->len; j++) {
			const AxisImportDupFile *a = g_ptr_array_index(group->files, i);
			const AxisImportDupFile *b = g_ptr_array_index(group->files, j);
			gint64 shorter = MIN(a->duration_ms, b->duration_ms);
			gint64 longer = MAX(a->duration_ms, b->duration_ms);

			if ((longer - shorter) * 100 > shorter * AXIS_IMPORT_DUP_DURATION_PERCENT) {
				g_autofree gchar *first = file_name(a);
				g_autofree gchar *second = file_name(b);
				g_autofree gchar *first_runs = duration_words(a->duration_ms);
				g_autofree gchar *second_runs = duration_words(b->duration_ms);

				return g_strdup_printf("These files run for different lengths, so they may not be copies of the same "
					"thing: %s runs %s, %s runs %s. One may be a part of it, another cut, or cut short.", first,
					first_runs, second, second_runs);
			}
		}
	}
	for (i = 0; i < group->files->len; i++) {
		for (j = i + 1; j < group->files->len; j++) {
			g_autofree gchar *a = part_marker(g_ptr_array_index(group->files, i));
			g_autofree gchar *b = part_marker(g_ptr_array_index(group->files, j));

			if (g_strcmp0(a, b) != 0) {
				g_autofree gchar *first = file_name(g_ptr_array_index(group->files, i));
				g_autofree gchar *second = file_name(g_ptr_array_index(group->files, j));

				return g_strdup_printf("These files are named as parts of a set (%s and %s), so they may be parts of "
					"one thing and not copies of it.", first, second);
			}
		}
	}
	return NULL;
}

/* Positive when @a is the better file, negative when @b is, 0 when nothing
 * tells them apart; *@rule says what told them apart, of the better one, and
 * *@doubt why nothing did.  As an upgrade tells two files apart
 * (check_upgrade() in src/quality/axis-quality-decide.c): the profile's order
 * when it lists both qualities, else the standard ladder; within one quality
 * the revision, then the score; and only then what an upgrade does not look
 * at, the size and a person's hand. */
static gint
compare(
	const AxisImportDupGroup  *group,
	const AxisImportDupFile   *a,
	const AxisImportDupFile   *b,
	const gchar              **rule,
	const gchar              **doubt
){
	AxisQuality qa = a->file->quality;
	AxisQuality qb = b->file->quality;
	gint c;

	*rule = NULL;
	*doubt = "cannot be compared, because the quality of one of them is not known";
	if (qa == AXIS_QUALITY_UNKNOWN || qb == AXIS_QUALITY_UNKNOWN) {
		/* Nothing known of one of them: no place on the ladder to compare. */
		if ((qa == AXIS_QUALITY_UNKNOWN && a->file->resolution == AXIS_RELEASE_RESOLUTION_NONE) ||
		    (qb == AXIS_QUALITY_UNKNOWN && b->file->resolution == AXIS_RELEASE_RESOLUTION_NONE))
			return 0;
		c = axis_quality_compare_on_disk(qa, a->file->resolution, qb, b->file->resolution);
		if (c != 0)
			*rule = "its place on the quality ladder is higher";
		return c;
	}
	if (qa != qb) {
		if (group->profile_ok) {
			gint ta = axis_quality_profile_tier(&group->profile, qa);
			gint tb = axis_quality_profile_tier(&group->profile, qb);

			if (ta >= 0 && tb >= 0 && ta != tb) {
				*rule = "the quality profile prefers its quality";
				return ta < tb ? 1 : -1;
			}
		}
		/* The profile does not order the two (it lists one of them, or
		 * neither, or both alike): the standard ladder, as for an upgrade.  A
		 * quality the profile does not list is not worse for that. */
		c = axis_quality_compare(qa, qb);
		if (c != 0) {
			*rule = "its quality is higher";
			return c;
		}
	}
	if (a->ranked && b->ranked) {
		if (a->revision != b->revision) {
			*rule = "it is of the same quality and a later revision (a proper or repack)";
			return a->revision > b->revision ? 1 : -1;
		}
		if (a->score != b->score) {
			*rule = "it is of the same quality and its release scores higher in the quality profile";
			return a->score > b->score ? 1 : -1;
		}
		if (a->file->size != b->file->size) {
			*rule = "it is of the same quality and larger";
			return a->file->size > b->file->size ? 1 : -1;
		}
	} else if (a->file->size != b->file->size) {
		/* An upgrade would look at the revision and the score first, and
		 * they are not known here: the size alone does not say. */
		*doubt = "have the same quality, and their quality profile cannot be used to rank them further (the size "
			"alone does not say which is better)";
		return 0;
	}
	if (by_hand(a) != by_hand(b)) {
		*rule = "it is of the same quality and size, and a person keeps it or linked it by hand";
		return by_hand(a) ? 1 : -1;
	}
	*doubt = "have the same quality and the same size, and neither is kept by hand";
	return 0;
}

static void
set_text(
	gchar **place,
	gchar  *text
){
	g_free(*place);
	*place = text;
}

/* Who made or keeps the link, for a sentence: "root linked it by hand". */
static gchar *
hand_words(const AxisImportDupFile *f)
{
	const gchar *who = f->hand_by != NULL && f->hand_by[0] != '\0' ? f->hand_by : "a person";

	if (g_strcmp0(f->hand_how, "forced") == 0)
		return g_strdup_printf("%s grabbed it by force%s", who, f->file->kept ? ", and it is kept by hand" : "");
	if (g_strcmp0(f->hand_how, "assigned") == 0)
		return g_strdup_printf("%s assigned it by hand%s", who, f->file->kept ? ", and it is kept by hand" : "");
	if (g_strcmp0(f->hand_how, "brought") == 0)
		return g_strdup_printf("%s migrated it from Radarr or Sonarr%s", who, !f->file->kept ? "" :
			f->file->kept_from != NULL ? ", and Axis does not replace it" : ", and it is kept by hand");
	if (f->hand_how != NULL)
		return g_strdup_printf("%s linked it by hand%s", who, f->file->kept ? ", and it is kept by hand" : "");
	return g_strdup("it is kept by hand");
}

void
axis_import_dup_recommend(AxisImportDupGroup *group)
{
	g_autofree gchar *not_copies = NULL;
	const gchar *rule = NULL;
	const gchar *doubt = NULL;
	gint keep = -1;
	guint i;
	guint j;

	g_return_if_fail(group != NULL);

	/* First of all: are these copies of one thing, as far as Axis can tell? */
	not_copies = group->files->len >= 2 ? copies_doubt(group) : NULL;
	/* The file that wins against every other. */
	for (i = 0; not_copies == NULL && i < group->files->len && keep < 0; i++) {
		gboolean wins = TRUE;

		for (j = 0; j < group->files->len && wins; j++) {
			if (j != i && compare(group, g_ptr_array_index(group->files, i), g_ptr_array_index(group->files, j),
			        &rule, &doubt) <= 0)
				wins = FALSE;
		}
		if (wins)
			keep = (gint)i;
	}
	group->keep = -1;
	if (group->files->len < 2) {
		set_text(&group->summary, g_strdup("There is one file: nothing to clean up."));
	} else if (not_copies != NULL) {
		set_text(&group->summary, g_strdup_printf("%s Nothing is recommended and nothing is ticked for you: look at the "
			"files yourself before you remove one.", not_copies));
	} else if (keep < 0) {
		/* Say which two could not be told apart. */
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;
		const gchar *why = NULL;

		for (i = 0; i < group->files->len && first == NULL; i++) {
			for (j = i + 1; j < group->files->len && first == NULL; j++) {
				const AxisImportDupFile *a = g_ptr_array_index(group->files, i);
				const AxisImportDupFile *b = g_ptr_array_index(group->files, j);

				if (compare(group, a, b, &rule, &doubt) == 0) {
					first = file_name(a);
					second = file_name(b);
					why = doubt;
				}
			}
		}
		set_text(&group->summary, first == NULL ?
			g_strdup("Axis cannot tell which file is better: no file is better than every other. Nothing is "
				"recommended: decide yourself which to keep.") :
			g_strdup_printf("Axis cannot tell which file is better: %s and %s %s. Nothing is recommended: decide "
				"yourself which to keep.", first, second, why));
	} else {
		const AxisImportDupFile *best = g_ptr_array_index(group->files, keep);
		g_autofree gchar *name = file_name(best);

		if (best->unseen) {
			/* Never on the strength of a file that cannot be seen. */
			set_text(&group->summary, g_strdup_printf("The better file by what Axis recorded is %s (%s), but %s: Axis "
				"cannot see it now, so nothing is recommended until it can.", name, quality_text(best),
				best->fixed != NULL ? best->fixed : "its disk cannot be read now"));
			keep = -1;
		} else {
			g_autoptr(GString) why = g_string_new(NULL);

			for (j = 0; j < group->files->len; j++) {
				g_autofree gchar *other = NULL;

				if ((gint)j == keep)
					continue;
				other = file_name(g_ptr_array_index(group->files, j));
				compare(group, best, g_ptr_array_index(group->files, j), &rule, &doubt);
				g_string_append_printf(why, "%sagainst %s, %s", why->len > 0 ? "; " : "", other, rule);
			}
			set_text(&group->summary, g_strdup_printf("Axis would keep %s (%s): %s.", name, quality_text(best),
				why->str));
			group->keep = keep;
		}
	}
	for (i = 0; i < group->files->len; i++) {
		AxisImportDupFile *f = g_ptr_array_index(group->files, i);

		f->ticked = FALSE;
		if (f->fixed != NULL) {
			f->advice = AXIS_IMPORT_DUP_STAYS;
			set_text(&f->advice_text, (gint)i == group->keep ?
				g_strdup_printf("Keep. It also cannot be removed here: %s.", f->fixed) :
				g_strdup_printf("Stays, whatever Axis would recommend: %s.", f->fixed));
		} else if (group->keep < 0) {
			f->advice = AXIS_IMPORT_DUP_NONE;
			set_text(&f->advice_text, g_strdup("No recommendation."));
		} else if ((gint)i == group->keep) {
			f->advice = AXIS_IMPORT_DUP_KEEP;
			set_text(&f->advice_text, g_strdup("Keep."));
		} else {
			compare(group, g_ptr_array_index(group->files, group->keep), f, &rule, &doubt);
			if (by_hand(f)) {
				g_autofree gchar *hand = hand_words(f);

				f->advice = AXIS_IMPORT_DUP_REMOVE_HAND;
				set_text(&f->advice_text, g_strdup_printf("Axis would remove it (of the file to keep: %s), but %s. It is "
					"not ticked for you: tick it yourself only if you mean to remove it.", rule, hand));
			} else {
				f->advice = AXIS_IMPORT_DUP_REMOVE;
				f->ticked = TRUE;
				set_text(&f->advice_text, g_strdup_printf("Remove. Of the file to keep: %s.", rule));
			}
		}
	}
}

/* ── What the disks say ─────────────────────────────────────────── */

/* The file's disk: its name, whether the file can be looked at there now,
 * and why it is never offered for removal. */
static void
look(
	AxisImport         *self,
	AxisImportDupFile  *f,
	gboolean            read_only
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, f->file->volume_id);
	const gchar *name = volume != NULL && volume->name != NULL ? volume->name : "its disk";

	set_text(&f->volume_name, volume != NULL ? g_strdup(volume->name) : NULL);
	f->unseen = volume == NULL || volume->state == AXIS_VOLUME_OFFLINE || volume->state == AXIS_VOLUME_MISMATCH;
	g_clear_pointer(&f->fixed, g_free);
	if (volume == NULL)
		f->fixed = g_strdup("its disk is not one Axis knows any more");
	else if (volume->state == AXIS_VOLUME_OFFLINE)
		f->fixed = g_strdup_printf("its disk, %s, is offline", name);
	else if (volume->state == AXIS_VOLUME_MISMATCH)
		f->fixed = g_strdup_printf("something else is where its disk, %s, should be", name);
	else if (f->held != NULL)
		f->fixed = g_strdup(f->held);
	else if (f->pinned_links)
		f->fixed = g_strdup("it has another name on its disk (a hard link), so something else still uses it");
	else if (f->pinned_downloads)
		f->fixed = g_strdup("it lies in a downloads folder, where its download client still holds it (it may seed)");
	else if (volume->state == AXIS_VOLUME_READ_ONLY)
		f->fixed = g_strdup_printf("its disk, %s, is mounted read-only", name);
	else if (read_only)
		f->fixed = g_strdup("Read-only storage is on (Settings, Storage), and Axis writes nothing to any disk while "
			"it is");
}

/* The file's revision and score, as an upgrade reads them from the name it
 * came in with (a file with no such name: the original, score 0); not known
 * without a usable profile. */
static void
rank(
	const AxisImportDupGroup  *group,
	AxisImportDupFile         *f
){
	g_autoptr(AxisRelease) fields = NULL;
	g_autoptr(AxisQualityVerdict) verdict = NULL;
	AxisQualityContext context;
	AxisQualityCandidate candidate;

	f->ranked = group->profile_ok;
	f->revision = 0;
	f->score = 0;
	if (!group->profile_ok || f->file->release_name == NULL || f->file->release_name[0] == '\0')
		return;
	fields = axis_release_parse(f->file->release_name);
	if (fields == NULL)
		return;
	memset(&context, 0, sizeof(context));
	context.profile = &group->profile;
	context.protocol = AXIS_QUALITY_PROTOCOL_ANY;
	context.now_us = g_get_real_time();
	memset(&candidate, 0, sizeof(candidate));
	candidate.release = fields;
	candidate.name = f->file->release_name;
	candidate.match = AXIS_RELEASE_MATCH_YES;
	candidate.size_bytes = -1;
	candidate.seeders = -1;
	verdict = axis_quality_decide_one(&context, &candidate);
	if (verdict == NULL)
		return;
	f->revision = verdict->keys.revision;
	f->score = verdict->score;
}

static void
judge(
	AxisImport          *self,
	AxisImportDupGroup  *group
){
	g_autoptr(GString) text = g_string_new(NULL);
	AxisImportSettings settings;
	gboolean read_only = axis_storage_is_read_only_mode(self->storage);
	guint i;

	axis_import_settings_read(self, &settings);
	group->retention_s = settings.retention_s;
	axis_import_settings_clear(&settings);
	for (i = 0; i < group->files->len; i++) {
		AxisImportDupFile *f = g_ptr_array_index(group->files, i);

		look(self, f, read_only);
		rank(group, f);
	}
	axis_import_dup_recommend(group);
	/* Everything the review showed and the recommendation stood on. */
	g_string_append_printf(text, "%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%d|%d",
		group->module != NULL ? group->module : "", group->owner_id, group->item_id, group->title_id, group->keep,
		group->retention_s == 0 ? 0 : 1);
	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);

		g_string_append_printf(text, "\n%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT
			"|%s|%d|%d|%s|%d%d|%d|%s|%s", f->file->library_file_id, f->file->volume_id, f->file->inode, f->file->size,
			quality_text(f), (gint)f->file->resolution, f->file->kept ? 1 : 0,
			f->hand_how != NULL ? f->hand_how : "", f->pinned_links ? 1 : 0, f->pinned_downloads ? 1 : 0, (gint)f->advice,
			f->fixed != NULL ? f->fixed : "", f->file->rel_path != NULL ? f->file->rel_path : "");
		/* Who linked it, what else it holds, what the analysis says of it,
		 * and how it ranks: each on a line of its own, as any of them may
		 * hold a separator. */
		g_string_append_printf(text, "\n by %s\n also %s\n probe %s|%" G_GINT64_FORMAT "\n rank %d|%d|%d",
			f->hand_by != NULL ? f->hand_by : "", f->also != NULL ? f->also : "", f->probe != NULL ? f->probe : "",
			f->duration_ms, f->ranked ? 1 : 0, f->revision, f->score);
	}
	set_text(&group->digest, g_compute_checksum_for_string(G_CHECKSUM_MD5, text->str, -1));
}

/* An owner's groups of @owner_id (0: all), judged, appended to @groups. */
static gboolean
collect(
	AxisImport             *self,
	AxisDbConn             *conn,
	const AxisImportOwner  *owner,
	gint64                  owner_id,
	GPtrArray              *groups,
	GError                **error
){
	guint from = groups->len;
	guint i;

	if (owner->info->dup_list == NULL)
		return TRUE;
	if (!owner->info->dup_list(owner->data, conn, owner_id, groups, error))
		return FALSE;
	for (i = from; i < groups->len; i++) {
		AxisImportDupGroup *group = g_ptr_array_index(groups, i);

		/* A group the owner's limit cut down to one file is not shown as a
		 * duplicate: its other files were not read. */
		if (group->files->len < 2) {
			g_ptr_array_remove_index(groups, i--);
			continue;
		}
		group->module = owner->info->module;
		group->noun = owner->info->noun;
		judge(self, group);
	}
	return TRUE;
}

gboolean
axis_import_duplicates_list(
	AxisImport  *self,
	GPtrArray  **groups,
	GError     **error
){
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_dup_group_free);
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	g_return_val_if_fail(self != NULL && groups != NULL, FALSE);

	*groups = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	for (i = 0; ok && i < self->owners->len; i++)
		ok = collect(self, conn, &g_array_index(self->owners, AxisImportOwner, i), 0, found, error);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*groups = g_steal_pointer(&found);
	return ok;
}

/* The one group of @owner_id that is @item_id, or %NULL in *@group. */
static gboolean
find_group(
	AxisImport             *self,
	AxisDbConn             *conn,
	const AxisImportOwner  *owner,
	gint64                  owner_id,
	gint64                  item_id,
	AxisImportDupGroup    **group,
	GError                **error
){
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_dup_group_free);
	guint i;

	*group = NULL;
	if (!collect(self, conn, owner, owner_id, found, error))
		return FALSE;
	for (i = 0; i < found->len; i++) {
		AxisImportDupGroup *g = g_ptr_array_index(found, i);

		if (g->owner_id == owner_id && g->item_id == item_id && g->files->len >= 2) {
			*group = g_ptr_array_steal_index(found, i);
			break;
		}
	}
	return TRUE;
}

/* An import of the group's title that is open may replace or retire these
 * very files: nothing is removed beside it. */
static gboolean
import_open(
	AxisDbConn                *conn,
	const AxisImportDupGroup  *group,
	gchar                    **busy,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *owner = axis_import_int_text(group->owner_id);
	g_autofree gchar *title = axis_import_int_text(group->title_id);
	const gchar *params[3];
	const gchar *state;

	*busy = NULL;
	params[0] = group->module;
	params[1] = owner;
	params[2] = title;
	if (!axis_db_conn_exec(conn, "SELECT id, state FROM import WHERE module = $1 AND (owner_id = $2::bigint"
	        " OR ($3::bigint > 0 AND title_id = $3::bigint)) AND state IN ('waiting', 'running', 'blocked', 'partial')"
	        " ORDER BY id LIMIT 1", 3, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	state = axis_db_result_get_text(rows, 0, 1);
	*busy = g_strdup_printf("An import of %s is open (/imports/%s: %s). It may replace these very files. Let it finish, "
		"or dismiss it there, then look again.", group->title_label != NULL ? group->title_label : "this title",
		axis_db_result_get_text(rows, 0, 0), g_strcmp0(state, "running") == 0 ? "running" :
		g_strcmp0(state, "blocked") == 0 ? "waiting for a person to decide" : g_strcmp0(state, "partial") == 0 ?
		"partly imported" : "waiting to go on");
	return TRUE;
}

gboolean
axis_import_duplicates_review(
	AxisImport          *self,
	const gchar         *module,
	gint64               owner_id,
	gint64               item_id,
	AxisImportDupGroup **group,
	GError             **error
){
	const AxisImportOwner *owner;
	g_autoptr(AxisImportDupGroup) found = NULL;
	g_autofree gchar *busy = NULL;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && group != NULL, FALSE);

	*group = NULL;
	owner = module != NULL ? axis_import_find_owner(self, module) : NULL;
	if (owner == NULL || owner->info->dup_list == NULL || owner->info->dup_guard == NULL || owner_id <= 0 || item_id < 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* Read as a removal would read it, under the title's lock; nothing is
	 * written, and the transaction is rolled back. */
	ok = axis_db_conn_begin(conn, error) &&
		owner->info->dup_guard(owner->data, conn, owner_id, item_id, NULL, NULL, &busy, error) &&
		find_group(self, conn, owner, owner_id, item_id, &found, error);
	if (ok && found != NULL && busy == NULL)
		ok = import_open(conn, found, &busy, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (found != NULL) {
		found->busy = g_steal_pointer(&busy);
		*group = g_steal_pointer(&found);
	}
	return TRUE;
}

/* ── The removal ────────────────────────────────────────────────── */

/* Why @f is not a file a title can be left with, or %NULL when it is: on a
 * disk that is online, not empty, read by the analysis as it is now, and on
 * its disk at this moment as the library records it.  A row of the index
 * that says "present" is not enough: it is what the last scan saw. */
static const gchar *
cannot_stay(
	AxisImport               *self,
	const AxisImportDupFile  *f
){
	const gchar *why;

	if (f->unseen)
		return "is on a disk Axis cannot read now";
	why = unreadable_words(f);
	if (why != NULL)
		return why;
	if (!axis_import_library_file_seen(self, f->file))
		return "is not on its disk where the library records it";
	return NULL;
}

/* Why the ticks cannot be taken as they are (nothing is removed then), or
 * %NULL; each file's @ticked is set as posted. */
static gchar *
ticks_refused(
	AxisImport          *self,
	AxisImportDupGroup  *group,
	const GArray        *remove
){
	g_autofree gchar *unfit = NULL;
	guint ticked = 0;
	gboolean stays = FALSE;
	guint i;
	guint j;

	for (i = 0; i < group->files->len; i++)
		((AxisImportDupFile *)g_ptr_array_index(group->files, i))->ticked = FALSE;
	if (remove == NULL || remove->len == 0)
		return g_strdup("Nothing was ticked, so nothing was removed. Tick the files to remove.");
	/* The title stays locked while the files move: a bounded piece of work. */
	if (remove->len > AXIS_IMPORT_DUP_MAX_REMOVE)
		return g_strdup_printf("More than %d files were ticked. Axis removes at most %d at a time, as the title is "
			"locked while they move. Nothing was removed: tick fewer, then come back for the rest.",
			AXIS_IMPORT_DUP_MAX_REMOVE, AXIS_IMPORT_DUP_MAX_REMOVE);
	for (j = 0; j < remove->len; j++) {
		gint64 id = g_array_index(remove, gint64, j);
		AxisImportDupFile *named = NULL;

		for (i = 0; i < group->files->len && named == NULL; i++) {
			AxisImportDupFile *f = g_ptr_array_index(group->files, i);

			if (f->file->library_file_id == id)
				named = f;
		}
		if (named == NULL)
			return g_strdup_printf("A file named in the request is not one of the files of %s. Nothing was removed.",
				group->label);
		if (named->fixed != NULL) {
			g_autofree gchar *name = file_name(named);

			return g_strdup_printf("%s cannot be removed here: %s. Nothing was removed.", name, named->fixed);
		}
		if (!named->ticked)
			ticked++;
		named->ticked = TRUE;
	}
	/* The guard this page stands on: one file of the title always stays. */
	if (ticked >= group->files->len)
		return g_strdup_printf("Every file of %s was ticked. One must stay: this page never removes the last file of a "
			"title. Nothing was removed.", group->label);
	for (i = 0; i < group->files->len && !stays; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		const gchar *why;

		if (f->ticked)
			continue;
		why = cannot_stay(self, f);
		if (why == NULL) {
			stays = TRUE;
		} else if (unfit == NULL) {
			g_autofree gchar *name = file_name(f);

			unfit = g_strdup_printf("%s %s", name, why);
		}
	}
	if (!stays)
		return g_strdup_printf("No file of %s that would stay is one Axis can leave it with: %s. A file that stays must "
			"be one that can be seen on its disk now and that the analysis could read. Nothing was removed: scan the "
			"library and let it be analysed, then look again.", group->label, unfit != NULL ? unfit : "none stays");
	return NULL;
}

/* Why the ticked files cannot go for the sake of what they hold, or %NULL:
 * each of @others (the owner's, read with the staying files' links locked:
 * the movie, or every episode a ticked file holds, the group's own among
 * them, each with the files that would still be linked to it) keeps a file
 * that can stay. */
static gchar *
others_refused(
	AxisImport  *self,
	GPtrArray   *others
){
	gboolean read_only = axis_storage_is_read_only_mode(self->storage);
	guint i;
	guint j;

	for (i = 0; i < others->len; i++) {
		const AxisImportDupGroup *other = g_ptr_array_index(others, i);
		g_autofree gchar *unfit = NULL;
		gboolean stays = FALSE;

		for (j = 0; j < other->files->len && !stays; j++) {
			AxisImportDupFile *f = g_ptr_array_index(other->files, j);
			const gchar *why;

			look(self, f, read_only);
			why = cannot_stay(self, f);
			if (why == NULL) {
				stays = TRUE;
			} else if (unfit == NULL) {
				g_autofree gchar *name = file_name(f);

				unfit = g_strdup_printf("%s %s", name, why);
			}
		}
		if (stays)
			continue;
		if (other->files->len == 0)
			return g_strdup_printf("Removing these files would leave %s without a file: one of them is its only file. "
				"Nothing was removed.", other->label != NULL ? other->label : "another episode");
		return g_strdup_printf("Removing these files would leave %s without a file Axis can see on its disk and read "
			"now: the file it would be left with, %s. Nothing was removed: scan the library and let it be analysed, then "
			"look again.",
			other->label != NULL ? other->label : "another episode", unfit);
	}
	return NULL;
}

static gchar *
file_line(const AxisImportDupFile *f)
{
	g_autofree gchar *shown = axis_library_name_display(f->file->rel_path);
	g_autofree gchar *size = axis_size_format((guint64)MAX(f->file->size, 0));

	return g_strdup_printf("%s (%s, %s)", shown != NULL ? shown : "a file", quality_text(f), size);
}

/* Where a removed file is now: ".axis-recycle/films/... on Disk one". */
static gchar *
recycled_words(const AxisImportDupFile *f)
{
	g_autofree gchar *shown = f->recycle_path != NULL ? axis_library_name_display(f->recycle_path) : NULL;

	return g_strdup_printf("%s on %s", shown != NULL ? shown : "the recycle folder", f->volume_name != NULL ?
		f->volume_name : "its disk");
}

/* Each ticked file retired: moved, and its record made on @conn, the
 * transaction that holds the title and the files' links.  Each file says
 * how it went (@removed, @unrecorded, @failure). */
static void
retire_ticked(
	AxisImport          *self,
	AxisDbConn          *conn,
	AxisImportDupGroup  *group,
	const gchar         *user
){
	guint i;

	for (i = 0; i < group->files->len; i++) {
		AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autofree gchar *line = file_line(f);
		g_autoptr(GError) local = NULL;
		gboolean moved = FALSE;

		if (!f->ticked)
			continue;
		if (axis_import_retire_duplicate(self, conn, f->file, group->label, user, &f->recycle_row, &f->recycle_path,
		        &moved, &local)) {
			g_autofree gchar *where = recycled_words(f);

			f->removed = TRUE;
			g_message("duplicates: %s retired %s of %s into the recycle folder (%s)", user != NULL ? user : "a person",
				line, group->label, where);
		} else if (moved) {
			/* In the recycle folder, its record not written: said as it is. */
			f->unrecorded = TRUE;
			f->failure = g_strdup(local != NULL ? local->message : "its record could not be written");
		} else {
			f->failure = g_strdup(local != NULL ? local->message : "it could not be moved");
		}
	}
}

/* The title's transaction is over without its commit (the connection was
 * lost, or the history could not be written on it): the records it held of
 * the files that were moved went with it.  Each is made again, on a
 * connection of its own, and each file says how that went. */
static void
record_again(
	AxisImport          *self,
	AxisImportDupGroup  *group,
	const gchar         *user
){
	guint i;

	for (i = 0; i < group->files->len; i++) {
		AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autoptr(GError) local = NULL;

		if ((!f->removed && !f->unrecorded) || f->recycle_path == NULL)
			continue;
		if (axis_import_record_duplicate(self, NULL, f->recycle_row, f->recycle_path, f->file, group->label, user,
		        &local)) {
			f->removed = TRUE;
			f->unrecorded = FALSE;
			g_clear_pointer(&f->failure, g_free);
		} else {
			g_autofree gchar *shown = axis_library_name_display(f->recycle_path);

			f->removed = FALSE;
			f->unrecorded = TRUE;
			set_text(&f->failure, g_strdup_printf("It was moved into the recycle folder of its disk (%s), but the "
				"database could not be reached to record that (%s). It is no longer at its place, though the library "
				"still lists it and its link remains for now: the clean-up job finishes the record within the hour, and "
				"deletes the file after the retention time counted from then.", shown,
				local != NULL ? local->message : "unknown"));
			g_warning("duplicates: a file was moved into the recycle folder (%s), but its records could not be "
				"updated: %s", shown, local != NULL ? local->message : "?");
		}
	}
}

/* What the title's history is told, from how each file went; %NULL when no
 * file was moved. */
static gchar *
event_text(
	const AxisImportDupGroup  *group,
	const gchar               *user
){
	g_autoptr(GString) went = g_string_new(NULL);
	g_autoptr(GString) unrecorded = g_string_new(NULL);
	g_autoptr(GString) failed = g_string_new(NULL);
	g_autoptr(GString) stays = g_string_new(NULL);
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) until = g_date_time_add_seconds(now, (gdouble)group->retention_s);
	g_autofree gchar *when = until != NULL ? g_date_time_format(until, "%Y-%m-%d %H:%M UTC") : NULL;
	gchar *event = NULL;
	guint removed = 0;
	guint moved_only = 0;
	guint i;

	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autofree gchar *line = file_line(f);
		g_autofree gchar *where = f->removed || f->unrecorded ? recycled_words(f) : NULL;

		if (f->removed) {
			removed++;
			g_string_append_printf(went, "%s%s, now %s", went->len > 0 ? "; " : "", line, where);
		} else if (f->unrecorded) {
			moved_only++;
			g_string_append_printf(unrecorded, "%s%s, now %s", unrecorded->len > 0 ? "; " : "", line, where);
		} else if (f->ticked) {
			g_string_append_printf(failed, "%s%s: %s", failed->len > 0 ? "; " : "", line,
				f->failure != NULL ? f->failure : "it could not be moved");
		} else {
			g_string_append_printf(stays, "%s%s", stays->len > 0 ? "; " : "", line);
		}
	}
	if (removed + moved_only > 0) {
		GString *text = g_string_new(NULL);

		g_string_append_printf(text, "%s removed %s of %s on the Duplicates page.", user != NULL ? user : "A person",
			removed + moved_only == 1 ? "a duplicate file" : "duplicate files", group->label);
		if (removed > 0 && group->retention_s == 0)
			g_string_append_printf(text, " Deleted for good at the next cleanup, as the recycle folder keeps nothing "
				"(import.recycle_retention is 0): %s.", went->str);
		else if (removed > 0)
			g_string_append_printf(text, " Moved into the recycle folder of its disk, from where it is deleted after "
				"%s (until then it can be moved back by hand; Duplicates, Recycled duplicates lists it): %s.",
				when != NULL ? when : "the retention time", went->str);
		if (moved_only > 0)
			g_string_append_printf(text, " Moved into the recycle folder, but the record of it could not be written, "
				"so the library still lists it until the cleanup job finishes the record: %s.", unrecorded->str);
		if (failed->len > 0)
			g_string_append_printf(text, " Could not be moved, and stays as it was: %s.", failed->str);
		g_string_append_printf(text, " Stays linked: %s.", stays->str);
		event = g_string_free(text, FALSE);
	}
	return event;
}

/* The removal is committed and the title's history told.  On @conn, the
 * transaction that holds the title and the records of the files that were
 * moved: the history and the records are committed together.  When that
 * fails (the connection was lost while the files moved), the transaction is
 * rolled back, the records are made again each on a connection of its own
 * (record_again()), and the history, told anew from how that went, is
 * written on another connection.  %FALSE when that failed too: the event is
 * in the log only.  @conn's transaction is over either way. */
static gboolean
tell_history(
	AxisImport             *self,
	const AxisImportOwner  *owner,
	AxisDbConn             *conn,
	AxisImportDupGroup     *group,
	const gchar            *user
){
	g_autoptr(GError) first = NULL;
	g_autoptr(GError) second = NULL;
	g_autofree gchar *event = event_text(group, user);
	AxisImportTarget target;
	AxisDbConn *other;
	gboolean ok;

	memset(&target, 0, sizeof(target));
	target.owner_id = group->owner_id;
	target.title_id = group->title_id;
	target.label = group->title_label;
	if (axis_import_hook(self, "duplicate_history", 0, 0) != AXIS_IMPORT_HOOK_GO)
		g_set_error_literal(&first, G_IO_ERROR, G_IO_ERROR_FAILED, "the test's hook failed the history");
	else if (owner->info->note(owner->data, conn, &target, "hand_link", event, user, &first) &&
	    axis_db_conn_commit(conn, &first))
		return TRUE;
	/* The title's lock goes before another connection writes of the title,
	 * and with the transaction go the records it held. */
	axis_db_conn_rollback(conn, NULL);
	record_again(self, group, user);
	g_free(event);
	event = event_text(group, user);
	other = axis_db_pool_acquire(self->pool, &second);
	ok = other != NULL;
	if (ok && axis_import_hook(self, "duplicate_history", 0, 0) != AXIS_IMPORT_HOOK_GO) {
		g_set_error_literal(&second, G_IO_ERROR, G_IO_ERROR_FAILED, "the test's hook failed the history");
		ok = FALSE;
	}
	ok = ok && axis_db_conn_begin(other, &second) &&
		owner->info->note(owner->data, other, &target, "hand_link", event, user, &second) &&
		axis_db_conn_commit(other, &second);
	if (other != NULL) {
		axis_db_conn_rollback(other, NULL);
		axis_db_pool_release(self->pool, other);
	}
	if (!ok)
		g_warning("duplicates: the history of %s could not be written after its files were retired (%s; then: %s). "
			"The event: %s", group->label, first != NULL ? first->message : "?",
			second != NULL ? second->message : "?", event);
	return ok;
}

gboolean
axis_import_duplicates_remove(
	AxisImport            *self,
	const gchar           *module,
	gint64                 owner_id,
	gint64                 item_id,
	const GArray          *remove,
	const gchar           *digest,
	const gchar           *user,
	AxisImportDupOutcome  *outcome,
	gchar                **refusal,
	AxisImportDupGroup   **group,
	GError               **error
){
	const AxisImportOwner *owner;
	g_autoptr(AxisImportDupGroup) found = NULL;
	g_autoptr(GPtrArray) others = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_dup_group_free);
	g_autofree gchar *why = NULL;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && outcome != NULL && refusal != NULL && group != NULL, FALSE);

	*outcome = AXIS_IMPORT_DUP_GONE;
	*refusal = NULL;
	*group = NULL;
	owner = module != NULL ? axis_import_find_owner(self, module) : NULL;
	if (owner == NULL || owner->info->dup_list == NULL || owner->info->dup_guard == NULL ||
	    owner->info->note == NULL || owner_id <= 0 || item_id < 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The title locked, then everything read again under the lock. */
	ok = axis_db_conn_begin(conn, error) &&
		owner->info->dup_guard(owner->data, conn, owner_id, item_id, NULL, NULL, &why, error) &&
		find_group(self, conn, owner, owner_id, item_id, &found, error);
	if (ok && found != NULL && why == NULL)
		ok = import_open(conn, found, &why, error);
	if (ok && found != NULL) {
		if (why != NULL) {
			*outcome = AXIS_IMPORT_DUP_REFUSED;
			found->busy = g_strdup(why);
		} else if (digest == NULL || g_strcmp0(found->digest, digest) != 0) {
			*outcome = AXIS_IMPORT_DUP_CHANGED;
		} else if ((why = ticks_refused(self, found, remove)) != NULL) {
			*outcome = AXIS_IMPORT_DUP_REFUSED;
		} else {
			/* The owner again, with the files named: what would be left to
			 * every episode one of them holds.  Whether that is a file that
			 * can stay is decided here, by the same test as for the group. */
			axis_import_hook(self, "duplicate_checked", 0, 0);
			ok = owner->info->dup_guard(owner->data, conn, owner_id, item_id, remove, others, &why, error);
			if (ok && why == NULL)
				why = others_refused(self, others);
			if (ok && why != NULL) {
				*outcome = AXIS_IMPORT_DUP_REFUSED;
			} else if (ok) {
				gboolean any = FALSE;
				guint i;

				retire_ticked(self, conn, found, user);
				*outcome = AXIS_IMPORT_DUP_DONE;
				for (i = 0; i < found->files->len; i++) {
					const AxisImportDupFile *f = g_ptr_array_index(found->files, i);

					any = any || f->removed || f->unrecorded;
				}
				/* The files went, whatever becomes of the history: the
				 * outcome says so, and the group says when the history
				 * could not be written. */
				if (any)
					found->history_lost = !tell_history(self, owner, conn, found, user);
			}
		}
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*refusal = g_steal_pointer(&why);
	*group = g_steal_pointer(&found);
	return TRUE;
}

/* ── What was removed ───────────────────────────────────────────── */

gboolean
axis_import_duplicates_recycled(
	AxisImport  *self,
	GPtrArray  **items,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	GPtrArray *found;
	guint i;

	g_return_val_if_fail(self != NULL && items != NULL, FALSE);

	*items = NULL;
	if (!axis_db_pool_exec(self->pool, "SELECT c.original_path, c.recycle_path, c.how, c.state, c.size, "
	        US("c.retired_at") ", " US("c.remove_after") ", c.note, c.label, c.by_user, v.name, c.volume_id, "
	        US("c.removed_at") " FROM import_recycle c LEFT JOIN volume v ON v.id = c.volume_id"
	        " WHERE c.label IS NOT NULL ORDER BY c.retired_at DESC, c.id DESC LIMIT 500", 0, NULL, &rows, error))
		return FALSE;
	found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_recycled_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportRecycledInfo *item = g_new0(AxisImportRecycledInfo, 1);

		item->original_path = g_strdup(axis_db_result_get_text(rows, i, 0));
		item->recycle_path = g_strdup(axis_db_result_get_text(rows, i, 1));
		item->how = g_strdup(axis_db_result_get_text(rows, i, 2));
		item->state = g_strdup(axis_db_result_get_text(rows, i, 3));
		axis_db_result_get_int64(rows, i, 4, &item->size);
		if (!axis_db_result_is_null(rows, i, 5))
			axis_db_result_get_int64(rows, i, 5, &item->retired_us);
		if (!axis_db_result_is_null(rows, i, 6))
			axis_db_result_get_int64(rows, i, 6, &item->remove_after_us);
		item->note = g_strdup(axis_db_result_get_text(rows, i, 7));
		item->label = g_strdup(axis_db_result_get_text(rows, i, 8));
		item->by_user = g_strdup(axis_db_result_get_text(rows, i, 9));
		item->volume_name = g_strdup(axis_db_result_get_text(rows, i, 10));
		axis_db_result_get_int64(rows, i, 11, &item->volume_id);
		if (!axis_db_result_is_null(rows, i, 12))
			axis_db_result_get_int64(rows, i, 12, &item->removed_us);
		g_ptr_array_add(found, item);
	}
	*items = found;
	return TRUE;
}
