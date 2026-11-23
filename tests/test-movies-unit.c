/*
 * test-movies-unit.c - a film's state from its facts (the one derivation),
 * availability, the grab-once check's in-flight rule, and the blocklist's
 * name key; no database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Task 4.2 (every state from its conditions; not available before the
 * release date) at the level of the derivation itself; test-movies.c checks
 * the same states from real rows.
 */

#include <glib.h>
#include <string.h>

#include "modules/movies/axis-movies.h"

/* 2026-11-14 12:00 UTC */
#define NOW_US (G_GINT64_CONSTANT(1794657600) * G_USEC_PER_SEC)

static AxisMovieRules
rules_now(void)
{
	AxisMovieRules rules;

	axis_movie_rules_init(&rules, NOW_US);
	return rules;
}

/* A monitored, released film with a destination online, never searched. */
static AxisMovieFacts *
facts_new(void)
{
	AxisMovieFacts *f = g_new0(AxisMovieFacts, 1);

	f->id = 7;
	f->title_id = 101;
	f->title = g_strdup("The Mazework");
	f->year = 1999;
	f->release_date = g_strdup("1999-03-31");
	f->status = AXIS_META_STATUS_RELEASED;
	f->runtime = 136;
	f->profile_id = 2;
	f->profile_name = g_strdup("Balanced");
	f->profile_usable = TRUE;
	f->profile_cutoff = AXIS_QUALITY_BLURAY_1080P;
	f->profile_upgrades = TRUE;
	f->root_id = 3;
	f->root_name = g_strdup("Films");
	f->volume_id = 1;
	f->volume_name = g_strdup("Disk one");
	f->volume_state = AXIS_VOLUME_ONLINE;
	f->monitored = TRUE;
	f->availability = AXIS_MOVIE_RELEASED;
	f->download_size = -1;
	f->download_done = -1;
	return f;
}

static AxisMovieState
derive(
	const AxisMovieFacts *f,
	gchar               **reason
){
	AxisMovieRules rules = rules_now();

	return axis_movie_derive(f, &rules, reason);
}

static void
assert_state(
	const AxisMovieFacts *f,
	AxisMovieState        expected,
	const gchar          *in_reason
){
	g_autofree gchar *reason = NULL;
	AxisMovieState state = derive(f, &reason);

	if (state != expected)
		g_error("expected %s, got %s (%s)", axis_movie_state_name(expected), axis_movie_state_name(state),
			reason != NULL ? reason : "");
	g_assert_nonnull(reason);
	if (in_reason != NULL && strstr(reason, in_reason) == NULL)
		g_error("expected '%s' in the reason: %s", in_reason, reason);
}

static void
test_names(void)
{
	guint i;

	for (i = 0; i < AXIS_MOVIE_N_STATES; i++) {
		AxisMovieState back;

		g_assert_true(axis_movie_state_from_name(axis_movie_state_name((AxisMovieState)i), &back));
		g_assert_cmpint(back, ==, i);
		g_assert_nonnull(axis_movie_state_label((AxisMovieState)i));
		g_assert_cmpuint(strlen(axis_movie_state_short((AxisMovieState)i)), <=, 17);
	}
	g_assert_false(axis_movie_state_from_name("missing", NULL));
	for (i = 0; i <= AXIS_MOVIE_RELEASED; i++) {
		AxisMovieAvailability back;

		g_assert_true(axis_movie_availability_from_name(axis_movie_availability_name((AxisMovieAvailability)i),
			&back));
		g_assert_cmpint(back, ==, i);
	}
}

static void
test_wanted(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	assert_state(f, AXIS_MOVIE_WANTED, "Not searched for yet");
	f->search_us = NOW_US - G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC;
	f->search_outcome = g_strdup("nothing_found");
	f->next_search_us = NOW_US + G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC;
	assert_state(f, AXIS_MOVIE_WANTED, "found no release");
	assert_state(f, AXIS_MOVIE_WANTED, "next automatic search");
	f->monitored = FALSE;
	assert_state(f, AXIS_MOVIE_WANTED, "Not monitored");
}

