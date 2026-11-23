/*
 * test-download-qbittorrent.c - the qBittorrent download client kind, against
 * a stand-in for qBittorrent's Web API v2 on loopback (4.x and 5.x), a real
 * database and a real volume in a scratch directory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE FIXTURES AND THE STAND-IN ARE HAND-WRITTEN.  No qBittorrent was
 * available: the answers in tests/fixtures/qbittorrent and the stand-in in
 * tests/axis-test-qbittorrent.c follow qBittorrent's published Web API and
 * the author's memory of 4.6 and 5.0, not a recording.  What is believed but
 * not verified is listed in tests/fixtures/qbittorrent/README.md and in the
 * report of the change.  These tests show that the kind does what it means to
 * against that format; they do not show that the format is right.
 *
 * Spec scenarios (downloaders):
 *   qBittorrent client: Add; Already there; Added but not shown; The category
 *     is made; Seeding; Not Axis's
 *   qBittorrent states are mapped, never guessed: Unknown word; Renamed words
 *   A refused qBittorrent login is not retried in a loop: Wrong password; Banned
 *   A lost qBittorrent session never repeats an add: Session lost during a
 *     list; Session lost during an add
 *   A download client may have a user name: Saved and shown
 * and tasks 2 and 3 of add-downloader-qbittorrent.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-plex.h"
#include "axis-test-qbittorrent.h"
#include "axis-test-storage.h"
#include "download/axis-download.h"
#include "download/axis-download-qbittorrent.h"
#include "download/axis-download-qbittorrent-parse.h"
#include "download/axis-download-private.h"

#define USER "axisuser"
#define PASSWORD "qbit pw+7d1c&9e0a=5f%"
#define PASSWORD_ENCODED "qbit%20pw%2B7d1c%269e0a%3D5f%25"
#define WRONG_PASSWORD "wrong-web-pw-0a9d33f1b8e2"
#define OTHER_PASSWORD "other-web-pw-3c71e9a2d5f0"
#define FIXTURES "tests/fixtures/qbittorrent/"
#define MAGNET_HASH "89abcdef0123456789abcdef0123456789abcdef"
#define MAGNET "magnet:?xt=urn:btih:" MAGNET_HASH "&dn=Some.Magnet.Release"

/* ── Fixture files ──────────────────────────────────────────────── */

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	return text;
}

/* ── Reading answers (no database) ──────────────────────────────── */

/* The slots of a fixture's list. */
static GPtrArray *
slots_of(
	const gchar *fixture,
	const gchar *category
){
	g_autofree gchar *text = fixture_text(fixture);

	return axis_qbit_parse_torrents(text, strlen(text), category, NULL);
}

static const AxisQbitSlot *
slot_by_ref(
	GPtrArray   *slots,
	const gchar *ref
){
	guint i;

	for (i = 0; i < slots->len; i++) {
		const AxisQbitSlot *slot = g_ptr_array_index(slots, i);

		if (strcmp(slot->item->client_ref, ref) == 0)
			return slot;
	}
	g_error("no slot %s", ref);
	return NULL;
}

static gboolean
has_slot(
	GPtrArray   *slots,
	const gchar *ref
){
	guint i;

	for (i = 0; i < slots->len; i++) {
		if (strcmp(((AxisQbitSlot *)g_ptr_array_index(slots, i))->item->client_ref, ref) == 0)
			return TRUE;
	}
	return FALSE;
}

static void
assert_item(
	const AxisQbitSlot    *slot,
	AxisDownloadItemState  state,
	gint64                 size,
	gint64                 done,
	gint64                 rate,
	gint64                 eta
){
	g_assert_cmpint(slot->item->state, ==, state);
	g_assert_cmpint(slot->item->size_bytes, ==, size);
	g_assert_cmpint(slot->item->done_bytes, ==, done);
	g_assert_cmpint(slot->item->rate_bps, ==, rate);
	g_assert_cmpint(slot->item->eta_s, ==, eta);
}

/* Task 2.1: the plain text answers */
static void
test_parse_text(void)
{
	g_assert_cmpint(axis_qbit_parse_text("", 0), ==, AXIS_QBIT_TEXT_OK);
	g_assert_cmpint(axis_qbit_parse_text(NULL, 0), ==, AXIS_QBIT_TEXT_OK);
	g_assert_cmpint(axis_qbit_parse_text("Ok.", 3), ==, AXIS_QBIT_TEXT_OK);
	g_assert_cmpint(axis_qbit_parse_text("Ok.\n", 4), ==, AXIS_QBIT_TEXT_OK);
	g_assert_cmpint(axis_qbit_parse_text(" Fails. ", 8), ==, AXIS_QBIT_TEXT_FAILS);
	g_assert_cmpint(axis_qbit_parse_text("Fails.", 6), ==, AXIS_QBIT_TEXT_FAILS);
	/* Case matters; anything else is not an answer. */
	g_assert_cmpint(axis_qbit_parse_text("OK.", 3), ==, AXIS_QBIT_TEXT_OTHER);
	g_assert_cmpint(axis_qbit_parse_text("Ok", 2), ==, AXIS_QBIT_TEXT_OTHER);
	g_assert_cmpint(axis_qbit_parse_text("fails.", 6), ==, AXIS_QBIT_TEXT_OTHER);
	g_assert_cmpint(axis_qbit_parse_text("<html>Welcome to nginx</html>", 29), ==, AXIS_QBIT_TEXT_OTHER);
	g_assert_cmpint(axis_qbit_parse_text("Ok.\0x", 5), ==, AXIS_QBIT_TEXT_OTHER);
}

/* Task 2.1: the versions */
static void
test_parse_version(void)
{
	g_autofree gchar *a = axis_qbit_parse_version("v4.6.2", 6);
	g_autofree gchar *b = axis_qbit_parse_version("2.9.3\n", 6);
	g_autofree gchar *c = axis_qbit_parse_version("v5.0.0beta1+git-1", 17);

	g_assert_cmpstr(a, ==, "v4.6.2");
	g_assert_cmpstr(b, ==, "2.9.3");
	g_assert_cmpstr(c, ==, "v5.0.0beta1+git-1");
	g_assert_null(axis_qbit_parse_version("", 0));
	g_assert_null(axis_qbit_parse_version(NULL, 0));
	g_assert_null(axis_qbit_parse_version("4.6.2 beta", 10));
	g_assert_null(axis_qbit_parse_version("<html>", 6));
	g_assert_null(axis_qbit_parse_version("{\"version\": 1}", 14));
	g_assert_null(axis_qbit_parse_version("v4.6.2\0", 7));
	g_assert_null(axis_qbit_parse_version("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 44));
}

/* Task 2.1: the list of categories */
static void
test_parse_categories(void)
{
	g_autofree gchar *with = fixture_text("categories.json");
	g_autofree gchar *without = fixture_text("categories-no-axis.json");
	g_autofree gchar *list = fixture_text("categories-not-an-object.json");

	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), "axis"), ==, 1);
	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), "manual"), ==, 1);
	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), "movies/hd"), ==, 1);
	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), "Axis"), ==, 0);
	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), "nope"), ==, 0);
	g_assert_cmpint(axis_qbit_parse_has_category(with, strlen(with), NULL), ==, 0);
	/* Case matters: qBittorrent's categories do. */
	g_assert_cmpint(axis_qbit_parse_has_category(without, strlen(without), "axis"), ==, 0);
	g_assert_cmpint(axis_qbit_parse_has_category(without, strlen(without), "Axis"), ==, 1);
	g_assert_cmpint(axis_qbit_parse_has_category("{}", 2, "axis"), ==, 0);
	/* Not an object: not an answer. */
	g_assert_cmpint(axis_qbit_parse_has_category(list, strlen(list), "axis"), ==, -1);
	g_assert_cmpint(axis_qbit_parse_has_category("", 0, "axis"), ==, -1);
	g_assert_cmpint(axis_qbit_parse_has_category("null", 4, "axis"), ==, -1);
	g_assert_cmpint(axis_qbit_parse_has_category("{\"axis\": ", 9, "axis"), ==, -1);
	g_assert_cmpint(axis_qbit_parse_has_category("<html>", 6, "axis"), ==, -1);
}

/* Spec: qBittorrent states are mapped, never guessed (the table of the spec) */
static void
test_state_map(void)
{
	static const struct
	{
		const gchar          *word;
		AxisDownloadItemState state;
		gboolean              finished;
		gboolean              moving;
	} known[] = {
		{ "downloading", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "forcedDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "stalledDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "metaDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "forcedMetaDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "queuedDL", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "allocating", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "checkingDL", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "checkingResumeData", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "pausedDL", AXIS_DOWNLOAD_ITEM_PAUSED, FALSE, FALSE },
		{ "stoppedDL", AXIS_DOWNLOAD_ITEM_PAUSED, FALSE, FALSE },
		{ "uploading", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "forcedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "stalledUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "queuedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "checkingUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "pausedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "stoppedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "moving", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, TRUE },
		{ "error", AXIS_DOWNLOAD_ITEM_FAILED, FALSE, FALSE },
		{ "missingFiles", AXIS_DOWNLOAD_ITEM_FAILED, FALSE, FALSE }
	};
	static const gchar *const unknown[] = {
		"unknown", "UPLOADING", "Downloading", "stopped", "paused", "pausedup", "reticulatingSplines", "", " uploading", NULL
	};
	AxisQbitMapped mapped;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known); i++) {
		g_assert_true(axis_qbit_state_map(known[i].word, &mapped));
		g_assert_cmpint(mapped.state, ==, known[i].state);
		g_assert_cmpint(mapped.finished, ==, known[i].finished);
		g_assert_cmpint(mapped.moving, ==, known[i].moving);
	}
	/* Every other word is not known, and says nothing: queued, not finished. */
	for (i = 0; i < G_N_ELEMENTS(unknown); i++) {
		g_assert_false(axis_qbit_state_map(unknown[i], &mapped));
		g_assert_cmpint(mapped.state, ==, AXIS_DOWNLOAD_ITEM_QUEUED);
		g_assert_false(mapped.finished);
		g_assert_false(mapped.moving);
	}
	g_assert_false(axis_qbit_state_map(NULL, &mapped));
}

