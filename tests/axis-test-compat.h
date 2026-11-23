/*
 * axis-test-compat.h - test support for client compatibility: a database
 * with the library of axis-test-savings.h, a compat object connected to a
 * fake media server on loopback, and helpers that write sessions and
 * evidence straight into the tables
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The disk "Disk A" of the savings library holds the roots Films (movies),
 * Shows (tv), Music (music), Books (audiobooks) and Downloads; the fake
 * media server's libraries (tests/fixtures/compat/sections.json) are mapped
 * to them by axis_test_compat_map_standard():
 *
 *   /data/movies  -> <disk A>/Films      (library "1", movies)
 *   /data/tv      -> <disk A>/Shows      (library "2", shows; its second
 *                                         folder /data/tv-extra is not mapped
 *                                         until a test maps it)
 *   /data/music   -> <disk A>/Music      (library "3", music)
 */

#ifndef AXIS_TEST_COMPAT_H
#define AXIS_TEST_COMPAT_H

#include <glib.h>

#include "axis-test-library.h"
#include "axis-test-plex.h"
#include "axis-test-savings.h"
#include "compat/axis-compat.h"
#include "compat/axis-compat-report.h"

G_BEGIN_DECLS

/* A token no page, log line, job or message may ever show. */
#define AXIS_TEST_COMPAT_TOKEN "tok-7f3a9c2e1b5d4a60c1"

/* The same for the Jellyfin connection: another secret, so that a test can
 * tell whose reached whom. */
#define AXIS_TEST_COMPAT_JELLYFIN_KEY "jf-key-5d2e8a1c9b7f4036ae"

typedef struct
{
	AxisTestLibrary  *lib;
	AxisTestSavings   s;
	gboolean          has_library;
	AxisSettings     *settings;
	AxisEstimator    *estimator;
	AxisCompat       *compat;
	AxisTestPlex     *plex;
	AxisTestPlex     *jellyfin;	/* NULL until axis_test_compat_enable_jellyfin() */
} AxisTestCompat;

/* ── A second server: Jellyfin ──────────────────────────────────── */

/* The answers of the real server recorded in tests/fixtures/compat/jellyfin:
 * its identity, two libraries, eight devices and seven sessions playing. */
void
axis_test_jellyfin_standard(AxisTestPlex *server);

/* Starts a fake Jellyfin server with the standard answers. */
void
axis_test_compat_enable_jellyfin(AxisTestCompat *t);

/* Sets the Jellyfin address (the fake server) and key. */
void
axis_test_compat_connect_jellyfin(AxisTestCompat *t);

/* Maps the Jellyfin libraries' folders: /media/films -> <disk A>/Films and
 * /media/music -> <disk A>/Music. */
void
axis_test_compat_map_jellyfin_standard(AxisTestCompat *t);

/* The key of the standard Jellyfin libraries. */
#define AXIS_TEST_JELLYFIN_FILMS "db4c1708cbb5dd1676284a40f2950aba"
#define AXIS_TEST_JELLYFIN_MUSIC "7e64e319657a9516ec78490da03edccb"

/**
 * AxisTestEvidence:
 *
 * One observed session of a device with one stream's evidence, written
 * straight into the tables, for the kind of server it is of (%NULL: Plex).
 */
typedef struct
{
	const gchar *server_kind;
	const gchar *device;
	const gchar *library_key;
	gint         kind;
	const gchar *codec;
	const gchar *profile;
	gint         bit_depth;
	const gchar *res_class;
	gint         hdr;
	const gchar *container;
	const gchar *effect;
	const gchar *property;		/* "container", ... or %NULL */
	const gchar *stated_reason;	/* or %NULL */
	gboolean     remote;
	guint        age_days;
} AxisTestEvidence;

void
axis_test_compat_add_evidence(
	AxisTestCompat         *t,
	const AxisTestEvidence *e
);

/* A device of a kind of server (axis_test_compat_device() makes Plex's). */
void
axis_test_compat_device_of(
	AxisTestCompat *t,
	const gchar    *server_kind,
	const gchar    *client_id,
	const gchar    *name
);

/* A play of @library_key of a kind of server by @device, @age_days ago. */
void
axis_test_compat_play_of(
	AxisTestCompat *t,
	const gchar    *server_kind,
	const gchar    *device,
	const gchar    *library_key,
	guint           age_days
);

/**
 * axis_test_compat_new:
 * @with_library: build the savings library (scanned and probed; seconds)
 *
 * Returns: %NULL when database tests are skipped
 */
AxisTestCompat *
axis_test_compat_new(gboolean with_library);

void
axis_test_compat_free(AxisTestCompat *t);

/* Sets the address (the fake server) and the token. */
void
axis_test_compat_connect(AxisTestCompat *t);

/* Maps the standard folders (see above). */
void
axis_test_compat_map_standard(AxisTestCompat *t);

void
axis_test_compat_map(
	AxisTestCompat *t,
	const gchar    *server_path,
	const gchar    *axis_path
);

/* The index id of a file of a root, by its path in the root. */
gint64
axis_test_compat_file_id(
	AxisTestCompat *t,
	gint64          root_id,
	const gchar    *rel
);

/* A fixture file with names of the library substituted: the observed
 * session's film is "Big (2001)" of the Films root. */
gchar *
axis_test_compat_fixture(
	const gchar *name,
	const gchar *from,
	const gchar *to
);

/* ── Rows written straight into the tables ──────────────────────── */

void
axis_test_compat_device(
	AxisTestCompat *t,
	const gchar    *client_id,
	const gchar    *name
);

/* A play of @library_key by @device, @age_days ago. */
void
axis_test_compat_play(
	AxisTestCompat *t,
	const gchar    *device,
	const gchar    *library_key,
	guint           age_days
);

/**
 * axis_test_compat_evidence:
 * @effect: "direct", "passthrough", "converted_codec", ...
 * @remote: the session was not local
 *
 * One observed session of @device with one stream's evidence (kind 1 video,
 * 2 audio), @age_days ago.  Creates the device if it is new.
 */
void
axis_test_compat_evidence(
	AxisTestCompat *t,
	const gchar    *device,
	const gchar    *library_key,
	gint            kind,
	const gchar    *codec,
	const gchar    *profile,
	gint            bit_depth,
	const gchar    *res_class,
	gint            hdr,
	const gchar    *container,
	const gchar    *effect,
	gboolean        remote,
	guint           age_days
);

/* The Savings snapshot's figure for a target and a root (what the
 * recommendation reads): @bytes now and @mid after. */
void
axis_test_compat_fact(
	AxisTestCompat *t,
	const gchar    *target,
	gint64          root_id,
	gint64          bytes,
	gint64          mid
);

/* A count of rows: SELECT count(*) FROM @table WHERE @where. */
gint64
axis_test_compat_count(
	AxisTestCompat *t,
	const gchar    *table,
	const gchar    *where
);

/* The report; fails the test if it cannot be computed. */
AxisCompatReport *
axis_test_compat_report(AxisTestCompat *t);

/* The library report of the report with this key. */
const AxisCompatLibraryReport *
axis_test_compat_library(
	AxisCompatReport *report,
	const gchar      *key
);

/* The verdict of a target in a library report, or %AXIS_VERDICT_UNKNOWN
 * with *found = FALSE when the target does not apply. */
const AxisCompatTargetVerdict *
axis_test_compat_target(
	const AxisCompatLibraryReport *library,
	const gchar                   *target_id
);

G_END_DECLS

#endif /* AXIS_TEST_COMPAT_H */