/* Spec: Before release */
static void
test_not_available(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();
	AxisMovieRules rules = rules_now();
	g_autofree gchar *from = NULL;

	/* Released: 90 days after the release date. */
	g_free(f->release_date);
	f->release_date = g_strdup("2026-10-01");
	g_assert_false(axis_movie_available(f, &rules, &from));
	g_assert_cmpstr(from, ==, "2026-12-30");
	assert_state(f, AXIS_MOVIE_NOT_AVAILABLE, "2026-12-30");
	/* In cinemas: from the date itself. */
	f->availability = AXIS_MOVIE_IN_CINEMAS;
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	g_free(f->release_date);
	f->release_date = g_strdup("2026-11-15");
	assert_state(f, AXIS_MOVIE_NOT_AVAILABLE, "2026-11-15");
	g_free(f->release_date);
	f->release_date = g_strdup("2026-11-14");
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	/* Announced: always. */
	g_free(f->release_date);
	f->release_date = g_strdup("2030-01-01");
	f->availability = AXIS_MOVIE_ANNOUNCED;
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	/* No date: only what the provider says is released. */
	g_clear_pointer(&f->release_date, g_free);
	f->availability = AXIS_MOVIE_RELEASED;
	f->status = AXIS_META_STATUS_UPCOMING;
	assert_state(f, AXIS_MOVIE_NOT_AVAILABLE, "No release date");
	f->status = AXIS_META_STATUS_RELEASED;
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	/* The setting moves the day. */
	g_free(f->release_date);
	f->release_date = g_strdup("2026-10-01");
	rules.released_after_days = 30;
	g_assert_true(axis_movie_available(f, &rules, NULL));
}

static void
test_searching(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->search_queued = TRUE;
	assert_state(f, AXIS_MOVIE_SEARCHING, "queued");
	f->search_queued = FALSE;
	f->search_running = TRUE;
	assert_state(f, AXIS_MOVIE_SEARCHING, "Searching");
}

static void
test_downloading(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	/* Claimed, not handed over yet: in flight already. */
	f->grab_id = 4;
	f->grab_release = g_strdup("The.Mazework.1999.1080p.BluRay.x264-GRP");
	g_assert_true(axis_movie_in_flight(f));
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "Being handed");
	/* A search queued meanwhile does not change it: downloading comes first. */
	f->search_queued = TRUE;
	assert_state(f, AXIS_MOVIE_DOWNLOADING, NULL);
	f->search_queued = FALSE;
	/* Sent, and the client's answer was lost: in flight. */
	f->download_id = 9;
	f->download_known = TRUE;
	f->download_state = AXIS_DOWNLOAD_SENDING;
	f->client_name = g_strdup("SABnzbd");
	g_assert_true(axis_movie_in_flight(f));
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "waiting for the client to confirm");
	f->download_state = AXIS_DOWNLOAD_DOWNLOADING;
	f->download_size = 10000000000;
	f->download_done = 4000000000;
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "40% of 9.3\302\240GiB at SABnzbd.");
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "GiB at SABnzbd.");
	f->download_stalled = TRUE;
	f->download_progress_us = NOW_US - G_GINT64_CONSTANT(7200) * G_USEC_PER_SEC;
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "Stalled at SABnzbd");
	/* Failed, not dealt with yet: still in flight, so nothing else is grabbed. */
	f->download_stalled = FALSE;
	f->download_state = AXIS_DOWNLOAD_FAILED;
	g_assert_true(axis_movie_in_flight(f));
	assert_state(f, AXIS_MOVIE_DOWNLOADING, "blocklist");
}

