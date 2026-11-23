/*
 * axis-acquire-hand.c - a person's own look and grab: where a stored
 * release stands on a manual search's page, what a grab by hand of it
 * would do, and Grab this one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A manual search (cause "list") stores every release as any search does
 * and grabs nothing.  A person may then take any release that is this
 * title, whatever the profile and the module's own rules say: what a fixed
 * rule rules out and what is on the blocklist stay out.  The confirmation
 * says what the grab fills and what it replaces; replacing a file it is not
 * better than needs a tick of its own, which the grab records for those
 * very files (replaces_files, with the quality shown), and so does a
 * release the module warns is numbered otherwise than its data
 * (numbering_ok).  The grab is marked set_aside "hand": only such a grab's
 * import sets the profile aside.
 */

#include "acquire/axis-acquire-private.h"

#include <string.h>

AxisAcquireReplaced *
axis_acquire_replaced_new(
	gint64                 file_id,
	const gchar           *label,
	AxisQuality            quality,
	AxisReleaseResolution  resolution
){
	AxisAcquireReplaced *replaced = g_new0(AxisAcquireReplaced, 1);

	replaced->file_id = file_id;
	replaced->label = g_strdup(label);
	replaced->quality = quality;
	replaced->resolution = resolution;
	return replaced;
}

void
axis_acquire_replaced_free(AxisAcquireReplaced *replaced)
{
	if (replaced == NULL)
		return;
	g_free(replaced->label);
	g_free(replaced->kept_by);
	g_free(replaced);
}

gchar *
axis_acquire_replaced_kept_words(const AxisAcquireReplaced *replaced)
{
	if (replaced->kept_from != NULL)
		return g_strdup_printf("kept as it was migrated from %s", replaced->kept_from);
	return g_strdup_printf("kept by hand by %s", replaced->kept_by != NULL ? replaced->kept_by : "a person");
}

AxisAcquireChoice *
axis_acquire_choice_new(
	gint64       id,
	const gchar *label,
	gboolean     whole,
	gboolean     picked
){
	AxisAcquireChoice *choice = g_new0(AxisAcquireChoice, 1);

	choice->id = id;
	choice->label = g_strdup(label);
	choice->whole = whole;
	choice->picked = picked;
	return choice;
}

void
axis_acquire_choice_free(AxisAcquireChoice *choice)
{
	if (choice == NULL)
		return;
	g_free(choice->label);
	g_free(choice);
}

void
axis_acquire_hand_preview_free(AxisAcquireHandPreview *preview)
{
	if (preview == NULL)
		return;
	axis_acquire_candidate_free(preview->candidate);
	g_free(preview->refused);
	g_clear_pointer(&preview->fills, g_ptr_array_unref);
	g_clear_pointer(&preview->replaces, g_ptr_array_unref);
	g_free(preview->missing);
	g_free(preview->numbering);
	g_free(preview->profile_name);
	g_free(preview->digest);
	g_free(preview->owner_label);
	g_clear_pointer(&preview->choices, g_ptr_array_unref);
	g_free(preview->pick_help);
	g_free(preview->pick_link);
	g_free(preview->pick_link_text);
	g_free(preview->bound_text);
	g_free(preview->import_text);
	g_free(preview->digest_more);
	g_free(preview->flying);
	g_clear_pointer(&preview->standing, g_ptr_array_unref);
	g_free(preview->standing_text);
	g_free(preview->replace_digest);
	g_free(preview);
}

gchar *
axis_acquire_replaces_json(
	const AxisAcquireHandPreview *preview,
	gboolean                      all,
	guint                        *n
){
	GString *json = g_string_new("[");
	guint i;

	*n = 0;
	for (i = 0; preview->replaces != NULL && i < preview->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);

		/* A file kept by hand is recorded whenever the grab is made: it was
		 * ticked by name, or the grab was refused. */
		if ((!all && r->better > 0 && r->kept_by == NULL) || r->file_id <= 0)
			continue;
		g_string_append_printf(json, "%s{\"f\": %" G_GINT64_FORMAT ", \"q\": ", *n > 0 ? ", " : "", r->file_id);
		if (r->quality != AXIS_QUALITY_UNKNOWN)
			g_string_append_printf(json, "\"%s\"", axis_quality_name(r->quality));
		else
			g_string_append(json, "null");
		if (r->resolution != AXIS_RELEASE_RESOLUTION_NONE)
			g_string_append_printf(json, ", \"r\": \"%s\"}", axis_release_resolution_name(r->resolution));
		else
			g_string_append(json, ", \"r\": null}");
		(*n)++;
	}
	g_string_append_c(json, ']');
	return g_string_free(json, FALSE);
}

/* ── The groups ─────────────────────────────────────────────────── */