/* Task 2.1: the list, state by state */
static void
test_parse_torrents(void)
{
	g_autoptr(GPtrArray) slots = slots_of("torrents.json", "axis");
	g_autoptr(GPtrArray) all = slots_of("torrents.json", NULL);
	const AxisQbitSlot *slot;

	g_assert_nonnull(slots);
	/* Another category, none and a sub-category are not Axis's: 26 of the 29. */
	g_assert_cmpuint(slots->len, ==, 26);
	g_assert_false(has_slot(slots, "1717171717171717171717171717171717171717"));
	g_assert_false(has_slot(slots, "1818181818181818181818181818181818181818"));
	g_assert_false(has_slot(slots, "1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c"));
	g_assert_cmpuint(all->len, ==, 29);
	g_assert_cmpstr(slot_by_ref(all, "1717171717171717171717171717171717171717")->item->category, ==, "manual");
	g_assert_cmpstr(slot_by_ref(all, "1818181818181818181818181818181818181818")->item->category, ==, "");
	g_assert_cmpstr(slot_by_ref(all, "1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c")->item->category, ==, "axis/sub");

	slot = slot_by_ref(slots, "0101010101010101010101010101010101010101");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, 4000000000, 2000000000, 5242880, 280);
	g_assert_cmpstr(slot->item->name, ==, "Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(slot->item->category, ==, "axis");
	g_assert_cmpstr(slot->item->info_hash, ==, "0101010101010101010101010101010101010101");
	g_assert_cmpstr(slot->word, ==, "downloading");
	g_assert_true(slot->known);
	g_assert_null(slot->item->path);
	/* Where its files are now, though it has not completed (for what
	 * accounts for the client's folder, never for an import). */
	g_assert_cmpstr(slot->item->place, ==, "/downloads/Some.Film.2020.1080p.WEB-DL");
	g_assert_null(slot->item->message);

	/* Queued, checking, allocating: queued; no rate or time for what is not running. */
	assert_item(slot_by_ref(slots, "0202020202020202020202020202020202020202"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 0, -1, -1);
	assert_item(slot_by_ref(slots, "0303030303030303030303030303030303030303"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 187500000, -1, -1);
	assert_item(slot_by_ref(slots, "0404040404040404040404040404040404040404"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 0, -1, -1);
	assert_item(slot_by_ref(slots, "1515151515151515151515151515151515151515"), AXIS_DOWNLOAD_ITEM_QUEUED, 1000, 500, -1, -1);
	/* Paused (4.x) and stopped (5.x). */
	assert_item(slot_by_ref(slots, "0505050505050505050505050505050505050505"), AXIS_DOWNLOAD_ITEM_PAUSED, 700000000, 350000000, -1, -1);
	assert_item(slot_by_ref(slots, "0606060606060606060606060606060606060606"), AXIS_DOWNLOAD_ITEM_PAUSED, 700000000, 175000000, -1, -1);

	/* Moving is downloading, all of it: never a path. */
	slot = slot_by_ref(slots, "0707070707070707070707070707070707070707");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, 2000000000, 2000000000, -1, -1);
	g_assert_null(slot->item->path);
	g_assert_null(slot->item->place);

	/* Every way of being finished is completed, at its content path (a slash at the end is none). */
	slot = slot_by_ref(slots, "0808080808080808080808080808080808080808");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_COMPLETED, 3000000000, 3000000000, -1, -1);
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Seeding.Film.2018.1080p");
	g_assert_cmpstr(slot_by_ref(slots, "0909090909090909090909090909090909090909")->item->path, ==, "/downloads/Stalled.Seed.2017");
	g_assert_cmpstr(slot_by_ref(slots, "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a")->item->path, ==, "/downloads/Done.And.Paused.2017");
	g_assert_cmpstr(slot_by_ref(slots, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")->item->path, ==, "/downloads/Done.And.Stopped.2016");
	g_assert_cmpint(slot_by_ref(slots, "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a")->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_cmpint(slot_by_ref(slots, "1212121212121212121212121212121212121212")->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_cmpint(slot_by_ref(slots, "1313131313131313131313131313131313131313")->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_cmpint(slot_by_ref(slots, "1414141414141414141414141414141414141414")->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);

	/* Error and missing files: failed, with a reason. */
	slot = slot_by_ref(slots, "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c");
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_cmpstr(slot->item->message, ==, "qBittorrent reports this torrent as in error");
	g_assert_null(slot->item->path);
	slot = slot_by_ref(slots, "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_nonnull(strstr(slot->item->message, "missing"));

	/* The other ways of downloading: forced, metadata, stalled. */
	assert_item(slot_by_ref(slots, "0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 1000, 500, 100, 5);
	assert_item(slot_by_ref(slots, "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 0, 0, 0, -1);
	assert_item(slot_by_ref(slots, "1010101010101010101010101010101010101010"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 1000, 500, 0, -1);
	assert_item(slot_by_ref(slots, "1111111111111111111111111111111111111111"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 0, 0, 0, -1);

	/* A word qBittorrent documents as "unknown" is not known to Axis either. */
	slot = slot_by_ref(slots, "1616161616161616161616161616161616161616");
	g_assert_false(slot->known);
	g_assert_cmpstr(slot->word, ==, "unknown");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_QUEUED);
	g_assert_null(slot->item->path);

	/* The hash is lower-cased: it is the identifier and the hash. */
	slot = slot_by_ref(slots, "abababababababababababababababababababab");
	g_assert_cmpstr(slot->item->info_hash, ==, "abababababababababababababababababababab");

	/* No content path (qBittorrent before 4.3.2), or one that is not a place: the save path and the name. */
	g_assert_cmpstr(slot_by_ref(slots, "1919191919191919191919191919191919191919")->item->path, ==, "/downloads/Old.Client.Film");
	g_assert_cmpstr(slot_by_ref(slots, "1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a")->item->path, ==, "/downloads/Relative.Content.Path");

	/* The path keeps a name's own spaces; the name shown is cleaned. */
	slot = slot_by_ref(slots, "1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b");
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Double  Space  Name");
	g_assert_cmpstr(slot->item->name, ==, "Double Space Name");
}

/* Task 2.1: wrong types, out of range, missing, not objects */
static void
test_parse_broken_answers(void)
{
	g_autoptr(GPtrArray) slots = slots_of("torrents-wrong-types.json", "axis");
	g_autoptr(GPtrArray) any = slots_of("torrents-wrong-types.json", NULL);
	g_autoptr(GPtrArray) empty = slots_of("torrents-empty.json", "axis");
	g_autoptr(GPtrArray) unknown = slots_of("torrents-unknown-state.json", "axis");
	g_autofree gchar *not_a_list = fixture_text("torrents-not-a-list.json");
	g_autofree gchar *problem = NULL;
	const AxisQbitSlot *slot;

	/* Entries with a bad hash or that are not objects are skipped, a hash is
	 * taken once, and one with no category is not Axis's: seven are left, and
	 * the two whose hash is read but not their name, which say so (review of
	 * the fixes, item 7: the layer must know which torrent it is). */
	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, 9);
	g_assert_cmpstr(slot_by_ref(slots, "2727272727272727272727272727272727272727")->item->unreadable, ==,
		"it has no name Axis can read");
	g_assert_null(slot_by_ref(slots, "2727272727272727272727272727272727272727")->item->name);
	g_assert_nonnull(slot_by_ref(slots, "2828282828282828282828282828282828282828")->item->unreadable);
	g_assert_null(slot_by_ref(slots, "2020202020202020202020202020202020202020")->item->unreadable);
	/* Without a category asked for, the two without one are read, with none. */
	g_assert_cmpuint(any->len, ==, 11);
	g_assert_cmpstr(slot_by_ref(any, "3232323232323232323232323232323232323232")->item->category, ==, "");
	/* Numbers in strings are numbers. */
	assert_item(slot_by_ref(slots, "2020202020202020202020202020202020202020"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 2000, 1000, 300, 60);
	/* Garbage sizes and times: not known, not wrong. */
	assert_item(slot_by_ref(slots, "2121212121212121212121212121212121212121"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, -1, -1);
	/* Out of range: nothing. */
	assert_item(slot_by_ref(slots, "2222222222222222222222222222222222222222"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, -1, -1);
	/* No progress: no amount done; no time left is none. */
	assert_item(slot_by_ref(slots, "2323232323232323232323232323232323232323"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 1000, -1, 0, -1);
	/* The first of one hash. */
	g_assert_cmpstr(slot_by_ref(slots, "2424242424242424242424242424242424242424")->item->name, ==, "Same.Hash.Once");
	/* A state that is not a word is not a known one. */
	slot = slot_by_ref(slots, "2525252525252525252525252525252525252525");
	g_assert_false(slot->known);
	g_assert_null(slot->word);
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_QUEUED);
	/* An error with nothing else still says something. */
	slot = slot_by_ref(slots, "2626262626262626262626262626262626262626");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_nonnull(slot->item->message);

	/* Words nobody knows, unfinished and (in another case) finished: unknown, with their words, and no path. */
	g_assert_nonnull(unknown);
	g_assert_cmpuint(unknown->len, ==, 2);
	slot = slot_by_ref(unknown, "4040404040404040404040404040404040404040");
	g_assert_false(slot->known);
	g_assert_cmpstr(slot->word, ==, "reticulatingSplines");
	slot = slot_by_ref(unknown, "4141414141414141414141414141414141414141");
	g_assert_false(slot->known);
	g_assert_cmpstr(slot->word, ==, "UPLOADING");
	g_assert_null(slot->item->path);

	/* Empty is a list; anything but a list is none, with a reason. */
	g_assert_nonnull(empty);
	g_assert_cmpuint(empty->len, ==, 0);
	g_assert_null(axis_qbit_parse_torrents(not_a_list, strlen(not_a_list), "axis", &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_qbit_parse_torrents("{}", 2, "axis", NULL));
	g_assert_null(axis_qbit_parse_torrents("null", 4, "axis", NULL));
	g_assert_null(axis_qbit_parse_torrents("\"text\"", 6, "axis", NULL));
	g_assert_null(axis_qbit_parse_torrents("", 0, "axis", &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_qbit_parse_torrents(NULL, 0, "axis", NULL));
	g_assert_null(axis_qbit_parse_torrents("[{\"hash\": \"0101", 15, "axis", &problem));
	g_assert_nonnull(problem);
	g_assert_null(axis_qbit_parse_torrents("<html>Bad gateway</html>", 24, "axis", NULL));
}

/* Task 2.1: a list is cut */
static void
test_parse_capped(void)
{
	GString *text = g_string_new("[");
	g_autoptr(GPtrArray) slots = NULL;
	guint i;

	for (i = 1; i <= AXIS_QBIT_MAX_TORRENTS + 100; i++)
		g_string_append_printf(text, "%s{\"hash\": \"%040x\", \"name\": \"T%u\", \"category\": \"axis\", \"state\": \"queuedDL\"}",
			i > 1 ? "," : "", i, i);
	g_string_append_c(text, ']');
	slots = axis_qbit_parse_torrents(text->str, text->len, "axis", NULL);
	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, AXIS_QBIT_MAX_TORRENTS);
	g_clear_pointer(&slots, g_ptr_array_unref);
	/* And it says so (third review, item 5). */
	{
		gboolean cut = FALSE;
		guint unreadable = 99;

		slots = axis_qbit_parse_torrents_counted(text->str, text->len, "axis", &unreadable, &cut, NULL);
		g_assert_nonnull(slots);
		g_assert_true(cut);
		g_assert_cmpuint(unreadable, ==, 0);
	}
	g_string_free(text, TRUE);
	g_assert_true(axis_qbit_hash_text_valid("0123456789abcdef0123456789ABCDEF01234567"));
	g_assert_false(axis_qbit_hash_text_valid("0123456789abcdefABCDEF0123456789abcdefA"));
	g_assert_false(axis_qbit_hash_text_valid("0123456789abcdefABCDEF0123456789abcdefAZ"));
	g_assert_false(axis_qbit_hash_text_valid(NULL));
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestLibrary        *t;
	AxisTestQbit           *qb;
	AxisDownloads          *downloads;
	gchar                  *disk;
	gint64                  volume_id;
	gint64                  client_id;
	AxisTestQbitGeneration  generation;
} Fixture;

static AxisDownloads *
layer_new(Fixture *fx)
{
	AxisDownloads *downloads = axis_downloads_new(fx->t->pool, fx->t->storage, NULL);
	AxisDownloadTuning tuning;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_kind(downloads, axis_download_qbittorrent_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(downloads, &tuning);
	return downloads;
}

static gint64
add_client_full(
	Fixture              *fx,
	const gchar          *name,
	const gchar          *address,
	const gchar          *base_path,
	const gchar          *username,
	const gchar          *password,
	const gchar          *category
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_DOWNLOAD_QBITTORRENT_NAME;
	spec.address = address;
	spec.base_path = base_path;
	spec.category = category;
	spec.username = username;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_HIGH;
	spec.enabled = TRUE;
	spec.secret = password;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_client_for(
	Fixture      *fx,
	AxisTestQbit *qb,
	const gchar  *name,
	const gchar  *password
){
	g_autofree gchar *base = axis_test_qbit_base(qb);

	return add_client_full(fx, name, base, NULL, USER, password, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	fx->generation = (AxisTestQbitGeneration)GPOINTER_TO_INT(data);
	axis_test_log_init();
	axis_test_log_reset();
	axis_download_qbittorrent_forget();
	fx->t = axis_test_library_new(4);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->qb = axis_test_qbit_new(fx->generation, USER, PASSWORD);
	fx->downloads = layer_new(fx);
	fx->client_id = add_client_for(fx, fx->qb, "Torrents", PASSWORD);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL)
		return;
	axis_downloads_unref(fx->downloads);
	axis_test_qbit_free(fx->qb);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
	axis_download_qbittorrent_forget();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->db, sql);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static guint
logged(const gchar *pattern)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	guint n = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		n += axis_test_log_count(levels[i], pattern);
	return n;
}

static AxisDownloadPollReport
poll_client(
	Fixture *fx,
	gint64   id
){
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, id, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

static AxisDownloadPollReport
poll(Fixture *fx)
{
	return poll_client(fx, fx->client_id);
}

/* A torrent (the support code's small valid one) of @name; its info-hash in @hash. */
static gint64
add_torrent_release_at(
	Fixture      *fx,
	const gchar  *name,
	gchar       **hash,
	GError      **error
){
	g_autoptr(GBytes) file = axis_test_download_torrent(name, hash);
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = name;
	request.title_label = "Some Film (2020)";
	request.module = "movies";
	axis_downloads_add(fx->downloads, &request, &id, error);
	return id;
}

static gint64
add_torrent_release(
	Fixture      *fx,
	const gchar  *name,
	gchar       **hash
){
	g_autoptr(GError) error = NULL;
	gint64 id = add_torrent_release_at(fx, name, hash, &error);

	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

static gint64
add_magnet_at(
	Fixture     *fx,
	const gchar *name,
	GError     **error
){
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = MAGNET;
	request.release_name = name;
	axis_downloads_add(fx->downloads, &request, &id, error);
	return id;
}

static gchar *
ref_of(
	Fixture *fx,
	gint64   id
){
	return q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
}

static void
assert_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state
){
	g_autofree gchar *now = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);

	g_assert_cmpstr(now, ==, state);
}

/* A record of a download that qBittorrent (the stand-in's torrents) holds, as
 * if Axis had added it. */
static gint64
insert_record(
	Fixture     *fx,
	const gchar *hash,
	const gchar *name,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("INSERT INTO download (client_id, client_name, client_ref, info_hash, protocol, "
		"source, release_name, state) VALUES (%" G_GINT64_FORMAT ", 'Torrents', '%s', '%s', 'torrent', 'file', '%s', '%s')"
		" RETURNING id", fx->client_id, hash, hash, name, state);
	g_autofree gchar *id = axis_test_db_query(fx->t->db, sql);

	return g_ascii_strtoll(id, NULL, 10);
}

static void
add_map(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/downloads", fx->volume_id, "downloads",
		&error));
	g_assert_no_error(error);
}

/* The restarted daemon: a new layer on the same database, a record left
 * 'sending' made older than the reconciliation wait. */
static void
restart(Fixture *fx)
{
	axis_downloads_unref(fx->downloads);
	axis_download_qbittorrent_forget();
	fx->downloads = layer_new(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour' WHERE state = 'sending'");
}

static AxisDownloadTestResult
test_client(
	Fixture *fx,
	gint64   id
){
	AxisDownloadTestResult result;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_test_client(fx->downloads, id, &result, &error));
	g_assert_no_error(error);
	return result;
}

static AxisDownloadClientState
client_state(
	Fixture *fx,
	gint64   id
){
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_get_client(fx->downloads, id, &client, &error));
	return client->state;
}

static gchar *
client_message(
	Fixture *fx,
	gint64   id
){
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_get_client(fx->downloads, id, &client, &error));
	return g_strdup(client->state_message != NULL ? client->state_message : "");
}

/* A client edited as it is, with the password and the user name as given. */
static void
edit_client(
	Fixture     *fx,
	gint64       id,
	const gchar *username,
	const gchar *password
){
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;

	g_assert_true(axis_downloads_get_client(fx->downloads, id, &client, &error));
	memset(&spec, 0, sizeof(spec));
	spec.name = client->name;
	spec.kind = client->kind;
	spec.address = client->address;
	spec.base_path = client->base_path;
	spec.category = client->category;
	spec.priority = client->priority;
	spec.enabled = TRUE;
	spec.username = username;
	spec.secret = password;
	g_assert_true(axis_downloads_save_client(fx->downloads, id, &spec, NULL, &error));
	g_assert_no_error(error);
}

static const gchar *
version_of(Fixture *fx)
{
	return fx->generation == AXIS_TEST_QBIT_V4 ? "v4.6.2 (Web API 2.9.3)" : "v5.0.1 (Web API 2.11.2)";
}

static const gchar *
paused_word(Fixture *fx)
{
	return fx->generation == AXIS_TEST_QBIT_V4 ? "pausedDL" : "stoppedDL";
}

/* ── Test (2.2, 2.4) ────────────────────────────────────────────── */

/* Spec: Add (a client that works); tasks 2.2, 2.4: what the test says, and the calls it makes */
static void
test_client_test(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result = test_client(fx, fx->client_id);
	g_autofree gchar *calls = NULL;
	g_autofree gchar *user = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpstr(result.version, ==, version_of(fx));
	g_assert_cmpint(result.category_exists, ==, 1);
	/* The priority is ignored, and the test says so. */
	g_assert_nonnull(strstr(result.note, "ignores the priority"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_cmpstr(client->version, ==, version_of(fx));
	g_assert_cmpint(client->category_exists, ==, 1);
	calls = axis_test_qbit_calls(fx->qb);
	g_assert_cmpstr(calls, ==, "login version webapi categories");
	/* Every request named the client's own address in its Referer and Origin, or it would be refused. */
	g_assert_cmpuint(axis_test_qbit_bad_requests(fx->qb), ==, 0);
	user = axis_test_qbit_last_login_user(fx->qb);
	g_assert_cmpstr(user, ==, USER);
	/* Nothing was changed at qBittorrent. */
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "create-category"), ==, 0);
}

/* Spec: The category is made */
static void
test_client_test_makes_the_category(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;

	axis_test_qbit_clear_categories(fx->qb);
	axis_test_qbit_add_category(fx->qb, "movies");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "\"axis\" did not exist"));
	g_assert_nonnull(strstr(result.note, "it was made"));
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	g_assert_true(axis_test_qbit_has_category(fx->qb, "axis"));
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "create-category:axis"), ==, 1);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
	/* Once there, the next test makes nothing. */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "create-category"), ==, 1);

	/* A 409 from a category that is not there afterwards: it is not taken to mean "exists". */
	axis_test_qbit_clear_categories(fx->qb);
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_CATEGORY_409, TRUE);
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_false(axis_test_qbit_has_category(fx->qb, "axis"));
}

/* Spec: Wrong password; Task 3.6: not retried by itself; the password is in no message */
static void
test_client_wrong_password(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client_for(fx, fx->qb, "Wrong", WRONG_PASSWORD);
	AxisTestQbit *closed = axis_test_qbit_new(fx->generation, USER, PASSWORD);
	g_autofree gchar *closed_base = axis_test_qbit_base(closed);
	g_autofree gchar *message = NULL;
	AxisDownloadPollReport report;
	AxisDownloadTestResult result;
	guint requests;
	gint64 down;

	result = test_client(fx, wrong);
	g_assert_nonnull(strstr(result.note, "refused the user name or password"));
	g_assert_nonnull(strstr(result.note, "not trying again"));
	g_assert_null(strstr(result.note, WRONG_PASSWORD));
	g_assert_null(strstr(result.note, PASSWORD));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, wrong), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_failed_logins(fx->qb), ==, 1);
	/* Nothing else was asked after the refusal. */
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "version"), ==, 0);

	/* Tracking does not log in again by itself, poll after poll: no request at all. */
	requests = axis_test_plex_n_requests(axis_test_qbit_server(fx->qb));
	report = poll_client(fx, wrong);
	g_assert_false(report.reached);
	report = poll_client(fx, wrong);
	g_assert_false(report.reached);
	report = poll_client(fx, wrong);
	g_assert_false(report.reached);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_qbit_server(fx->qb)), ==, requests);
	g_assert_cmpuint(axis_test_qbit_failed_logins(fx->qb), ==, 1);
	g_assert_cmpint(client_state(fx, wrong), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	message = client_message(fx, wrong);
	g_assert_nonnull(strstr(message, "not trying again"));
	g_assert_null(strstr(message, WRONG_PASSWORD));

	/* A test is one deliberate login, whatever the wait. */
	result = test_client(fx, wrong);
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);

	/* Saving the right password ends the wait: the next poll logs in. */
	edit_client(fx, wrong, NULL, PASSWORD);
	report = poll_client(fx, wrong);
	g_assert_true(report.reached);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 3);
	g_assert_cmpint(client_state(fx, wrong), ==, AXIS_DOWNLOAD_CLIENT_OK);

	/* Nothing listening: unreachable, not refused. */
	axis_test_qbit_free(closed);
	down = add_client_full(fx, "Down", closed_base, NULL, USER, PASSWORD, NULL);
	result = test_client(fx, down);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, down), ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);
	g_assert_cmpuint(logged("*" WRONG_PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" PASSWORD "*"), ==, 0);
}

/* Spec: Banned; Task 3.6.  A 403 to the login is a ban: said plainly, never retried in a loop. */
static void
test_client_banned(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client_for(fx, fx->qb, "Wrong", WRONG_PASSWORD);
	g_autofree gchar *message = NULL;
	AxisDownloadTestResult result;
	AxisDownloadPollReport report;
	guint requests;
	guint i;

	/* Five wrong logins (qBittorrent's default) and the address is banned. */
	for (i = 0; i < 5; i++) {
		result = test_client(fx, wrong);
		axis_download_test_result_clear(&result);
	}
	g_assert_cmpuint(axis_test_qbit_failed_logins(fx->qb), ==, 5);
	g_assert_cmpuint(axis_test_qbit_banned_logins(fx->qb), ==, 0);

	/* The sixth finds it banned: one login, and the message says what it means. */
	result = test_client(fx, wrong);
	g_assert_nonnull(strstr(result.note, "banned"));
	g_assert_nonnull(strstr(result.note, "not trying again"));
	g_assert_nonnull(strstr(result.note, "does not retry in a loop"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, wrong), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_cmpuint(axis_test_qbit_banned_logins(fx->qb), ==, 1);

	/* However many polls follow, and of whichever client, nothing more is asked of qBittorrent
	 * by this client: no request at all. */
	requests = axis_test_plex_n_requests(axis_test_qbit_server(fx->qb));
	for (i = 0; i < 5; i++) {
		report = poll_client(fx, wrong);
		g_assert_false(report.reached);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_qbit_server(fx->qb)), ==, requests);
	g_assert_cmpuint(axis_test_qbit_banned_logins(fx->qb), ==, 1);
	message = client_message(fx, wrong);
	g_assert_nonnull(strstr(message, "banned"));
	g_assert_null(strstr(message, WRONG_PASSWORD));
}

/* Spec: Banned, in the middle of a life: a 403 that is a ban and not a lost session */
static void
test_client_banned_while_running(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *message = NULL;
	AxisDownloadTestResult result;
	AxisDownloadPollReport report;
	guint requests;
	guint i;

	g_assert_true(poll(fx).reached);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);

	/* The address is banned now: the list is answered 403, the kind logs in once (as for a lost
	 * session), is answered 403 again, and stops. */
	axis_test_qbit_set_banned(fx->qb, TRUE);
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_cmpuint(axis_test_qbit_banned_logins(fx->qb), ==, 1);
	message = client_message(fx, fx->client_id);
	g_assert_nonnull(strstr(message, "banned"));

	requests = axis_test_plex_n_requests(axis_test_qbit_server(fx->qb));
	for (i = 0; i < 4; i++)
		g_assert_false(poll(fx).reached);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_qbit_server(fx->qb)), ==, requests);

	/* Lifted at qBittorrent: a test is one deliberate login, which ends the wait. */
	axis_test_qbit_set_banned(fx->qb, FALSE);
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_true(poll(fx).reached);
}

