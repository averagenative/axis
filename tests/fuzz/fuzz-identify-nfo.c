/*
 * fuzz-identify-nfo.c - libFuzzer target for the scanner of sidecar .nfo
 * bytes (identify/axis-identify-ids.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Any bytes: nothing crashes; what is found is a well-formed identifier
 * ("tt" and 7 to 9 digits; 1 to 12 digits not starting with 0) that is in
 * the bytes looked at; a conflict leaves its scheme empty; only the first
 * AXIS_IDENTIFY_NFO_MAX bytes count (the same bytes cut there find the
 * same); and scanning twice finds the same.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "identify/axis-identify-ids.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gboolean
digits(
	const gchar *p,
	gsize        min,
	gsize        max
){
	gsize n = strlen(p);
	gsize i;

	if (n < min || n > max)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return FALSE;
	}
	return TRUE;
}

/* Whether @needle (ASCII) is in @data, ignoring case. */
static gboolean
inside(
	const uint8_t *data,
	gsize          size,
	const gchar   *needle
){
	gsize n = strlen(needle);
	gsize i;
	gsize k;

	for (i = 0; n > 0 && i + n <= size; i++) {
		for (k = 0; k < n; k++) {
			if (g_ascii_tolower((gchar)data[i + k]) != g_ascii_tolower(needle[k]))
				break;
		}
		if (k == n)
			return TRUE;
	}
	return FALSE;
}

static void
check(
	const AxisIdentifyIds *ids,
	const uint8_t         *data,
	gsize                  size
){
	if (ids->tmdb[0] != '\0' && (!digits(ids->tmdb, 1, 12) || ids->tmdb[0] == '0' || !inside(data, size, ids->tmdb)))
		abort();
	if (ids->imdb[0] != '\0' && (strncmp(ids->imdb, "tt", 2) != 0 || !digits(ids->imdb + 2, 7, 9) ||
	    !inside(data, size, ids->imdb)))
		abort();
	if (ids->tmdb_conflict && ids->tmdb[0] != '\0')
		abort();
	if (ids->imdb_conflict && ids->imdb[0] != '\0')
		abort();
	if (ids->tvdb[0] != '\0' || ids->tvdb_conflict)
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	AxisIdentifyIds first;
	AxisIdentifyIds again;
	AxisIdentifyIds cut;
	gsize bounded = MIN(size, (gsize)AXIS_IDENTIFY_NFO_MAX);

	axis_identify_nfo_scan(data, size, &first);
	check(&first, data, bounded);
	axis_identify_nfo_scan(data, size, &again);
	if (memcmp(&first, &again, sizeof(first)) != 0)
		abort();
	axis_identify_nfo_scan(data, bounded, &cut);
	if (memcmp(&first, &cut, sizeof(first)) != 0)
		abort();
	return 0;
}