static gboolean
has_reason(
	const AxisAcquireCandidate *c,
	const gchar                *kind,
	const gchar                *code
){
	guint i;

	for (i = 0; c->reasons != NULL && i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);

		if (g_strcmp0(r->kind, kind) == 0 && (code == NULL || g_strcmp0(r->code, code) == 0))
			return TRUE;
	}
	return FALSE;
}

AxisAcquireGroup
axis_acquire_candidate_group(const AxisAcquireCandidate *candidate)
{
	if (candidate->safety || candidate->blocklisted)
		return AXIS_ACQUIRE_GROUP_RULED_OUT;
	if (candidate->accepted)
		return AXIS_ACQUIRE_GROUP_ACCEPTABLE;
	/* The module's own rejection (what was asked for, what is wanted now)
	 * comes before the profile's: it is why the search left it. */
	if (has_reason(candidate, "wanted", NULL))
		return AXIS_ACQUIRE_GROUP_NOT_ASKED;
	return AXIS_ACQUIRE_GROUP_PROFILE;
}

/* ── What a grab by hand would do ───────────────────────────────── */

gboolean
axis_acquire_preview_fill(
	AxisAcquire                *self,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
){
	AxisQuality release = AXIS_QUALITY_UNKNOWN;
	gboolean not_better = has_reason(candidate, "upgrade", "not_better");
	guint i;

	if (preview->fills == NULL)
		preview->fills = g_ptr_array_new_with_free_func(g_free);
	if (preview->replaces == NULL)
		preview->replaces = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_replaced_free);
	if (preview->profile_name == NULL)
		preview->profile_name = g_strdup(want->profile_name);
	if (self->info->hand_preview != NULL &&
	    !self->info->hand_preview(self->data, conn, want, candidate, preview, error))
		return FALSE;
	if (preview->fills->len == 0)
		g_ptr_array_add(preview->fills, g_strdup_printf("the %s", self->info->noun != NULL ? self->info->noun :
			"title"));
	if (candidate->quality == NULL || !axis_quality_from_name(candidate->quality, &release))
		release = AXIS_QUALITY_UNKNOWN;
	/* Better than each file it replaces, by the order of qualities on disk;
	 * a release of unknown quality is not known to be better.  The profile's
	 * own comparison (tiers, revisions, scores) saying not better stands. */
	preview->needs_tick = FALSE;
	for (i = 0; i < preview->replaces->len; i++) {
		AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);
		gint order = release == AXIS_QUALITY_UNKNOWN ? -1 :
			axis_quality_compare_on_disk(release, AXIS_RELEASE_RESOLUTION_NONE, r->quality, r->resolution);

		r->better = order > 0 ? 1 : order < 0 ? -1 : 0;
		if (not_better && r->better > 0)
			r->better = 0;
		if (r->better <= 0)
			preview->needs_tick = TRUE;
	}
	/* What stands in the way, and whether it can be replaced. */
	return axis_acquire_standing_fill(self, conn, want, candidate, preview, error);
}

gchar *
axis_acquire_safety_words(const AxisAcquireCandidate *c)
{
	g_autoptr(GString) text = g_string_new(NULL);
	guint i;

	for (i = 0; c->reasons != NULL && i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);

		if (g_strcmp0(r->kind, "safety") == 0)
			g_string_append_printf(text, "%s%s", text->len > 0 ? " " : "", r->text);
	}
	if (text->len == 0 && c->match_text != NULL)
		g_string_append_printf(text, "%c%s.", g_ascii_toupper(c->match_text[0]), c->match_text + 1);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* Why a release on the blocklist is not grabbed by hand or by force, and
 * what to do about it; %NULL when it is not on it. */
static gchar *
blocked_refusal(
	const GPtrArray            *blocklist,
	const AxisAcquireCandidate *candidate
){
	const AxisAcquireBlockKey *block = axis_acquire_blocked_by(blocklist, candidate->name_key, candidate->info_hash);
	g_autofree gchar *why = NULL;

	if (block == NULL)
		return NULL;
	why = axis_acquire_block_words(block->reason, block->created_by);
	return g_strdup_printf("It is on the blocklist: %s. Take it off the blocklist first to grab it.", why);
}