/* Something that answers but is not qBittorrent's web interface: failing, not refused */
static void
test_client_not_qbittorrent(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;

	axis_test_qbit_set_raw(fx->qb, "/auth/login", 200, "text/html", "<html><body>Welcome to nginx</body></html>");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "cannot be read"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	axis_test_qbit_clear_raw(fx->qb, "/auth/login");

	/* A login that says yes and sets no session. */
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_NO_COOKIE, TRUE);
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "set no session"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_NO_COOKIE, FALSE);

	/* A version that is not one. */
	axis_test_qbit_set_raw(fx->qb, "/app/version", 200, "text/html", "<html>hello</html>");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "may not be qBittorrent"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	axis_test_qbit_clear_raw(fx->qb, "/app/version");

	/* No list of categories. */
	axis_test_qbit_set_raw(fx->qb, "/torrents/categories", 200, "application/json", "[]");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "cannot be read"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	axis_test_qbit_clear_raw(fx->qb, "/torrents/categories");

	/* A web API version that cannot be had is not a failure. */
	axis_test_qbit_set_raw(fx->qb, "/app/webapiVersion", 404, "text/plain", "Not Found");
	result = test_client(fx, fx->client_id);
	g_assert_cmpstr(result.version, ==, fx->generation == AXIS_TEST_QBIT_V4 ? "v4.6.2" : "v5.0.1");
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
}

