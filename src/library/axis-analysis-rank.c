/*
 * axis-analysis-rank.c - which copy of a title looks best
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-analysis-rank.h"

#include <string.h>

gint
axis_analysis_resolution_rank(const gchar *resolution)
{
	if (resolution == NULL)
		return 0;
	if (strcmp(resolution, "2160p") == 0)
		return 4;
	if (strcmp(resolution, "1080p") == 0)
		return 3;
	if (strcmp(resolution, "720p") == 0)
		return 2;
	if (strcmp(resolution, "sd") == 0)
		return 1;
	return 0;
}

gint
axis_analysis_hdr_rank(
	const gchar *hdr,
	gboolean     uncertain
){
	if (hdr == NULL || uncertain)
		return 0;
	if (strcmp(hdr, "dolby_vision") == 0)
		return 5;
	if (strcmp(hdr, "hdr10plus") == 0)
		return 4;
	if (strcmp(hdr, "hdr10") == 0)
		return 3;
	if (strcmp(hdr, "hlg") == 0)
		return 2;
	if (strcmp(hdr, "sdr") == 0)
		return 1;
	return 0;
}

/* Better first: a larger number sorts first. */
static gint
by_number_desc(
	gint64 a,
	gint64 b
){
	return a > b ? -1 : a < b ? 1 : 0;
}

gint
axis_analysis_copy_compare(
	const AxisAnalysisCopy *a,
	const AxisAnalysisCopy *b
){
	gint result;

	result = by_number_desc(a->probed ? 1 : 0, b->probed ? 1 : 0);
	if (result != 0)
		return result;
	result = by_number_desc(axis_analysis_resolution_rank(a->resolution),
		axis_analysis_resolution_rank(b->resolution));
	if (result != 0)
		return result;
	result = by_number_desc(axis_analysis_hdr_rank(a->hdr, a->hdr_uncertain),
		axis_analysis_hdr_rank(b->hdr, b->hdr_uncertain));
	if (result != 0)
		return result;
	result = by_number_desc(a->lossless > 0 ? 1 : 0, b->lossless > 0 ? 1 : 0);
	if (result != 0)
		return result;
	if (a->bits_per_pixel >= 0 && b->bits_per_pixel >= 0) {
		if (a->bits_per_pixel > b->bits_per_pixel)
			return -1;
		if (a->bits_per_pixel < b->bits_per_pixel)
			return 1;
	} else {
		result = by_number_desc(a->bitrate, b->bitrate);
		if (result != 0)
			return result;
	}
	result = by_number_desc(a->size, b->size);
	if (result != 0)
		return result;
	return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}