gboolean
axis_acquire_hand_preview(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview_out,
	GError                  **error
){
	g_autoptr(AxisAcquireWant) want = NULL;
	g_autoptr(GPtrArray) blocklist = NULL;
	g_autoptr(AxisAcquireHandPreview) preview = g_new0(AxisAcquireHandPreview, 1);
	AxisAcquireTarget own = *target;
	const gchar *skipped = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*preview_out = NULL;
	own.by_hand = TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = self->info->load(self->data, conn, &own, &want, &skipped, error);
	if (ok && want == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", skipped != NULL ? skipped : "it was removed");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_candidate(self, conn, want->owner_id, candidate_id, &preview->candidate, error);
	if (ok && preview->candidate == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "that release is not one of its searches'");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_block_keys(self, conn, want->owner_id, &blocklist, error);
	/* What only a fixed rule decides is not set aside by anyone. */
	if (ok && preview->candidate->safety) {
		g_autofree gchar *why = axis_acquire_safety_words(preview->candidate);

		/* The rules about its name are an administrator's to set aside, on a
		 * page of its own (axis_acquire_force_preview()). */
		if (self->info->force && axis_acquire_force_allowed(preview->candidate, NULL))
			preview->refused = g_strdup_printf("A fixed rule rules it out, so Grab this one does not take it: %s%s An "
				"administrator who can tell that it is the %s may grab it by force, from the manual search's list.",
				why, g_str_has_suffix(why, ".") ? "" : ".", self->info->noun != NULL ? self->info->noun : "title");
		else
			preview->refused = g_strdup_printf("A fixed rule rules it out, and no one may grab it: %s", why);
	} else if (ok) {
		preview->refused = blocked_refusal(blocklist, preview->candidate);
	}
	if (ok && preview->refused == NULL)
		ok = axis_acquire_preview_fill(self, conn, want, preview->candidate, preview, error);
	else if (ok && preview->profile_name == NULL)
		preview->profile_name = g_strdup(want->profile_name);
	/* (A grab in flight is said by axis_acquire_preview_fill(): which one, and
	 * whether it can be replaced.) */
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*preview_out = g_steal_pointer(&preview);
	return TRUE;
}

gboolean
axis_acquire_grab_by_hand(
	AxisAcquire                 *self,
	const AxisAcquireTarget     *target,
	gint64                       candidate_id,
	const gchar                 *user,
	const AxisAcquireHandTicks  *ticks,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	gchar                      **problem,
	GError                     **error
){
	AxisAcquireHandGrabbed none;
	AxisAcquireHow how;

	memset(&none, 0, sizeof(none));
	memset(&how, 0, sizeof(how));
	how.by_hand = TRUE;
	how.ticks = ticks;
	how.grabbed = grabbed != NULL ? grabbed : &none;
	memset(how.grabbed, 0, sizeof(*how.grabbed));
	return axis_acquire_grab_stored(self, target, candidate_id, user, &how, NULL, result, problem, error);
}

/* ── A grab by force ────────────────────────────────────────────── */

/*
 * The fixed rules an administrator may set aside, by the stored reason's
 * code: the ones that say "Axis cannot tell that this is the title".  Every
 * other fixed rule (a sample, password-protected, an archive's or a
 * program's extension, one part of a post, and any rule added later) stays
 * for everyone, so a new rule is kept until it is listed here.
 */
static const gchar *const force_codes[] = { "ambiguous", "no_match", "no_title", NULL };

gboolean
axis_acquire_force_allowed(
	const AxisAcquireCandidate  *candidate,
	gchar                      **kept
){
	g_autoptr(GString) stands = g_string_new(NULL);
	guint n = 0;
	guint i;

	if (kept != NULL)
		*kept = NULL;
	if (candidate == NULL || !candidate->safety)
		return FALSE;
	for (i = 0; candidate->reasons != NULL && i < candidate->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(candidate->reasons, i);

		if (g_strcmp0(r->kind, "safety") != 0)
			continue;
		n++;
		if (r->code == NULL || !g_strv_contains(force_codes, r->code))
			g_string_append_printf(stands, "%s%s", stands->len > 0 ? " " : "", r->text != NULL ? r->text : "");
	}
	/* Marked as ruled out with no reason kept: nothing says which rule. */
	if (n == 0) {
		if (kept != NULL)
			*kept = g_strdup("The rule that rules it out was not recorded.");
		return FALSE;
	}
	if (stands->len == 0)
		return TRUE;
	if (kept != NULL)
		*kept = g_string_free(g_steal_pointer(&stands), FALSE);
	return FALSE;
}

gchar *
axis_acquire_forced_json(
	const AxisAcquireCandidate *candidate,
	gboolean                    one_file,
	guint                      *n
){
	g_autoptr(GPtrArray) rules = g_ptr_array_new();
	AxisAcquireReason choice = { (gchar *)"choice", (gchar *)"one_file",
		(gchar *)"The person ticked that one file holds the episodes they picked." };
	guint i;

	for (i = 0; candidate->reasons != NULL && i < candidate->reasons->len; i++) {
		AxisAcquireReason *r = g_ptr_array_index(candidate->reasons, i);

		if (g_strcmp0(r->kind, "safety") == 0)
			g_ptr_array_add(rules, r);
	}
	*n = rules->len;
	if (one_file)
		g_ptr_array_add(rules, &choice);
	return axis_acquire_reasons_to_json(rules);
}

gchar *
axis_acquire_force_digest(
	const AxisAcquireCandidate   *c,
	const AxisAcquireHandPreview *preview,
	const AxisAcquireWant        *want
){
	g_autoptr(GString) text = g_string_new("force");
	guint i;

	g_string_append_printf(text, "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%d|%d", c->id, want->owner_id,
		want->in_flight, c->blocklisted);
	g_string_append_printf(text, "|title:%" G_GINT64_FORMAT ":%s|profile:%" G_GINT64_FORMAT, want->title_id,
		want->label != NULL ? want->label : "", want->profile_id);
	for (i = 0; c->reasons != NULL && i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);

		if (g_strcmp0(r->kind, "safety") == 0)
			g_string_append_printf(text, "|%s:%s", r->code != NULL ? r->code : "", r->text != NULL ? r->text : "");
	}
	for (i = 0; preview->fills != NULL && i < preview->fills->len; i++)
		g_string_append_printf(text, "|for:%s", (const gchar *)g_ptr_array_index(preview->fills, i));
	for (i = 0; preview->replaces != NULL && i < preview->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);

		g_string_append_printf(text, "|file:%" G_GINT64_FORMAT ":%d:%d:%s", r->file_id, (gint)r->quality,
			(gint)r->resolution, r->kept_by != NULL ? r->kept_by : "");
	}
	/* What only the module knows of what the page showed. */
	if (preview->digest_more != NULL)
		g_string_append_printf(text, "|more:%s", preview->digest_more);
	return g_compute_checksum_for_string(G_CHECKSUM_MD5, text->str, -1);
}