/* ── The user name ──────────────────────────────────────────────── */

/* Spec: Saved and shown.  The user name is the client's own, kept unless a new one is typed. */
static void
test_user_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *user = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	AxisDownloadTestResult result;
	g_autofree gchar *base = axis_test_qbit_base(fx->qb);
	g_autofree gchar *long_name = g_strnfill(129, 'a');

	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	user = axis_test_qbit_last_login_user(fx->qb);
	g_assert_cmpstr(user, ==, USER);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpstr(client->username, ==, USER);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* Not given: kept. */
	edit_client(fx, fx->client_id, NULL, NULL);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpstr(client->username, ==, USER);
	g_assert_true(client->secret_set);
	g_clear_pointer(&client, axis_download_client_info_free);
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);

	/* Given empty: removed, and the login is "admin", which this qBittorrent does not know. */
	edit_client(fx, fx->client_id, "", NULL);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_null(client->username);
	g_clear_pointer(&client, axis_download_client_info_free);
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_clear_pointer(&user, g_free);
	user = axis_test_qbit_last_login_user(fx->qb);
	g_assert_cmpstr(user, ==, "admin");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);

	/* Given again, with spaces around it: trimmed. */
	edit_client(fx, fx->client_id, "  " USER " ", NULL);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpstr(client->username, ==, USER);
	g_clear_pointer(&client, axis_download_client_info_free);
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);

	/* Refused: a control character, and too long. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "Bad user";
	spec.kind = AXIS_DOWNLOAD_QBITTORRENT_NAME;
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = TRUE;
	spec.username = "bad\nuser";
	g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	spec.username = long_name;
	g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	assert_q(fx, "1", "SELECT count(*) FROM download_client");
}

/* ── Add (2.5) ──────────────────────────────────────────────────── */

/* Spec: Add.  A torrent file: added in the category, found again by its info-hash, recorded by it. */
static void
test_add_torrent_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *category = axis_test_qbit_category_of(fx->qb, hash);
	g_autofree gchar *file_name = axis_test_qbit_last_add(fx->qb, "torrents-name");
	g_autofree gchar *paused = axis_test_qbit_last_add(fx->qb, "paused");
	g_autofree gchar *stopped = axis_test_qbit_last_add(fx->qb, "stopped");
	g_autofree gchar *form_category = axis_test_qbit_last_add(fx->qb, "category");
	g_autofree gchar *urls = axis_test_qbit_last_add(fx->qb, "urls");
	g_autofree gchar *calls = axis_test_qbit_calls(fx->qb);
	g_autofree gchar *expected = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpstr(ref, ==, hash);
	assert_state(fx, id, "queued");
	g_assert_cmpstr(category, ==, "axis");
	g_assert_cmpstr(form_category, ==, "axis");
	g_assert_cmpstr(file_name, ==, "Some.Film.2020.1080p.torrent");
	g_assert_null(urls);
	/* Never added paused: both of the generations' words say so. */
	g_assert_cmpstr(paused, ==, "false");
	g_assert_cmpstr(stopped, ==, "false");
	assert_q(fx, hash, "SELECT info_hash FROM download");
	/* A session, the category made sure of, the hash not there yet, the add, and the torrent asked for by its hash. */
	expected = g_strdup_printf("login categories info:hashes=%s add info:hashes=%s", hash, hash);
	g_assert_cmpstr(calls, ==, expected);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 1);
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpstr(info->info_hash, ==, hash);
	g_assert_cmpuint(axis_test_qbit_bad_requests(fx->qb), ==, 0);
}

/* Spec: Add.  A magnet link. */
static void
test_add_magnet(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id = add_magnet_at(fx, "Some.Magnet.Release", &error);
	g_autofree gchar *ref = NULL;
	g_autofree gchar *category = axis_test_qbit_category_of(fx->qb, MAGNET_HASH);
	g_autofree gchar *urls = axis_test_qbit_last_add(fx->qb, "urls");
	g_autofree gchar *file_name = axis_test_qbit_last_add(fx->qb, "torrents-name");

	g_assert_no_error(error);
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, MAGNET_HASH);
	g_assert_cmpstr(category, ==, "axis");
	g_assert_cmpstr(urls, ==, MAGNET);
	g_assert_null(file_name);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	assert_state(fx, id, "queued");
	/* The same magnet twice at one client is refused by the layer: nothing more is sent. */
	add_magnet_at(fx, "Some.Magnet.Release", &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
}

/* Task 2.5: the category is made before the torrent exists; a priority is ignored */
static void
test_add_makes_the_category_first(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	g_autofree gchar *calls = NULL;
	g_autofree gchar *expected = NULL;

	axis_test_qbit_clear_categories(fx->qb);
	add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	g_assert_true(axis_test_qbit_has_category(fx->qb, "axis"));
	calls = axis_test_qbit_calls(fx->qb);
	expected = g_strdup_printf("login categories create-category:axis info:hashes=%s add info:hashes=%s", hash, hash);
	g_assert_cmpstr(calls, ==, expected);
}

/* Spec: Already there.  A torrent qBittorrent holds is not Axis's to take over. */
static void
test_add_already_there(Fixture *fx, gconstpointer data)
{
	g_autoptr(GBytes) file = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *category = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	file = axis_test_download_torrent("Some.Film.2020.1080p", &hash);
	/* In a category of a person's. */
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "manual", "uploading", 1000, 1.0, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "already"));
	g_assert_nonnull(strstr(error->message, "manual"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	category = axis_test_qbit_category_of(fx->qb, hash);
	g_assert_cmpstr(category, ==, "manual");

	/* With none, and with qBittorrent's way of merging (it would answer "Ok."): the same. */
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", NULL, "uploading", 1000, 1.0, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_MERGES, TRUE);
	g_clear_error(&error);
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	g_clear_pointer(&category, g_free);
	category = axis_test_qbit_category_of(fx->qb, hash);
	g_assert_null(category);
}

/* qBittorrent answers "Fails." and does not say why: refused, nothing added, said so */
static void
test_add_refused_by_qbittorrent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_FAILS, TRUE);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "Fails."));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
}

/* Spec: Added but not shown.  "Ok." and no torrent: uncertain, never sent again. */
static void
test_add_ok_but_not_shown(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id;

	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_HIDDEN, TRUE);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN);
	assert_state(fx, id, "sending");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	/* It was asked for, and again, before giving up. */
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "info:hashes="), >, 2);

	/* Never sent again by anything: within the wait the record is left alone, after it the torrent
	 * (which qBittorrent does not show) is not found and the record failed. */
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 0);
	assert_state(fx, id, "sending");
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
}

/* A torrent qBittorrent put in another category than the one asked for is not followed */
static void
test_add_in_another_category(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *category = NULL;
	gint64 id;

	/* A rule of the person's at qBittorrent puts the new torrent in "manual": it is not in Axis's
	 * category, so Axis cannot follow it, says so, and leaves it where it is. */
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_RECATEGORISES, TRUE);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "not in Axis's category"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "delete"), ==, 0);
	category = axis_test_qbit_category_of(fx->qb, hash);
	g_assert_cmpstr(category, ==, "manual");
}

/* Refused before anything is sent: a release for the other protocol cannot reach this kind through the layer */
static void
test_add_refuses_usenet(Fixture *fx, gconstpointer data)
{
	AxisDownloadRequest request;
	g_autoptr(GBytes) file = g_bytes_new("<nzb/>", 6);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.client_id = fx->client_id;
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_qbit_server(fx->qb)), ==, 0);
}

/* Spec: Session lost during an add */
static void
test_add_with_a_lost_session_is_not_sent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *problem = NULL;
	gint64 id;
	gint64 again;

	/* Something to hold a session. */
	g_assert_true(poll(fx).reached);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);

	/* The session ends between the questions before the add and the add itself. */
	axis_test_qbit_expire_before(fx->qb, "/torrents/add");
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "no longer accepts the session"));
	/* Reported as not sent: failed, nothing added, the add asked for once and not repeated, no login in between. */
	assert_state(fx, id, "failed");
	problem = q(fx, "SELECT coalesce(problem, '') FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(problem, "Not sent"));
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised:/torrents/add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);

	/* Nothing sends it again by itself: polls and time change nothing. */
	poll(fx);
	poll(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	poll(fx);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);

	/* The next add logs in again and goes through, once. */
	g_clear_error(&error);
	g_clear_pointer(&hash, g_free);
	again = add_torrent_release_at(fx, "Some.Film.2020.1080p.Again", &hash, &error);
	g_assert_no_error(error);
	assert_state(fx, again, "queued");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 1);
	ref = ref_of(fx, again);
	g_assert_cmpstr(ref, ==, hash);
}

/* A session that ended while idle is noticed before the add (by the questions that come first), which is then not refused */
static void
test_add_after_an_idle_session(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id;

	g_assert_true(poll(fx).reached);
	axis_test_qbit_expire_session(fx->qb);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	assert_state(fx, id, "queued");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised"), ==, 1);
	g_assert_true(axis_test_qbit_has(fx->qb, hash));
}

/* A wrong password at add time: not sent, failed, and the password is in no message. */
static void
test_add_with_a_wrong_password(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client_for(fx, fx->qb, "Wrong", WRONG_PASSWORD);
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.client_id = wrong;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p.Wrong";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH);
	g_assert_null(strstr(error->message, WRONG_PASSWORD));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);
}

/* ── Tracking (2.2, 2.4) ────────────────────────────────────────── */

