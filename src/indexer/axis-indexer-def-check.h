/*
 * axis-indexer-def-check.h - axisd --check-definition: a definition run
 * against a saved page or its live site, step by step, with no database and
 * no daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * For a person repairing a definition (docs/CONTRIBUTING-definitions.md).
 * The definition is a file, or a built-in one by its id
 * (indexer/axis-indexer-shipped.h).  It is read and judged as the daemon
 * judges an uploaded one; one that is not usable says what it needs.  Then:
 *
 *  - with a saved page: the page is read as the answer of the search's first
 *    request, and nothing is asked of any site;
 *  - else, the live site: the search's requests are made to the address
 *    given (or the definition's first), at most one every two seconds (or
 *    its requestDelay), through the HTTP client the daemon uses (the same
 *    origin only, no redirect elsewhere, bounded answers and deadlines, the
 *    outbound binding the configuration sets), no cookie and no key; with a
 *    folder to save to, each answer is written there as it came, for a
 *    fixture;
 *  - with a saved release page: the download is read from it as at grab
 *    time (a magnet link, an info-hash, or a path on the site), nothing
 *    fetched.
 *
 * The account (indexer/def/axis-def-trace.h) is written to @out as plain
 * text.  Nothing is stored, and nothing read is followed.
 */

#ifndef AXIS_INDEXER_DEF_CHECK_H
#define AXIS_INDEXER_DEF_CHECK_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AxisDefCheckOptions:
 * @definition: a definition file, or a built-in definition's id
 * @page: (nullable): a saved answer to read instead of asking the site
 * @release_page: (nullable): a saved release page to read the download from
 * @words: (nullable): what to search for; none is the definition's test
 * @address: (nullable): the site's address; none is the definition's first
 * @excerpts: record excerpts of the rows (and of a page no row was read from)
 * @save_dir: (nullable): a folder the live answers are written to
 */
typedef struct
{
	const gchar *definition;
	const gchar *page;
	const gchar *release_page;
	const gchar *words;
	const gchar *address;
	gboolean     excerpts;
	const gchar *save_dir;
} AxisDefCheckOptions;

/**
 * axis_indexer_def_check_run:
 * @out: where the account is written
 *
 * Returns: the exit status: 0 when every answer was read (with results or
 *   none) and a release page gave its download, 1 when the definition is not
 *   usable or an answer was refused, drifted or a challenge, 2 when a file
 *   or the address could not be used
 */
gint
axis_indexer_def_check_run(
	const AxisDefCheckOptions *options,
	GString                   *out
);

G_END_DECLS

#endif /* AXIS_INDEXER_DEF_CHECK_H */