/* Spec: Completed */
static void
test_to_import(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->grab_id = 4;
	f->download_id = 9;
	f->download_known = TRUE;
	f->download_state = AXIS_DOWNLOAD_COMPLETED;
	f->download_volume = g_strdup("Disk one");
	f->download_rel_path = g_strdup("downloads/complete/The.Mazework");
	g_assert_false(axis_movie_in_flight(f));
	assert_state(f, AXIS_MOVIE_TO_IMPORT, "downloads/complete/The.Mazework");
	assert_state(f, AXIS_MOVIE_TO_IMPORT, "nothing has been moved");
	/* Recorded complete, and the client has since dropped it: still waiting. */
	f->grab_completed_us = NOW_US;
	f->download_state = AXIS_DOWNLOAD_GONE;
	assert_state(f, AXIS_MOVIE_TO_IMPORT, NULL);
	g_assert_false(axis_movie_in_flight(f));
}

static void
test_library(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->n_files = 1;
	f->file_quality = AXIS_QUALITY_WEBDL_1080P;
	assert_state(f, AXIS_MOVIE_UPGRADE_WANTED, "below the cutoff of its quality profile");
	f->monitored = FALSE;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, NULL);
	f->monitored = TRUE;
	f->file_quality = AXIS_QUALITY_BLURAY_1080P;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, NULL);
	f->file_quality = AXIS_QUALITY_UNKNOWN;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, NULL);
	f->file_quality = AXIS_QUALITY_WEBDL_1080P;
	f->profile_upgrades = FALSE;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, NULL);
}

/* add-force-grab-and-assign.  Spec (movies): A movie's file kept by hand is
 * not upgraded by itself; scenarios "Below the cutoff" (the state), "Let
 * go".  A file below the cutoff that a person keeps by hand is in the
 * library, not wanted for an upgrade, and the reason says who keeps it;
 * let go, the ordinary rule is back. */
static void
test_kept_by_hand(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->n_files = 1;
	f->file_quality = AXIS_QUALITY_WEBDL_1080P;
	assert_state(f, AXIS_MOVIE_UPGRADE_WANTED, "below the cutoff of its quality profile");
	f->kept_by = g_strdup("root");
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, "kept by hand by root: it is not upgraded by itself");
	g_clear_pointer(&f->kept_by, g_free);
	assert_state(f, AXIS_MOVIE_UPGRADE_WANTED, NULL);
}

/* Review finding 2 and 6: a grab by force let go (not_imported) whose import
 * is blocked leaves the movie needing attention, and the words say it is held
 * from automatic search and what to do. */
static void
test_held_forced(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->import_state = g_strdup("blocked");
	f->import_text = g_strdup("No video file in the download.");
	assert_state(f, AXIS_MOVIE_ATTENTION, "An import of it is blocked");
	f->held_forced = g_strdup("Paper.Kite.2019.1080p");
	assert_state(f, AXIS_MOVIE_ATTENTION, "You grabbed Paper.Kite.2019.1080p by force");
	assert_state(f, AXIS_MOVIE_ATTENTION, "does not search for this movie while the download is held for you");
	assert_state(f, AXIS_MOVIE_ATTENTION, "dismiss it and Axis searches again");
}

/* A stored release with these fixed rules ("code text", ...; the list ended
 * by NULL), and a rejection of the profile's besides. */
static AxisAcquireCandidate *
ruled_out(const gchar *first, ...)
{
	AxisAcquireCandidate *c = g_new0(AxisAcquireCandidate, 1);
	const gchar *code;
	va_list ap;

	c->name = g_strdup("Paper.Lantern.2019.1080p.WEB-DL.x264-GRP");
	c->reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_reason_free);
	g_ptr_array_add(c->reasons, axis_acquire_reason_new("profile", "too_large", "It is too large."));
	va_start(ap, first);
	for (code = first; code != NULL; code = va_arg(ap, const gchar *)) {
		g_autofree gchar *text = g_strdup_printf("The rule %s.", code);

		g_ptr_array_add(c->reasons, axis_acquire_reason_new("safety", code, text));
		c->safety = TRUE;
	}
	va_end(ap);
	return c;
}