/* Spec: Not Axis's; task 3.4 */
static void
test_other_categories_are_never_touched(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *removal = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	/* A person's categories, none, a sub-category of Axis's (which qBittorrent's filter may take
	 * along), and Axis's own category on a torrent Axis did not add. */
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_SUBCATEGORIES, TRUE);
	axis_test_qbit_put(fx->qb, "aa01aa01aa01aa01aa01aa01aa01aa01aa01aa01", "Holiday.Videos", "manual", "downloading", 100, 0.5,
		"/downloads", "/downloads/Holiday.Videos");
	axis_test_qbit_put(fx->qb, "aa02aa02aa02aa02aa02aa02aa02aa02aa02aa02", "Holiday.Videos.Two", "manual", "uploading", 100, 1.0,
		"/downloads", "/downloads/Holiday.Videos.Two");
	axis_test_qbit_put(fx->qb, "aa03aa03aa03aa03aa03aa03aa03aa03aa03aa03", "No.Category.Seed", NULL, "uploading", 100, 1.0,
		"/downloads", "/downloads/No.Category.Seed");
	axis_test_qbit_put(fx->qb, "aa04aa04aa04aa04aa04aa04aa04aa04aa04aa04", "Other.Film.2018", "axis", "downloading", 100, 0.5,
		"/downloads", "/downloads/Other.Film.2018");
	axis_test_qbit_put(fx->qb, "aa05aa05aa05aa05aa05aa05aa05aa05aa05aa05", "Sub.Category.Film", "axis/sub", "downloading", 100,
		0.5, "/downloads", "/downloads/Sub.Category.Film");

	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 2);		/* Axis's category only: its own and one of another program's */
	g_assert_cmpuint(report.ignored, ==, 1);	/* the other program's, never touched */
	assert_q(fx, "1", "SELECT count(*) FROM download");
	assert_q(fx, "0", "SELECT count(*) FROM download WHERE client_ref LIKE 'aa0%'");
	/* What the kind asked for: the category. */
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "info:category=axis"), ==, 1);

	/* Removal of what Axis did not add is refused, and nothing is sent. */
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa01aa01aa01aa01aa01aa01aa01aa01aa01aa01", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa03aa03aa03aa03aa03aa03aa03aa03aa03aa03", TRUE, &error));
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa04aa04aa04aa04aa04aa04aa04aa04aa04aa04", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "delete"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 6);

	/* Its own: removed, and only it. */
	g_assert_true(axis_downloads_remove_ref(fx->downloads, fx->client_id, hash, FALSE, &error));
	g_assert_no_error(error);
	g_assert_false(axis_test_qbit_has(fx->qb, hash));
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 5);
	poll(fx);
	poll(fx);
	removal = g_strdup_printf("delete:%s:keep", hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "delete"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, removal), ==, 1);
	assert_state(fx, id, "removed");
	assert_q(fx, "1", "SELECT count(*) FROM download");
}

/* Task 2.1: the fixture through the whole layer; Spec: Seeding */
static void
test_fixture_answers_through_the_layer(Fixture *fx, gconstpointer data)
{
	static const gchar *const places[] = {
		"Seeding.Film.2018.1080p", "Stalled.Seed.2017", "Done.And.Paused.2017", "Done.And.Stopped.2016", "Old.Client.Film",
		"Relative.Content.Path", "Double  Space  Name", NULL
	};
	g_autofree gchar *text = fixture_text("torrents.json");
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 downloading;
	gint64 queued;
	gint64 checking;
	gint64 paused;
	gint64 stopped;
	gint64 moving;
	gint64 seeding;
	gint64 stalled_up;
	gint64 done_paused;
	gint64 failed;
	gint64 missing;
	gint64 old;
	gint64 spaced;
	guint i;

	downloading = insert_record(fx, "0101010101010101010101010101010101010101", "Some.Film.2020.1080p.WEB-DL", "queued");
	queued = insert_record(fx, "0202020202020202020202020202020202020202", "Another.Film.2019.720p", "queued");
	checking = insert_record(fx, "0303030303030303030303030303030303030303", "Checking.Series.S01E01.720p", "downloading");
	paused = insert_record(fx, "0505050505050505050505050505050505050505", "Paused.Film.2018.1080p", "queued");
	stopped = insert_record(fx, "0606060606060606060606060606060606060606", "Stopped.Film.2018.1080p", "queued");
	moving = insert_record(fx, "0707070707070707070707070707070707070707", "Moving.Film.2021.1080p", "queued");
	seeding = insert_record(fx, "0808080808080808080808080808080808080808", "Seeding.Film.2018.1080p", "downloading");
	stalled_up = insert_record(fx, "0909090909090909090909090909090909090909", "Stalled.Seed.2017", "downloading");
	done_paused = insert_record(fx, "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a", "Done.And.Paused.2017", "downloading");
	failed = insert_record(fx, "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c", "Broken.Film.2017.720p", "downloading");
	missing = insert_record(fx, "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d", "Missing.Film.2017.720p", "downloading");
	old = insert_record(fx, "1919191919191919191919191919191919191919", "Old.Client.Film", "downloading");
	spaced = insert_record(fx, "1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b", "Double Space Name", "downloading");
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", text);
	for (i = 0; places[i] != NULL; i++) {
		g_autofree gchar *place = g_build_filename(fx->disk, "downloads", places[i], NULL);

		axis_test_storage_mkdir(place);
	}

	/* Without a path mapping nothing that finished is completed, and it says why. */
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.completed, ==, 0);
	g_assert_cmpuint(report.waiting, ==, 5);	/* the five of these with a place at qBittorrent */
	assert_state(fx, seeding, "downloading");
	assert_state(fx, downloading, "downloading");
	assert_state(fx, queued, "queued");
	assert_state(fx, checking, "queued");
	assert_state(fx, paused, "paused");
	assert_state(fx, stopped, "paused");
	assert_state(fx, moving, "downloading");
	assert_state(fx, failed, "failed");
	assert_state(fx, missing, "failed");
	g_assert_true(axis_downloads_get(fx->downloads, downloading, &info, &error));
	g_assert_cmpint(info->size_bytes, ==, 4000000000);
	g_assert_cmpint(info->done_bytes, ==, 2000000000);
	g_assert_cmpint(info->rate_bps, ==, 5242880);
	g_assert_cmpint(info->eta_s, ==, 280);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, failed, &info, &error));
	g_assert_cmpstr(info->message, ==, "qBittorrent reports this torrent as in error");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, moving, &info, &error));
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
	g_assert_null(info->rel_path);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, seeding, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path mapping"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Mapped: a finished torrent is completed, whatever it is doing, at its content path. */
	add_map(fx);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 5);	/* seeding, stalled, paused and finished, the old client's, the spaced one */
	assert_state(fx, seeding, "completed");
	assert_state(fx, stalled_up, "completed");
	assert_state(fx, done_paused, "completed");
	assert_state(fx, old, "completed");
	assert_state(fx, spaced, "completed");
	g_assert_true(axis_downloads_get(fx->downloads, seeding, &info, &error));
	g_assert_cmpint(info->volume_id, ==, fx->volume_id);
	g_assert_cmpstr(info->rel_path, ==, "downloads/Seeding.Film.2018.1080p");
	g_assert_cmpstr(info->client_path, ==, "/downloads/Seeding.Film.2018.1080p");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, old, &info, &error));
	g_assert_cmpstr(info->client_path, ==, "/downloads/Old.Client.Film");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, spaced, &info, &error));
	g_assert_cmpstr(info->client_path, ==, "/downloads/Double  Space  Name");
	g_clear_pointer(&info, axis_download_info_free);

	/* It stays completed while it seeds: poll after poll changes nothing. */
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 0);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 0);
	assert_state(fx, seeding, "completed");
	assert_state(fx, done_paused, "completed");
}

/* Spec: Seeding.  A torrent that finishes and goes on seeding, in any of the ways, is completed once and stays so. */
static void
test_seeding_stays_completed(Fixture *fx, gconstpointer data)
{
	static const gchar *const words[] = { "uploading", "stalledUP", "queuedUP", "forcedUP", "checkingUP", NULL };
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *stopped = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	guint i;

	axis_test_storage_mkdir(place);
	add_map(fx);
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	assert_state(fx, id, "downloading");
	axis_test_qbit_set_state(fx->qb, hash, "uploading", 1.0);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	assert_state(fx, id, "completed");
	for (i = 1; words[i] != NULL; i++) {
		axis_test_qbit_set_state(fx->qb, hash, words[i], 1.0);
		report = poll(fx);
		g_assert_cmpuint(report.completed, ==, 0);
		assert_state(fx, id, "completed");
	}
	/* Paused after seeding (4.x), or stopped (5.x): still finished. */
	stopped = g_strdup(fx->generation == AXIS_TEST_QBIT_V4 ? "pausedUP" : "stoppedUP");
	axis_test_qbit_set_state(fx->qb, hash, stopped, 1.0);
	poll(fx);
	assert_state(fx, id, "completed");
}

/* Spec: Renamed words.  Paused (4.x) and stopped (5.x) downloads are paused. */
static void
test_paused_and_stopped(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", paused_word(fx), 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	assert_state(fx, id, "paused");
	axis_test_qbit_set_state(fx->qb, hash, "downloading", 0.5);
	poll(fx);
	assert_state(fx, id, "downloading");
}

/* Spec: Unknown word.  A state word the kind does not know keeps the last state; the word is logged once. */
static void
test_unknown_state_keeps_the_last_state(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	guint i;

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	assert_state(fx, id, "downloading");
	axis_test_qbit_set_state(fx->qb, hash, "wibbling", 0.45);
	for (i = 0; i < 3; i++) {
		poll(fx);
		assert_state(fx, id, "downloading");
	}
	g_assert_cmpuint(logged("*qBittorrent client Torrents reported a torrent state Axis does not know (\"wibbling\")*"), ==, 1);
	/* The word qBittorrent itself calls "unknown" is one too. */
	axis_test_qbit_set_state(fx->qb, hash, "unknown", 0.45);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_cmpuint(logged("*(\"unknown\")*"), ==, 1);
	/* A known word again. */
	axis_test_qbit_set_state(fx->qb, hash, paused_word(fx), 0.45);
	poll(fx);
	assert_state(fx, id, "paused");
	/* Never seen before: queued, as it was recorded. */
	axis_test_qbit_put(fx->qb, "bb01bb01bb01bb01bb01bb01bb01bb01bb01bb01", "Never.Seen", "axis", "wibbling", 100, 0.1, "/downloads",
		"/downloads/Never.Seen");
	g_assert_cmpuint(poll(fx).ignored, ==, 1);
	g_assert_cmpuint(logged("*\"wibbling\"*"), ==, 1);
}

/* A torrent that was completed and is in a word nobody knows is not taken as finished */
static void
test_unknown_state_never_completes(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	axis_test_storage_mkdir(place);
	add_map(fx);
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "uploading", 1000, 1.0, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	assert_state(fx, id, "completed");
	axis_test_qbit_set_state(fx->qb, hash, "wibbling", 1.0);
	poll(fx);
	/* A record that is completed is not taken back by a word nobody knows. */
	assert_state(fx, id, "completed");
}

/* A list that cannot be read changes nothing (the torrents would look gone) */
static void
test_unreadable_lists_change_nothing(Fixture *fx, gconstpointer data)
{
	static const gchar *const bodies[] = {
		"{\"torrents\": []}", "{}", "null", "[1, 2", "<html>Bad gateway</html>", "", "\"Ok.\"", NULL
	};
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	guint i;

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	assert_state(fx, id, "downloading");
	for (i = 0; bodies[i] != NULL; i++) {
		axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", bodies[i]);
		report = poll(fx);
		g_assert_false(report.reached);
		assert_state(fx, id, "downloading");
	}
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 502, "text/html", "<html>Bad gateway</html>");
	g_assert_false(poll(fx).reached);
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	/* And everything is as before once it answers. */
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
}

/* Third review, item 5: an entry that cannot be read (no hash, no name) is
 * left out of the list, and counted as something the client has that Axis
 * could not read.  Another category's is listed (review of the fixes, item 1:
 * whether anything of the client's is at a path does not depend on its
 * category). */
static void
test_unreadable_entries_are_counted(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = NULL;
	guint unreadable = 99;

	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json",
		"[{\"hash\":\"0101010101010101010101010101010101010101\",\"name\":\"Some.Film.2020.1080p\",\"category\":\"axis\","
		"\"state\":\"downloading\",\"size\":1000,\"progress\":0.5,\"save_path\":\"/downloads\"},"
		"{\"hash\":\"0202020202020202020202020202020202020202\",\"name\":\"Other.2019\",\"category\":\"manual\","
		"\"state\":\"downloading\"}]");
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, 2);
	g_assert_cmpuint(unreadable, ==, 0);
	g_clear_pointer(&items, g_ptr_array_unref);
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json",
		"[{\"hash\":\"0101010101010101010101010101010101010101\",\"name\":\"Some.Film.2020.1080p\",\"category\":\"axis\","
		"\"state\":\"downloading\",\"size\":1000,\"progress\":0.5,\"save_path\":\"/downloads\"},"
		"{\"hash\":\"not a hash\",\"name\":\"Broken.2018\",\"category\":\"axis\",\"state\":\"downloading\"}]");
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, 1);
	g_assert_cmpuint(unreadable, >, 0);
	g_clear_pointer(&items, g_ptr_array_unref);
	/* A list cut at the most read is not whole: what was not looked at is not
	 * taken for absent. */
	{
		GString *text = g_string_new("[");
		guint i;

		for (i = 1; i <= AXIS_QBIT_MAX_TORRENTS + 1; i++)
			g_string_append_printf(text, "%s{\"hash\": \"%040x\", \"name\": \"T%u\", \"category\": \"axis\", \"state\": "
				"\"queuedDL\", \"save_path\": \"/downloads\"}", i > 1 ? "," : "", i, i);
		g_string_append_c(text, ']');
		axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", text->str);
		g_string_free(text, TRUE);
	}
	unreadable = 0;
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, AXIS_QBIT_MAX_TORRENTS);
	g_assert_cmpuint(unreadable, >, 0);
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
}

