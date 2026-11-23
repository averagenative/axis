/*
 * fuzz-library-filter.c - libFuzzer target for the file list's query parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a query string.  Whatever it is, the parser must not crash
 * or read past it, and:
 *
 *  - a query it accepts is within the bounds (length, pairs, name length)
 *    and holds only values of the types the header lists;
 *  - the canonical query of an accepted filter parses again, to a filter
 *    with the same canonical query (one spelling per filter), without the
 *    cursor when it is left out;
 *  - a query it refuses leaves the filter empty, with a fixed message.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "library/axis-library-filter.h"
#include "transcode/axis-estimate-target.h"
#include "transcode/axis-profile.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_empty(const AxisLibraryFilter *filter)
{
	if (filter->root_id != 0 || filter->kind != NULL || filter->container != NULL ||
	    filter->video_codec != NULL || filter->resolution != NULL || filter->hdr != NULL ||
	    filter->audio_codec != NULL || filter->pinned != -1 || filter->probe != NULL ||
	    filter->name != NULL || filter->min_mib != -1 || filter->max_mib != -1 || filter->after != 0 ||
	    filter->since != 0 || filter->changed != -1 || filter->volume_id != 0 || filter->type != NULL ||
	    filter->dir_id != 0 || filter->ids != NULL || filter->n_ids != 0 || filter->min_bpp != -1 ||
	    filter->max_bpp != -1 || filter->leftover != NULL || filter->target != NULL || filter->revision != 0 ||
	    filter->outcome != NULL)
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *query = g_strndup((const gchar *)data, size);
	AxisLibraryFilter filter;
	AxisLibraryFilter again;
	GError *error = NULL;
	gchar *canonical;
	gchar *second;
	gchar *with_cursor;

	/* A NUL ends the C string: still a query, a shorter one. */
	if (!axis_library_filter_parse(query, &filter, &error)) {
		if (error == NULL || error->domain != AXIS_LIBRARY_FILTER_ERROR)
			abort();
		check_empty(&filter);
		g_error_free(error);
		g_free(query);
		return 0;
	}
	if (error != NULL)
		abort();

	if (strlen(query) > AXIS_LIBRARY_FILTER_MAX_QUERY)
		abort();
	if (filter.name != NULL && (!g_utf8_validate(filter.name, -1, NULL) ||
	                            g_utf8_strlen(filter.name, -1) > AXIS_LIBRARY_FILTER_NAME_CHARS ||
	                            g_utf8_strlen(filter.name, -1) < AXIS_LIBRARY_FILTER_NAME_MIN_CHARS))
		abort();
	if (filter.container != NULL && strlen(filter.container) > AXIS_LIBRARY_FILTER_TOKEN_MAX)
		abort();
	if (filter.min_mib >= 0 && filter.max_mib >= 0 && filter.min_mib > filter.max_mib)
		abort();
	if (filter.state == NULL)
		abort();
	if (filter.min_bpp >= 0 && filter.max_bpp >= 0 && filter.min_bpp >= filter.max_bpp)
		abort();
	/* An outcome and a revision only with a target; the target a profile
	 * slug (whether there is such a profile is the estimator's to say), the
	 * outcome from the fixed list. */
	if ((filter.outcome != NULL || filter.revision != 0) && filter.target == NULL)
		abort();
	if (filter.target != NULL && !axis_profile_slug_valid(filter.target))
		abort();
	if (filter.revision < 0 || filter.revision > AXIS_LIBRARY_FILTER_MAX_REV)
		abort();
	if (filter.outcome != NULL && !g_strv_contains(axis_library_filter_outcomes(), filter.outcome))
		abort();
	if (axis_library_filter_needs_estimator(&filter) != (filter.target != NULL))
		abort();
	/* The file ids: a few, all different, in ascending order. */
	if ((filter.n_ids == 0) != (filter.ids == NULL) || filter.n_ids > AXIS_LIBRARY_FILTER_MAX_IDS)
		abort();
	{
		guint i;

		for (i = 0; i < filter.n_ids; i++) {
			if (filter.ids[i] < 1 || (i > 0 && filter.ids[i - 1] >= filter.ids[i]))
				abort();
		}
	}

	canonical = axis_library_filter_to_query(&filter, FALSE);
	with_cursor = axis_library_filter_to_query(&filter, TRUE);
	if (!axis_library_filter_parse(with_cursor, &again, &error))
		abort();
	second = axis_library_filter_to_query(&again, TRUE);
	if (strcmp(second, with_cursor) != 0)
		abort();
	g_free(second);
	axis_library_filter_clear(&again);

	if (!axis_library_filter_parse(canonical, &again, &error))
		abort();
	second = axis_library_filter_to_query(&again, FALSE);
	if (strcmp(second, canonical) != 0)
		abort();
	if (again.after != 0 || again.since != 0)
		abort();
	g_free(second);
	axis_library_filter_clear(&again);

	g_free(canonical);
	g_free(with_cursor);
	axis_library_filter_clear(&filter);
	g_free(query);
	return 0;
}