/* add-force-grab-and-assign.  Spec (quality-profiles): Some releases are
 * never accepted; scenarios "A rule about the name" and "A rule that
 * protects the system".  Only the fixed rules about a release's name may be
 * set aside by force; one rule that protects the system among them keeps
 * the release out, and its words are given; a release with no fixed rule is
 * Grab this one's, not force's. */
static void
test_force_allowed(void)
{
	static const gchar *const about_the_name[] = { "ambiguous", "no_match", "no_title" };
	static const gchar *const protects[] = { "sample", "password", "archive", "executable", "post_subject",
		"a_rule_added_later" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(about_the_name); i++) {
		AxisAcquireCandidate *c = ruled_out(about_the_name[i], NULL);
		g_autofree gchar *kept = NULL;

		if (!axis_acquire_force_allowed(c, &kept))
			g_error("%s is a rule about the name: it may be forced", about_the_name[i]);
		g_assert_null(kept);
		axis_acquire_candidate_free(c);
	}
	for (i = 0; i < G_N_ELEMENTS(protects); i++) {
		AxisAcquireCandidate *alone = ruled_out(protects[i], NULL);
		AxisAcquireCandidate *both = ruled_out("ambiguous", protects[i], NULL);
		g_autofree gchar *kept = NULL;
		g_autofree gchar *expected = g_strdup_printf("The rule %s.", protects[i]);

		if (axis_acquire_force_allowed(alone, NULL))
			g_error("%s protects the system: no one forces it", protects[i]);
		/* With a rule about the name beside it: still not, and only the rule
		 * that stands is named. */
		g_assert_false(axis_acquire_force_allowed(both, &kept));
		g_assert_cmpstr(kept, ==, expected);
		axis_acquire_candidate_free(alone);
		axis_acquire_candidate_free(both);
	}
	{
		/* Two rules about the name: allowed.  No fixed rule: not force's. */
		AxisAcquireCandidate *two = ruled_out("ambiguous", "no_title", NULL);
		AxisAcquireCandidate *none = ruled_out(NULL);

		g_assert_true(axis_acquire_force_allowed(two, NULL));
		g_assert_false(axis_acquire_force_allowed(none, NULL));
		/* Marked ruled out with no rule kept: nothing says which. */
		none->safety = TRUE;
		g_assert_false(axis_acquire_force_allowed(none, NULL));
		g_assert_false(axis_acquire_force_allowed(NULL, NULL));
		axis_acquire_candidate_free(two);
		axis_acquire_candidate_free(none);
	}
}

/* Spec: The whole library is listed.  A movie on disk that Axis does not
 * manage is in the library, whatever else its facts hold, and says what is
 * there and when its disk is away. */
static void
test_on_disk(void)
{
	g_autoptr(AxisMovieFacts) f = g_new0(AxisMovieFacts, 1);
	g_autofree gchar *reason = NULL;

	f->library_only = TRUE;
	f->title_id = 801;
	f->title = g_strdup("Lantern Harbour");
	f->year = 2011;
	f->n_files = 1;
	f->n_copies = 1;
	f->file_quality = AXIS_QUALITY_WEBDL_720P;
	/* Never upgrade wanted, never wanted, never searched: not managed. */
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, "On disk as WEB-DL 720p, not managed by Axis.");
	f->file_quality = AXIS_QUALITY_UNKNOWN;
	f->file_resolution = AXIS_RELEASE_RESOLUTION_1080P;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, "On disk as ");
	g_assert_cmpint(derive(f, &reason), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_null(strstr(reason, "offline"));
	g_clear_pointer(&reason, g_free);
	/* Its disk away: still in the library, and said. */
	f->n_away = 1;
	f->away_volume = g_strdup("Disk two");
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, "Its disk, Disk two, is offline: it stays listed");
	f->n_files = 2;
	f->n_copies = 2;
	assert_state(f, AXIS_MOVIE_IN_LIBRARY, "One of its disks, Disk two, is offline.");
	g_assert_false(axis_movie_in_flight(f));
}