/* An unreachable client changes nothing about its downloads. */
static void
test_down_client_fails_nothing(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	poll(fx);
	axis_test_qbit_set_mode(fx->qb, AXIS_TEST_QBIT_DOWN);
	report = poll(fx);
	g_assert_false(report.reached);
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);
	axis_test_qbit_set_mode(fx->qb, AXIS_TEST_QBIT_NORMAL);
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* ── Session (2.2, 3.5) ─────────────────────────────────────────── */

/* One login per connection: any number of calls after it use the same session. */
static void
test_one_login_per_connection(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;
	guint i;

	add_torrent_release(fx, "Some.Film.2020.1080p", NULL);
	for (i = 0; i < 4; i++)
		poll(fx);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised"), ==, 0);
	/* A test logs in afresh (it is the password that is being tested). */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);
	for (i = 0; i < 3; i++)
		poll(fx);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised"), ==, 0);
}

/* Spec: Session lost during a list */
static void
test_list_recovers_from_a_lost_session(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 1);

	/* The session ends in the middle of the list: it is refused, one login, and the list is asked once more. */
	axis_test_qbit_expire_before(fx->qb, "/torrents/info");
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 1);
	assert_state(fx, id, "downloading");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 2);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised:/torrents/info"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "info:category=axis"), ==, 1);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);

	/* An idle session that ended: the same. */
	axis_test_qbit_expire_session(fx->qb);
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 3);

	/* A session that is refused again right after a login is not asked for a third time. */
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 403, "text/plain", "Forbidden");
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "login"), ==, 4);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* ── Remove (2.7) ───────────────────────────────────────────────── */

static void
test_remove_with_and_without_data(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *with_data = NULL;
	g_autofree gchar *without_data = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	gint64 second = add_torrent_release(fx, "Another.Film.2019.720p", &other);

	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(axis_test_qbit_has(fx->qb, hash));
	with_data = g_strdup_printf("delete:%s:data", hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, with_data), ==, 1);
	g_assert_true(axis_test_qbit_has(fx->qb, other));

	g_assert_true(axis_downloads_remove(fx->downloads, second, FALSE, &error));
	assert_state(fx, second, "removed");
	without_data = g_strdup_printf("delete:%s:keep", other);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, without_data), ==, 1);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);
	g_assert_cmpuint(axis_test_qbit_bad_requests(fx->qb), ==, 0);
}

/* import.torrents_after: removed with its data when the setting says remove, left to seed when it says seed */
static void
test_remove_after_import_honours_the_setting(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *with_data = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'imported'");
	/* Seed: the layer does not call the kind. */
	g_assert_false(axis_downloads_remove_imported(fx->downloads, id, FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "delete"), ==, 0);
	g_assert_true(axis_test_qbit_has(fx->qb, hash));
	/* Remove: with its files, once. */
	g_assert_true(axis_downloads_remove_imported(fx->downloads, id, TRUE, &error));
	g_assert_no_error(error);
	with_data = g_strdup_printf("delete:%s:data", hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, with_data), ==, 1);
	g_assert_false(axis_test_qbit_has(fx->qb, hash));
	assert_state(fx, id, "imported");
}

/* A removal qBittorrent does not do is not taken for done; one that loses its session is made again once */
static void
test_remove_refusals(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	/* "200" and the torrent is still listed. */
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_DELETE_KEEPS, TRUE);
	g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "still lists"));
	g_clear_error(&error);
	assert_state(fx, id, "queued");
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_DELETE_KEEPS, FALSE);

	/* An error status. */
	axis_test_qbit_set_raw(fx->qb, "/torrents/delete", 500, "text/plain", "Internal Server Error");
	g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_clear_error(&error);
	assert_state(fx, id, "queued");
	axis_test_qbit_clear_raw(fx->qb, "/torrents/delete");

	/* The session ends right at the removal: it removes nothing by itself and so is made again, once. */
	axis_test_qbit_expire_before(fx->qb, "/torrents/delete");
	g_assert_true(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(axis_test_qbit_has(fx->qb, hash));
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "unauthorised:/torrents/delete"), ==, 1);

	/* A torrent a person already removed: qBittorrent would answer the delete
	 * 200 either way, so its list is asked first; not listed, it is gone (not
	 * removed by Axis), and no delete is sent. */
	g_clear_pointer(&hash, g_free);
	id = add_torrent_release(fx, "Another.Film.2019.720p", &hash);
	axis_test_qbit_drop(fx->qb, hash);
	g_assert_false(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE);
	g_clear_error(&error);
	assert_state(fx, id, "gone");
	{
		g_autofree gchar *deleted = g_strdup_printf("delete:%s", hash);

		g_assert_cmpuint(axis_test_qbit_count(fx->qb, deleted), ==, 0);
	}
}

/* Review of the fixes, items 1, 2 and 7, through qBittorrent's own answer:
 * what it has is asked of it whole, not of Axis's category only (a torrent in
 * another category, another program's or a person's, is still something at
 * its path); each name is kept exactly as qBittorrent gave it, for comparing
 * with names on disk (it was cleaned before it was kept); and an entry whose
 * hash is read and not its name is said, by its hash and why. */
static void
test_everything_listed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *why = NULL;
	const AxisDownloadItem *spaced = NULL;
	const AxisDownloadItem *theirs = NULL;
	guint unreadable = 99;
	guint i;

	axis_test_qbit_put(fx->qb, "0101010101010101010101010101010101010101", "Double  Space  Name ", "axis",
		"stalledUP", 1000, 1.0, "/downloads/axis", "/downloads/axis/Double  Space  Name ");
	axis_test_qbit_put(fx->qb, "0202020202020202020202020202020202020202", "Their.Own.2019", "tv", "stalledUP", 1000,
		1.0, "/downloads/tv", "/downloads/tv/Their.Own.2019");
	g_assert_true(axis_downloads_client_items_full(fx->downloads, fx->client_id, &items, &unreadable, &why, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(unreadable, ==, 0);
	g_assert_null(why);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "info"), >=, 1);
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(item->client_ref, "0101010101010101010101010101010101010101") == 0)
			spaced = item;
		if (g_strcmp0(item->client_ref, "0202020202020202020202020202020202020202") == 0)
			theirs = item;
	}
	g_assert_nonnull(spaced);
	g_assert_cmpstr(spaced->name, ==, "Double Space Name");
	g_assert_cmpstr(spaced->own_name, ==, "Double  Space  Name ");
	g_assert_nonnull(theirs);
	g_assert_cmpstr(theirs->category, ==, "tv");
	g_clear_pointer(&items, g_ptr_array_unref);

	/* A torrent of Axis's whose hash is read and not its name. */
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json",
		"[{\"hash\":\"0101010101010101010101010101010101010101\",\"name\":\"Some.Film.2020.1080p\",\"category\":\"axis\","
		"\"state\":\"downloading\",\"size\":1000,\"progress\":0.5,\"save_path\":\"/downloads\"},"
		"{\"hash\":\"0303030303030303030303030303030303030303\",\"category\":\"axis\",\"state\":\"downloading\"}]");
	g_assert_true(axis_downloads_client_items_full(fx->downloads, fx->client_id, &items, &unreadable, &why, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, 1);
	g_assert_cmpuint(unreadable, ==, 1);
	g_assert_nonnull(why);
	g_assert_nonnull(strstr(why, "0303030303030303030303030303030303030303"));
	g_assert_nonnull(strstr(why, "it has no name Axis can read"));
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
}

/* Review of the fixes, item 7, through qBittorrent's own answer: one entry
 * of Axis's category whose hash is read and not its name keeps that download
 * as it was, and only that one: another download the list no longer shows is
 * missed and gone as ever (before, the one entry made the list not whole for
 * good, and nothing became gone again). */
static void
test_unreadable_entry_holds_one(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDownloadClientInfo) info = NULL;
	g_autofree gchar *held = NULL;
	g_autofree gchar *gone = NULL;
	g_autofree gchar *body = NULL;
	AxisDownloadPollReport report;
	gint64 held_id = add_torrent_release(fx, "Some.Film.2020.1080p", &held);
	gint64 gone_id = add_torrent_release(fx, "Another.Film.2019.720p", &gone);
	guint i;

	axis_test_qbit_put(fx->qb, held, "Some.Film.2020.1080p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Some.Film.2020.1080p");
	axis_test_qbit_put(fx->qb, gone, "Another.Film.2019.720p", "axis", "downloading", 1000, 0.4, "/downloads",
		"/downloads/Another.Film.2019.720p");
	poll(fx);
	assert_state(fx, held_id, "downloading");
	assert_state(fx, gone_id, "downloading");
	/* The first's entry loses its name; the second is removed at the client. */
	body = g_strdup_printf("[{\"hash\":\"%s\",\"category\":\"axis\",\"state\":\"downloading\"}]", held);
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", body);
	for (i = 0; i < 5; i++) {
		report = poll(fx);
		g_assert_true(report.reached);
		g_assert_false(report.partial);
		g_assert_cmpuint(report.held, ==, 1);
	}
	assert_state(fx, held_id, "downloading");
	assert_state(fx, gone_id, "gone");
	/* The client's page says which entry, and why. */
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &info, &error));
	g_assert_no_error(error);
	g_assert_nonnull(info->problem);
	g_assert_nonnull(strstr(info->problem, held));
	g_assert_nonnull(strstr(info->problem, "it has no name Axis can read"));
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
}

/* Review of the fixes, item 5: before a removal qBittorrent is asked for the
 * torrent by its hash; an answer holding an entry Axis cannot read (the
 * torrent's own, without its name, or another without a hash) cannot say that
 * the torrent is gone, so nothing is removed and the release is not taken
 * for done (before, the entry was dropped uncounted, the torrent taken for
 * gone, and its files moved from under it). */
static void
test_remove_unreadable_answer(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	g_autofree gchar *any_delete = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	gchar *bodies[3];
	guint i;

	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "stalledUP", 1000, 1.0, "/downloads/axis",
		"/downloads/axis/Some.Film.2020.1080p");
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE download SET state = 'completed', volume_id = %" G_GINT64_FORMAT
			", rel_path = 'downloads/axis/' || release_name, client_path = '/downloads/axis/' || release_name",
			fx->volume_id);

		axis_test_db_exec(fx->t->db, sql);
	}
	any_delete = g_strdup_printf("delete:%s", hash);
	/* Its own entry without its name; another without a hash. */
	bodies[0] = g_strdup_printf("[{\"hash\":\"%s\",\"category\":\"axis\",\"state\":\"stalledUP\"}]", hash);
	bodies[1] = g_strdup_printf("[{\"hash\":\"not a hash, maybe %s\",\"name\":\"Some.Film.2020.1080p\","
		"\"category\":\"axis\"}]", hash);
	bodies[2] = NULL;
	for (i = 0; bodies[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *body = bodies[i];
		gboolean was_gone = TRUE;

		axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", body);
		g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE);
		g_assert_false(was_gone);
		g_assert_cmpuint(axis_test_qbit_count(fx->qb, any_delete), ==, 0);
		g_assert_true(axis_test_qbit_has(fx->qb, hash));
	}
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
	assert_state(fx, id, "completed");
}

