/*
 * axis-test-meta-fixture-parse.h - test support: the fixture provider's
 * answer format, read into the metadata model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The fixture provider (axis-test-meta-fixture.h) is a provider for tests
 * only: it speaks a small JSON format of its own to a fake server that
 * answers from tests/fixtures/meta.  It is never registered in a normal
 * build (it is not part of the daemon at all), but its parser is written
 * the way a real provider's must be, and is fuzzed
 * (tests/fuzz/fuzz-meta-fixture.c): every field is optional and an
 * unreadable one is left out; an entry without a title or the provider's
 * own id is skipped; a document that is not the expected shape is
 * %AXIS_META_ERROR_UNREADABLE.  What it returns passes
 * axis_meta_title_check() and axis_meta_season_check().
 *
 *   results  {"results": [ title, ... ]}
 *   title    {"id": "101", "kind": "film", "title": "...", "original_title",
 *            "year", "overview", "runtime", "status", "genres": [...],
 *            "language", "release_date", "first_air_date", "last_air_date",
 *            "poster", "backdrop", "ids": {"imdb": "tt...", "tvdb": "..."},
 *            "alt_titles": [{"title", "country", "language"}],
 *            "seasons": [{"number", "name", "air_date", "overview",
 *            "episodes": 10}]}
 *   season   {"number": 1, "name", "air_date", "overview", "episodes":
 *            [{"number", "absolute", "name", "air_date", "overview",
 *            "runtime"}]}
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h).
 */

#ifndef AXIS_TEST_META_FIXTURE_PARSE_H
#define AXIS_TEST_META_FIXTURE_PARSE_H

#include <glib.h>

#include "meta/axis-meta-model.h"

G_BEGIN_DECLS

/* Titles of @kind (entries of another kind are skipped), as search
 * results: brief records. */
gboolean
axis_test_meta_fixture_parse_results(
	const gchar   *provider,
	AxisMetaKind   kind,
	const gchar   *data,
	gsize          length,
	GPtrArray    **titles,
	GError       **error
);

/* A title's full record; it must be of @kind. */
gboolean
axis_test_meta_fixture_parse_title(
	const gchar    *provider,
	AxisMetaKind    kind,
	const gchar    *data,
	gsize           length,
	AxisMetaTitle **title,
	GError        **error
);

/* Season @number with its episodes. */
gboolean
axis_test_meta_fixture_parse_season(
	gint             number,
	const gchar     *data,
	gsize            length,
	AxisMetaSeason **season,
	GError         **error
);

G_END_DECLS

#endif /* AXIS_TEST_META_FIXTURE_PARSE_H */
