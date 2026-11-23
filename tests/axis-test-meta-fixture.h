/*
 * axis-test-meta-fixture.h - test support: the "fixture" metadata provider
 * and the fake servers it talks to
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A provider for tests only.  It is not part of the daemon (it lives here,
 * among the tests' support code), so it is never registered in a normal
 * build.  It speaks the format of axis-test-meta-fixture-parse.h to its API
 * origin, a fake server on loopback (axis-test-plex.h) answering from the
 * files of tests/fixtures/meta, and asks its image origin, a second fake
 * server, for artwork:
 *
 *   GET /search/<film|series>?q=<text>[&year=<n>]
 *   GET /title/<film|series>/<id>
 *   GET /title/series/<id>/season/<n>
 *   GET /find/<film|series>?scheme=<s>&value=<v>
 *   GET /img/<w185|w500|w1280><ref>            (image origin)
 *
 * Its key goes in X-Fixture-Key.
 */

#ifndef AXIS_TEST_META_FIXTURE_H
#define AXIS_TEST_META_FIXTURE_H

#include <glib.h>

#include "axis-test-plex.h"
#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_TEST_META_KEY_HEADER "X-Fixture-Key"
#define AXIS_TEST_META_KEY        "fixture-key-5b1c0e7d9a2f4486"

typedef struct
{
	AxisMetaProviderInfo  info;	/* first: a pointer to this is the provider */
	AxisTestPlex         *api;
	AxisTestPlex         *images;
	gchar                *name;
	gchar                *label;
	gchar                *api_origin;
	gchar                *image_origin;
} AxisTestMetaFixture;

/**
 * axis_test_meta_fixture_new:
 * @name: the provider's name ("fixture", "fixtureb")
 * @rate: its default rate (requests a second)
 * @burst: its burst
 *
 * Starts the two fake servers and makes the provider.  Its info must
 * outlive the metadata layer it is added to.
 */
AxisTestMetaFixture *
axis_test_meta_fixture_new(
	const gchar *name,
	gdouble      rate,
	guint        burst
);

void
axis_test_meta_fixture_free(AxisTestMetaFixture *self);

/* The standard answers of tests/fixtures/meta on the API server, and two
 * posters and a backdrop (real PNG images) on the image server. */
void
axis_test_meta_fixture_standard(AxisTestMetaFixture *self);

/* A real PNG image, for answers (its bytes). */
GBytes *
axis_test_meta_png(void);

/* A JPEG header of @width by @height (not a whole image: enough for the
 * sniffer), padded to @length bytes. */
GBytes *
axis_test_meta_jpeg(
	guint width,
	guint height,
	gsize length
);

/* A WebP (VP8L) header of @width by @height, padded to a whole RIFF of
 * @length bytes. */
GBytes *
axis_test_meta_webp(
	guint width,
	guint height,
	gsize length
);

G_END_DECLS

#endif /* AXIS_TEST_META_FIXTURE_H */