gboolean
axis_acquire_force_preview(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview_out,
	GError                  **error
){
	g_autoptr(AxisAcquireWant) want = NULL;
	g_autoptr(GPtrArray) blocklist = NULL;
	g_autoptr(AxisAcquireHandPreview) preview = g_new0(AxisAcquireHandPreview, 1);
	g_autofree gchar *kept = NULL;
	AxisAcquireTarget own = *target;
	const gchar *skipped = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*preview_out = NULL;
	own.by_hand = TRUE;
	preview->force = TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = self->info->load(self->data, conn, &own, &want, &skipped, error);
	if (ok && want == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", skipped != NULL ? skipped : "it was removed");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_candidate(self, conn, want->owner_id, candidate_id, &preview->candidate, error);
	if (ok && preview->candidate == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "that release is not one of its searches'");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_block_keys(self, conn, want->owner_id, &blocklist, error);
	if (ok)
		preview->owner_label = g_strdup(want->label);
	if (ok && !preview->candidate->safety)
		preview->refused = g_strdup("No fixed rule rules it out: use Grab this one on the manual search's list.");
	else if (ok && !axis_acquire_force_allowed(preview->candidate, &kept))
		preview->refused = g_strdup_printf("A fixed rule that protects the system rules it out, and no one may grab it, "
			"by force either: %s%s", kept != NULL ? kept : "", kept != NULL && g_str_has_suffix(kept, ".") ? "" : ".");
	else if (ok)
		preview->refused = blocked_refusal(blocklist, preview->candidate);
	if (ok && preview->refused == NULL)
		ok = axis_acquire_preview_fill(self, conn, want, preview->candidate, preview, error);
	else if (ok && preview->profile_name == NULL)
		preview->profile_name = g_strdup(want->profile_name);
	/* (A grab in flight: as axis_acquire_hand_preview().) */
	if (ok && preview->refused == NULL)
		preview->digest = axis_acquire_force_digest(preview->candidate, preview, want);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*preview_out = g_steal_pointer(&preview);
	return TRUE;
}

gboolean
axis_acquire_grab_by_force(
	AxisAcquire                  *self,
	const AxisAcquireTarget      *target,
	gint64                        candidate_id,
	const gchar                  *user,
	const AxisAcquireForceTicks  *ticks,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result,
	gchar                       **problem,
	GError                      **error
){
	AxisAcquireHandGrabbed none;
	AxisAcquireForceTicks unticked;
	AxisAcquireHow how;

	memset(&none, 0, sizeof(none));
	memset(&unticked, 0, sizeof(unticked));
	memset(&how, 0, sizeof(how));
	how.by_hand = TRUE;
	how.force = ticks != NULL ? ticks : &unticked;
	how.grabbed = grabbed != NULL ? grabbed : &none;
	memset(how.grabbed, 0, sizeof(*how.grabbed));
	return axis_acquire_grab_stored(self, target, candidate_id, user, &how, NULL, result, problem, error);
}