/* Spec: Nothing acceptable; and the other reasons a film needs attention. */
static void
test_attention(void)
{
	g_autoptr(AxisMovieFacts) f = facts_new();

	f->search_us = NOW_US;
	f->search_outcome = g_strdup("none_acceptable");
	f->search_text = g_strdup("4 releases were found and the profile Balanced rejected them all: 4 not this film.");
	assert_state(f, AXIS_MOVIE_ATTENTION, "rejected them all");
	g_free(f->search_outcome);
	f->search_outcome = g_strdup("nothing_found");
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	/* The destination, whatever the last search said. */
	f->volume_state = AXIS_VOLUME_OFFLINE;
	assert_state(f, AXIS_MOVIE_ATTENTION, "offline");
	f->volume_state = AXIS_VOLUME_READ_ONLY;
	assert_state(f, AXIS_MOVIE_WANTED, NULL);
	f->root_id = 0;
	assert_state(f, AXIS_MOVIE_ATTENTION, "destination library folder was removed");
	f->root_id = 3;
	f->profile_usable = FALSE;
	assert_state(f, AXIS_MOVIE_ATTENTION, "cannot be used");
	f->profile_usable = TRUE;
	/* A download Axis gave up on, still at its client: nothing else. */
	f->other_live = 1;
	g_assert_true(axis_movie_in_flight(f));
	assert_state(f, AXIS_MOVIE_ATTENTION, "still in its download client");
	f->other_live = 0;
	g_assert_false(axis_movie_in_flight(f));
	/* Every outcome that means a person has to look. */
	g_assert_true(axis_movie_outcome_needs_attention("exhausted"));
	g_assert_true(axis_movie_outcome_needs_attention("no_client"));
	g_assert_true(axis_movie_outcome_needs_attention("no_indexer"));
	g_assert_true(axis_movie_outcome_needs_attention("root_offline"));
	g_assert_false(axis_movie_outcome_needs_attention("grabbed"));
	g_assert_false(axis_movie_outcome_needs_attention("nothing_found"));
	g_assert_false(axis_movie_outcome_needs_attention(NULL));
}

static void
test_name_key(void)
{
	g_autofree gchar *a = axis_movie_name_key(1, 3, "NewsOwl", "The.Mazework.1999.1080p.BluRay.x264-GRP");
	g_autofree gchar *b = axis_movie_name_key(1, 3, NULL, "the mazework 1999 1080p bluray x264 grp");
	g_autofree gchar *c = axis_movie_name_key(1, 4, NULL, "The.Mazework.1999.1080p.BluRay.x264-GRP");
	g_autofree gchar *d = axis_movie_name_key(2, 0, "Tracker One", "The.Mazework.1999.1080p.BluRay.x264-GRP");
	g_autofree gchar *e = axis_movie_name_key(2, 0, NULL, "");
	g_autoptr(GString) longer = g_string_new(NULL);
	g_autofree gchar *f = NULL;

	g_assert_cmpstr(a, ==, "1:3:the.mazework.1999.1080p.bluray.x264.grp");
	/* The same release written another way is the same key... */
	g_assert_cmpstr(a, ==, b);
	/* ...but another indexer's is another. */
	g_assert_cmpstr(a, !=, c);
	g_assert_cmpstr(d, ==, "2:trackerone:the.mazework.1999.1080p.bluray.x264.grp");
	g_assert_cmpstr(e, ==, "2:-:-");
	while (longer->len < 2000)
		g_string_append(longer, "Ab.");
	f = axis_movie_name_key(1, 3, NULL, longer->str);
	g_assert_cmpuint(strlen(f), <=, 400);
	g_assert_true(g_utf8_validate(f, -1, NULL));
}