/* Spec (downloaders): a removal acts only on a torrent still in Axis's
 * category; one a person moved to another category of theirs is left alone,
 * nothing asked of qBittorrent about it (finding 2 of the stage 2 bug hunt:
 * it was released before a move, its files then moved out from under it, and
 * after an import by hardlink it was removed with its files). */
static void
test_remove_not_ours(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *category = NULL;
	g_autofree gchar *with_data = NULL;
	g_autofree gchar *any_delete = NULL;
	gboolean was_gone = FALSE;
	gboolean found = FALSE;
	gboolean ours = TRUE;
	gint64 id;
	gint64 second;

	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	second = add_torrent_release(fx, "Another.Film.2019.720p", &other);
	/* Both finished; a person moved them to a category of their own. */
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "manual", "stalledUP", 1000, 1.0, "/downloads/manual",
		"/downloads/manual/Some.Film.2020.1080p");
	axis_test_qbit_put(fx->qb, other, "Another.Film.2019.720p", "manual", "stalledUP", 1000, 1.0, "/downloads/manual",
		"/downloads/manual/Another.Film.2019.720p");
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE download SET state = 'completed', volume_id = %" G_GINT64_FORMAT
			", rel_path = 'downloads/manual/' || release_name, client_path = '/downloads/manual/' || release_name",
			fx->volume_id);

		axis_test_db_exec(fx->t->db, sql);
	}

	/* The release before a move (import.take_files move, the default). */
	g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS);
	g_assert_nonnull(strstr(error->message, "\"manual\""));
	g_clear_error(&error);
	category = axis_test_qbit_category_of(fx->qb, hash);
	g_assert_true(axis_test_qbit_has(fx->qb, hash));
	g_assert_cmpstr(category, ==, "manual");
	any_delete = g_strdup_printf("delete:%s", hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, any_delete), ==, 0);

	/* The removal after an import by hardlink, torrents removed. */
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'imported'");
	g_assert_false(axis_downloads_remove_imported(fx->downloads, second, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS);
	g_clear_error(&error);
	with_data = g_strdup_printf("delete:%s:data", other);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, with_data), ==, 0);
	g_assert_true(axis_test_qbit_has(fx->qb, other));

	/* Looked up by its hash in every category: found, not Axis's. */
	g_assert_true(axis_downloads_find_torrent(fx->downloads, fx->client_id, hash, &found, &ours, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(found);
	g_assert_false(ours);

	/* One still in Axis's category is removed as before. */
	axis_test_qbit_put(fx->qb, hash, "Some.Film.2020.1080p", "axis", "stalledUP", 1000, 1.0, "/downloads/axis",
		"/downloads/axis/Some.Film.2020.1080p");
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'completed'");
	g_assert_true(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_no_error(error);
	g_assert_false(axis_test_qbit_has(fx->qb, hash));
}

/* Spec (downloaders): a torrent qBittorrent no longer has, after its import:
 * gone, never a removal that failed, and nothing is deleted at the client. */
static void
test_remove_imported_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *deleted = NULL;
	gint64 id = add_torrent_release(fx, "Lanternfold.Harbour.2016.1080p", &hash);

	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'imported'");
	axis_test_qbit_drop(fx->qb, hash);
	g_assert_false(axis_downloads_remove_imported(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE);
	g_assert_nonnull(strstr(error->message, "no longer lists the torrent"));
	g_clear_error(&error);
	deleted = g_strdup_printf("delete:%s", hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, deleted), ==, 0);
	assert_state(fx, id, "imported");
	/* Its list that cannot be read is not "gone". */
	axis_test_qbit_set_raw(fx->qb, "/torrents/info", 200, "application/json", "{\"not\": \"a list\"}");
	g_assert_false(axis_downloads_remove_imported(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE);
	g_clear_error(&error);
	axis_test_qbit_clear_raw(fx->qb, "/torrents/info");
}

static gchar *
category_folder_of(
	Fixture  *fx,
	GError  **error
){
	gchar *folder = NULL;

	if (!axis_downloads_category_folder(fx->downloads, fx->client_id, &folder, error))
		g_assert_null(folder);
	return folder;
}

static void
assert_category_refused(
	Fixture     *fx,
	const gchar *words
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = category_folder_of(fx, &error);

	g_assert_null(folder);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	if (strstr(error->message, words) == NULL)
		g_error("expected \"%s\" in: %s", words, error->message);
}

/* Spec (downloaders): where qBittorrent keeps Axis's category.  Its own save
 * path, when it is a full path and neither the default save path nor another
 * category's folder is it or lies inside it. */
static void
test_category_folder(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = NULL;

	/* As Axis makes it: no save path of its own. */
	assert_category_refused(fx, "has no save path of its own");
	axis_test_qbit_set_category_path(fx->qb, "axis", "/downloads/axis/");
	folder = category_folder_of(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/axis");
	g_clear_pointer(&folder, g_free);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "default-path"), ==, 1);

	/* The default save path is it, or inside it. */
	axis_test_qbit_set_default_path(fx->qb, "/downloads/axis");
	assert_category_refused(fx, "qBittorrent's default save path");
	axis_test_qbit_set_default_path(fx->qb, "/downloads/axis/incomplete");
	assert_category_refused(fx, "qBittorrent's default save path");
	axis_test_qbit_set_default_path(fx->qb, "/downloads");

	/* Another category's folder is it, or inside it; one with no save path
	 * of its own is the default save path and its name. */
	axis_test_qbit_set_category_path(fx->qb, "manual", "/downloads/axis");
	assert_category_refused(fx, "the category \"manual\" saves to /downloads/axis too");
	axis_test_qbit_set_category_path(fx->qb, "manual", "/downloads/axis/extra");
	assert_category_refused(fx, "or to a folder inside it");
	axis_test_qbit_set_category_path(fx->qb, "manual", "");
	axis_test_qbit_set_default_path(fx->qb, "/media");
	axis_test_qbit_set_category_path(fx->qb, "axis", "/media");
	assert_category_refused(fx, "default save path");
	axis_test_qbit_set_category_path(fx->qb, "axis", "/media/everything");
	axis_test_qbit_set_category_path(fx->qb, "everything", "");
	assert_category_refused(fx, "the category \"everything\" saves to /media/everything too");
	axis_test_qbit_clear_categories(fx->qb);
	axis_test_qbit_set_category_path(fx->qb, "axis", "/media/axis");
	folder = category_folder_of(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/media/axis");
	g_clear_pointer(&folder, g_free);

	/* Not a full path; no such category. */
	axis_test_qbit_set_category_path(fx->qb, "axis", "axis");
	assert_category_refused(fx, "not a full path");
	axis_test_qbit_set_category_path(fx->qb, "axis", "/media/../axis");
	assert_category_refused(fx, "not a full path");
	axis_test_qbit_clear_categories(fx->qb);
	assert_category_refused(fx, "no category called \"axis\"");

	/* A default save path that cannot be read is not a folder of the
	 * category's own either way: it cannot be told. */
	axis_test_qbit_set_category_path(fx->qb, "axis", "/media/axis");
	axis_test_qbit_set_raw(fx->qb, "/app/defaultSavePath", 200, "text/plain", "");
	folder = category_folder_of(fx, &error);
	g_assert_null(folder);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE);
	g_clear_error(&error);
	axis_test_qbit_clear_raw(fx->qb, "/app/defaultSavePath");
}

/* ── The password and the session never leak (3.3) ──────────────── */

/* Task 3.3 */
static void
test_password_and_cookie_go_only_where_they_belong(Fixture *fx, gconstpointer data)
{
	AxisTestQbit *other = axis_test_qbit_new(fx->generation, USER, OTHER_PASSWORD);
	gint64 other_client = add_client_for(fx, other, "Other", OTHER_PASSWORD);
	g_autofree gchar *base = axis_test_qbit_base(fx->qb);
	g_autofree gchar *other_base = axis_test_qbit_base(other);
	g_autofree gchar *cookie_name = axis_test_qbit_cookie_name(fx->qb);
	g_autoptr(GPtrArray) requests = NULL;
	g_autoptr(GPtrArray) other_requests = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(GPtrArray) other_sessions = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *other_transcript = NULL;
	g_autofree gchar *hash = NULL;
	AxisDownloadTestResult result;
	gint64 wrong;
	gint64 id;
	guint logins = 0;
	guint i;

	/* A whole life: test, add, poll, remove, a session that ends, a wrong password; and another client. */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	poll(fx);
	axis_test_qbit_expire_session(fx->qb);
	poll(fx);
	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	wrong = add_client_for(fx, fx->qb, "Wrong", WRONG_PASSWORD);
	result = test_client(fx, wrong);
	axis_download_test_result_clear(&result);
	poll_client(fx, wrong);
	result = test_client(fx, other_client);
	axis_download_test_result_clear(&result);
	poll_client(fx, other_client);

	requests = axis_test_plex_requests(axis_test_qbit_server(fx->qb));
	sessions = axis_test_qbit_sessions(fx->qb);
	other_sessions = axis_test_qbit_sessions(other);
	g_assert_cmpuint(requests->len, >=, 14);
	for (i = 0; i < requests->len; i++) {
		const gchar *one = g_ptr_array_index(requests, i);
		gsize line = strcspn(one, "\n");
		const gchar *blank = strstr(one, "\n\n");
		const gchar *body = blank != NULL ? blank : "";
		g_autofree gchar *head = g_strndup(one, blank != NULL ? (gsize)(blank - one) : strlen(one));
		g_autofree gchar *request_line = g_strndup(one, line);
		g_autofree gchar *referer = g_strdup_printf("Referer: %s", base);
		g_autofree gchar *origin = g_strdup_printf("Origin: %s", base);
		gboolean login = strstr(request_line, "POST /api/v2/auth/login") != NULL;
		guint s;

		/* Always under /api/v2, and the client's own address as its Referer and its Origin. */
		g_assert_nonnull(strstr(request_line, " /api/v2/"));
		g_assert_nonnull(strstr(head, referer));
		g_assert_nonnull(strstr(head, origin));
		/* No password in any line or header, in any form, and in no body but a login's, and then encoded. */
		g_assert_null(strstr(head, PASSWORD));
		g_assert_null(strstr(head, PASSWORD_ENCODED));
		g_assert_null(strstr(head, WRONG_PASSWORD));
		g_assert_null(strstr(one, OTHER_PASSWORD));
		g_assert_null(strstr(one, PASSWORD));
		if (login) {
			logins++;
			g_assert_nonnull(blank);
			g_assert_true(strstr(body, PASSWORD_ENCODED) != NULL || strstr(body, WRONG_PASSWORD) != NULL);
			g_assert_nonnull(strstr(head, "Content-Type: application/x-www-form-urlencoded"));
			/* A login carries no cookie: it is made on a clean slate. */
			g_assert_null(strstr(head, cookie_name));
		} else {
			g_assert_null(strstr(body, PASSWORD_ENCODED));
			g_assert_null(strstr(body, WRONG_PASSWORD));
			g_assert_null(strstr(body, cookie_name));
		}
		/* No session id in the request line or the body, ever; in a header only as the cookie. */
		for (s = 0; s < sessions->len; s++) {
			g_assert_null(strstr(request_line, g_ptr_array_index(sessions, s)));
			g_assert_null(strstr(body, g_ptr_array_index(sessions, s)));
		}
	}
	g_assert_cmpuint(logins, >=, 3);
	g_assert_cmpuint(axis_test_qbit_bad_requests(fx->qb), ==, 0);
	g_assert_true(axis_test_plex_saw_non_get(axis_test_qbit_server(fx->qb)));

	/* The other client's server saw its own password and its own session, and none of the first's. */
	transcript = axis_test_plex_transcript(axis_test_qbit_server(fx->qb));
	other_transcript = axis_test_plex_transcript(axis_test_qbit_server(other));
	other_requests = axis_test_plex_requests(axis_test_qbit_server(other));
	g_assert_cmpuint(other_requests->len, >=, 3);
	g_assert_nonnull(strstr(other_transcript, OTHER_PASSWORD));
	g_assert_null(strstr(other_transcript, PASSWORD_ENCODED));
	g_assert_null(strstr(other_transcript, WRONG_PASSWORD));
	g_assert_null(strstr(transcript, OTHER_PASSWORD));
	g_assert_null(strstr(other_transcript, base));
	g_assert_null(strstr(transcript, other_base));
	g_assert_cmpuint(other_sessions->len, >=, 1);
	for (i = 0; i < sessions->len; i++)
		g_assert_null(strstr(other_transcript, g_ptr_array_index(sessions, i)));
	for (i = 0; i < other_sessions->len; i++)
		g_assert_null(strstr(transcript, g_ptr_array_index(other_sessions, i)));

	/* Nowhere else: not in any table but its own, not in a job row, not in any log line, not in what a client shows. */
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE strpos(to_jsonb(c)::text, '" PASSWORD "') > 0");
	assert_q(fx, "0", "SELECT count(*) FROM download d WHERE strpos(to_jsonb(d)::text, '" PASSWORD "') > 0");
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE strpos(to_jsonb(c)::text, '" WRONG_PASSWORD "') > 0");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE strpos(to_jsonb(j)::text, '" PASSWORD "') > 0");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE strpos(to_jsonb(j)::text, '" WRONG_PASSWORD "') > 0");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE strpos(to_jsonb(j)::text, '" OTHER_PASSWORD "') > 0");
	assert_q(fx, "3", "SELECT count(*) FROM download_client_secret WHERE secret IN ('" PASSWORD "', '" WRONG_PASSWORD "', '"
		OTHER_PASSWORD "')");
	/* The user name is the client's own, and on its page. */
	assert_q(fx, "3", "SELECT count(*) FROM download_client WHERE username = '" USER "'");
	g_assert_cmpuint(logged("*" PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" PASSWORD_ENCODED "*"), ==, 0);
	g_assert_cmpuint(logged("*" WRONG_PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" OTHER_PASSWORD "*"), ==, 0);
	for (i = 0; i < sessions->len + other_sessions->len; i++) {
		const gchar *session = i < sessions->len ? g_ptr_array_index(sessions, i) :
			g_ptr_array_index(other_sessions, i - sessions->len);
		g_autofree gchar *pattern = g_strdup_printf("*%s*", session);
		g_autofree gchar *sql_client = g_strdup_printf("SELECT count(*) FROM download_client c WHERE strpos(to_jsonb(c)::text, '%s') > 0",
			session);
		g_autofree gchar *sql_download = g_strdup_printf("SELECT count(*) FROM download d WHERE strpos(to_jsonb(d)::text, '%s') > 0",
			session);
		g_autofree gchar *sql_job = g_strdup_printf("SELECT count(*) FROM job j WHERE strpos(to_jsonb(j)::text, '%s') > 0", session);

		g_assert_cmpuint(logged(pattern), ==, 0);
		assert_q(fx, "0", sql_client);
		assert_q(fx, "0", sql_download);
		assert_q(fx, "0", sql_job);
	}
	g_assert_true(axis_downloads_get_client(fx->downloads, wrong, &client, &error));
	g_assert_null(strstr(client->state_message, WRONG_PASSWORD));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	axis_test_qbit_free(other);
}

/* A redirect is never followed, and neither the password nor the cookie goes along. */
static void
test_redirect_is_not_followed(Fixture *fx, gconstpointer data)
{
	AxisTestQbit *other = axis_test_qbit_new(fx->generation, USER, PASSWORD);
	g_autofree gchar *other_base = axis_test_qbit_base(other);
	g_autofree gchar *elsewhere = g_strconcat(other_base, "/api/v2/auth/login", NULL);
	g_autofree gchar *transcript = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	AxisDownloadTestResult result;
	gint64 id;

	/* A session first, so that there is a cookie to leak. */
	g_assert_true(poll(fx).reached);
	axis_test_qbit_set_redirect(fx->qb, elsewhere);
	axis_test_qbit_set_mode(fx->qb, AXIS_TEST_QBIT_REDIRECT);
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_qbit_server(other)), ==, 0);
	transcript = axis_test_plex_transcript(axis_test_qbit_server(other));
	g_assert_null(strstr(transcript, PASSWORD_ENCODED));
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_nonnull(strstr(client->state_message, "redirect"));
	axis_test_qbit_free(other);
}

/* A client served under a path is asked at that path, cookie and all */
static void
test_requests_carry_the_base_path(Fixture *fx, gconstpointer data)
{
	AxisTestQbit *served = axis_test_qbit_new(fx->generation, USER, PASSWORD);
	g_autofree gchar *base = axis_test_qbit_base(served);
	gint64 id = add_client_full(fx, "Under a path", base, "/qbittorrent", USER, PASSWORD, NULL);
	AxisDownloadTestResult result;
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", &hash);
	AxisDownloadRequest request;
	g_autoptr(GError) error = NULL;
	gint64 download = 0;

	axis_test_qbit_set_prefix(served, "/qbittorrent");
	result = test_client(fx, id);
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	report = poll_client(fx, id);
	g_assert_true(report.reached);
	memset(&request, 0, sizeof(request));
	request.client_id = id;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_true(axis_downloads_add(fx->downloads, &request, &download, &error));
	/* Its Referer and Origin are its own address, without the path: what a browser sends. */
	g_assert_cmpuint(axis_test_qbit_bad_requests(served), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(axis_test_qbit_server(served), "/qbittorrent/api/v2/auth/login"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(axis_test_qbit_server(served), "/api/v2/auth/login"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_count(served, "unauthorised"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_count(served, "login"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_count(served, "add"), ==, 1);
	axis_test_qbit_free(served);
}

/* ── Sent once (3.7) ────────────────────────────────────────────── */

/* A lost answer to the add: it was sent once and is never sent again.  The torrent
 * has Axis's category already, so unlike Deluge's the list finds it, and it is adopted by its hash. */
static void
test_lost_add_answer_is_never_sent_again(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_HANGS, TRUE);
	id = add_torrent_release_at(fx, "Some.Series.S01E01.720p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN);
	assert_state(fx, id, "sending");
	axis_test_qbit_set_flag(fx->qb, AXIS_TEST_QBIT_ADD_HANGS, FALSE);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_true(axis_test_qbit_has(fx->qb, hash));

	/* Within the wait the record is left alone; the torrent is not sent again by anything. */
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 0);
	assert_state(fx, id, "sending");
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 1);
}

static void
test_crash_before_sending_is_never_sent(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_RECORD);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	poll(fx);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 0);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 0);
}