/* Whether a torrent's client may be asked to delete the torrent at @place,
 * mapped to @rel (%NULL: not mapped) on a disk with a films root "films", a
 * series root "tv/series" and a downloads root "downloads"; *@why the
 * refusal. */
static gboolean
place_ok(
	const gchar  *place,
	const gchar  *rel,
	gchar       **why
){
	g_autoptr(GPtrArray) roots = g_ptr_array_new();

	g_ptr_array_add(roots, (gpointer)"films");
	g_ptr_array_add(roots, (gpointer)"");
	g_ptr_array_add(roots, (gpointer)"tv/series");
	g_ptr_array_add(roots, (gpointer)"");
	g_ptr_array_add(roots, (gpointer)"downloads");
	g_ptr_array_add(roots, (gpointer)"d");
	g_free(*why);
	*why = NULL;
	return axis_acquire_place_judge(place, rel != NULL, rel, roots, why);
}

/* add-replace-grab-and-delete-download, slice A: the rule for asking a
 * torrent's client to delete a torrent wherever it is now (a replace). */
static void
test_replace_place(void)
{
	g_autofree gchar *why = NULL;

	/* Its own place: in the client's download folder, a downloads root or
	 * not. */
	g_assert_true(place_ok("/data/incomplete/Lantern.Harbor.2011.720p.HDTV.x264-MADEUP",
		"incomplete/Lantern.Harbor.2011.720p.HDTV.x264-MADEUP", &why));
	g_assert_null(why);
	g_assert_true(place_ok("/data/downloads/axis/Lantern.Harbor.2011.720p.HDTV.x264-MADEUP",
		"downloads/axis/Lantern.Harbor.2011.720p.HDTV.x264-MADEUP", &why));
	/* Several slashes and one at the end are the one path they mean. */
	g_assert_true(place_ok("/data//incomplete/Lantern.Harbor/", "incomplete/Lantern.Harbor", &why));
	/* A name that only begins as a root's does is not inside it. */
	g_assert_true(place_ok("/data/films2/Lantern.Harbor", "films2/Lantern.Harbor", &why));

	/* Inside a library folder, at one, or holding one. */
	g_assert_false(place_ok("/data/films/Lantern Harbor (2011)", "films/Lantern Harbor (2011)", &why));
	g_assert_nonnull(strstr(why, "inside a library folder"));
	g_assert_false(place_ok("/data/tv/series/Marsh Lantern/Season 01", "tv/series/Marsh Lantern/Season 01", &why));
	g_assert_nonnull(strstr(why, "inside a library folder"));
	g_assert_false(place_ok("/data/films", "films", &why));
	g_assert_nonnull(strstr(why, "a library folder itself"));
	g_assert_false(place_ok("/data/tv", "tv", &why));
	g_assert_nonnull(strstr(why, "holds a library folder"));
	/* The downloads root itself is the client's whole folder, never one
	 * download's own place. */
	g_assert_false(place_ok("/data/downloads", "downloads", &why));
	g_assert_nonnull(strstr(why, "a library folder itself"));
	/* The top of a disk. */
	g_assert_false(place_ok("/data", "", &why));
	g_assert_nonnull(strstr(why, "the top of a disk"));

	/* No place; no mapping; not a plain full path (never resolved). */
	g_assert_false(place_ok(NULL, NULL, &why));
	g_assert_nonnull(strstr(why, "does not say where"));
	g_assert_false(place_ok("", NULL, &why));
	g_assert_false(place_ok("/media/films/Lantern Harbor (2011)", NULL, &why));
	g_assert_nonnull(strstr(why, "none of the client's path mappings reaches"));
	g_assert_false(place_ok("/data/incomplete/../films/Lantern.Harbor", "incomplete/../films/Lantern.Harbor", &why));
	g_assert_nonnull(strstr(why, "not a plain full path"));
	g_assert_false(place_ok("/data/./films", "films", &why));
	g_assert_false(place_ok("incomplete/Lantern.Harbor", "incomplete/Lantern.Harbor", &why));
	g_assert_nonnull(strstr(why, "not a plain full path"));
	g_assert_false(place_ok(" /data/incomplete/Lantern.Harbor", "incomplete/Lantern.Harbor", &why));
}

/* As place_ok(), for the positive half: the place is known to be one a client
 * must not delete at. */
static gboolean
place_in_library(
	const gchar  *place,
	const gchar  *rel,
	gchar       **why
){
	g_autoptr(GPtrArray) roots = g_ptr_array_new();

	g_ptr_array_add(roots, (gpointer)"films");
	g_ptr_array_add(roots, (gpointer)"");
	g_ptr_array_add(roots, (gpointer)"tv/series");
	g_ptr_array_add(roots, (gpointer)"");
	g_ptr_array_add(roots, (gpointer)"downloads");
	g_ptr_array_add(roots, (gpointer)"d");
	g_free(*why);
	*why = NULL;
	return axis_acquire_place_in_library(place, rel != NULL, rel, roots, why);
}

/* The positive half of the place rule, which Give up, Block and search again
 * and an ordinary failure use: only a place known to be in, at or holding a
 * library folder, or the top of a disk.  A place that cannot be judged is not
 * one: those removals stay with their data. */
static void
test_place_in_library(void)
{
	g_autofree gchar *why = NULL;

	g_assert_true(place_in_library("/data/films/Lantern Harbor (2011)", "films/Lantern Harbor (2011)", &why));
	g_assert_nonnull(strstr(why, "inside a library folder"));
	g_assert_true(place_in_library("/data/films", "films", &why));
	g_assert_true(place_in_library("/data/tv", "tv", &why));
	g_assert_nonnull(strstr(why, "holds a library folder"));
	g_assert_true(place_in_library("/data", "", &why));
	g_assert_nonnull(strstr(why, "the top of a disk"));
	/* A downloads root itself (a client that puts a multi-file torrent's files
	 * directly in its save path) is not a library folder. */
	g_assert_false(place_in_library("/data/downloads", "downloads", &why));

	/* Its own place. */
	g_assert_false(place_in_library("/data/incomplete/Lantern.Harbor", "incomplete/Lantern.Harbor", &why));
	g_assert_null(why);
	g_assert_false(place_in_library("/data/downloads/axis/Lantern.Harbor", "downloads/axis/Lantern.Harbor", &why));
	/* Not known: no place, no mapping, not a plain full path. */
	g_assert_false(place_in_library(NULL, NULL, &why));
	g_assert_false(place_in_library("", NULL, &why));
	g_assert_false(place_in_library("/media/films/Lantern Harbor (2011)", NULL, &why));
	g_assert_false(place_in_library("/data/incomplete/../films/Lantern.Harbor", "incomplete/../films/Lantern.Harbor",
		&why));
	g_assert_null(why);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/acquire/unit/replace-place", test_replace_place);
	g_test_add_func("/acquire/unit/place-in-library", test_place_in_library);
	g_test_add_func("/movies/unit/names", test_names);
	g_test_add_func("/movies/unit/wanted", test_wanted);
	g_test_add_func("/movies/unit/not-available", test_not_available);
	g_test_add_func("/movies/unit/searching", test_searching);
	g_test_add_func("/movies/unit/downloading", test_downloading);
	g_test_add_func("/movies/unit/to-import", test_to_import);
	g_test_add_func("/movies/unit/library", test_library);
	g_test_add_func("/movies/unit/kept-by-hand", test_kept_by_hand);
	g_test_add_func("/movies/unit/held-forced", test_held_forced);
	g_test_add_func("/acquire/unit/force-allowed", test_force_allowed);
	g_test_add_func("/movies/unit/on-disk", test_on_disk);
	g_test_add_func("/movies/unit/attention", test_attention);
	g_test_add_func("/movies/unit/name-key", test_name_key);
	return g_test_run();
}