/* Spec (core): a release handed over and the answer lost is matched by info-hash, never sent again */
static void
test_crash_after_sending_is_adopted_by_hash(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_SEND);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, hash);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 1);
	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_NONE);
}

/* The same torrent twice is refused before it reaches qBittorrent */
static void
test_the_same_torrent_twice_is_sent_once(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;

	add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
	g_assert_cmpuint(axis_test_qbit_count(fx->qb, "add"), ==, 1);
	g_assert_cmpuint(axis_test_qbit_n_torrents(fx->qb), ==, 1);
}

/* An unreachable client at send time: nothing went out, so failed, and never sent. */
static void
test_unreachable_client_at_send_time(Fixture *fx, gconstpointer data)
{
	AxisTestQbit *closed = axis_test_qbit_new(fx->generation, USER, PASSWORD);
	g_autofree gchar *closed_base = axis_test_qbit_base(closed);
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 client;
	gint64 id = 0;

	axis_test_qbit_free(closed);
	client = add_client_full(fx, "Down", closed_base, NULL, USER, PASSWORD, NULL);
	memset(&request, 0, sizeof(request));
	request.client_id = client;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE);
	assert_state(fx, id, "failed");
}

#define ADD(path, func) \
	AXIS_TEST_ADD_DB_CASE("/download/qbittorrent/v4/" path, Fixture, GINT_TO_POINTER(AXIS_TEST_QBIT_V4), func); \
	AXIS_TEST_ADD_DB_CASE("/download/qbittorrent/v5/" path, Fixture, GINT_TO_POINTER(AXIS_TEST_QBIT_V5), func)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/download/qbittorrent/parse/text", test_parse_text);
	g_test_add_func("/download/qbittorrent/parse/version", test_parse_version);
	g_test_add_func("/download/qbittorrent/parse/categories", test_parse_categories);
	g_test_add_func("/download/qbittorrent/parse/states", test_state_map);
	g_test_add_func("/download/qbittorrent/parse/torrents", test_parse_torrents);
	g_test_add_func("/download/qbittorrent/parse/broken", test_parse_broken_answers);
	g_test_add_func("/download/qbittorrent/parse/capped", test_parse_capped);
	ADD("client/test", test_client_test);
	ADD("client/test-makes-the-category", test_client_test_makes_the_category);
	ADD("client/wrong-password", test_client_wrong_password);
	ADD("client/banned", test_client_banned);
	ADD("client/banned-while-running", test_client_banned_while_running);
	ADD("client/not-qbittorrent", test_client_not_qbittorrent);
	ADD("client/user-name", test_user_name);
	ADD("add/torrent-file", test_add_torrent_file);
	ADD("add/magnet", test_add_magnet);
	ADD("add/category-first", test_add_makes_the_category_first);
	ADD("add/already-there", test_add_already_there);
	ADD("add/fails", test_add_refused_by_qbittorrent);
	ADD("add/ok-but-not-shown", test_add_ok_but_not_shown);
	ADD("add/another-category", test_add_in_another_category);
	ADD("add/refuses-usenet", test_add_refuses_usenet);
	ADD("add/wrong-password", test_add_with_a_wrong_password);
	ADD("list/other-categories", test_other_categories_are_never_touched);
	ADD("list/fixtures", test_fixture_answers_through_the_layer);
	ADD("list/unreadable", test_unreadable_lists_change_nothing);
	ADD("list/unreadable-entries", test_unreadable_entries_are_counted);
	ADD("track/seeding-stays-completed", test_seeding_stays_completed);
	ADD("track/paused-and-stopped", test_paused_and_stopped);
	ADD("track/unknown-state", test_unknown_state_keeps_the_last_state);
	ADD("track/unknown-state-never-completes", test_unknown_state_never_completes);
	ADD("track/client-down", test_down_client_fails_nothing);
	ADD("session/one-login", test_one_login_per_connection);
	ADD("session/list-recovers", test_list_recovers_from_a_lost_session);
	ADD("session/add-not-sent", test_add_with_a_lost_session_is_not_sent);
	ADD("session/add-after-idle", test_add_after_an_idle_session);
	ADD("remove/data", test_remove_with_and_without_data);
	ADD("remove/after-import", test_remove_after_import_honours_the_setting);
	ADD("remove/refusals", test_remove_refusals);
	ADD("remove/imported-gone", test_remove_imported_gone);
	ADD("remove/not-ours", test_remove_not_ours);
	ADD("remove/unreadable-answer", test_remove_unreadable_answer);
	ADD("list/everything", test_everything_listed);
	ADD("track/unreadable-entry-holds-one", test_unreadable_entry_holds_one);
	ADD("folder/category", test_category_folder);
	ADD("secret/password-and-cookie", test_password_and_cookie_go_only_where_they_belong);
	ADD("secret/redirect", test_redirect_is_not_followed);
	ADD("base-path/requests", test_requests_carry_the_base_path);
	ADD("once/lost-answer", test_lost_add_answer_is_never_sent_again);
	ADD("once/crash-before-send", test_crash_before_sending_is_never_sent);
	ADD("once/crash-after-send", test_crash_after_sending_is_adopted_by_hash);
	ADD("once/twice", test_the_same_torrent_twice_is_sent_once);
	ADD("once/unreachable", test_unreachable_client_at_send_time);
	return g_test_run();
}
