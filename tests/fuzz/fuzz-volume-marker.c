/*
 * fuzz-volume-marker.c - libFuzzer target for the volume marker parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A marker is read from a disk, so it is outside input.  Besides memory
 * safety this checks what makes the parser safe to build on: it accepts
 * only the one canonical form (formatting what it accepted gives back the
 * input byte for byte), what it accepts is valid, and it reads nothing
 * past the length it is given.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "storage/axis-volume-marker.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	AxisVolumeMarker marker;
	AxisVolumeMarker again;
	gchar buffer[AXIS_VOLUME_MARKER_MAX_BYTES];
	guint8 *copy;
	gsize length;
	GError *error = NULL;

	/* An exact-size copy, so that a read past the end is caught. */
	copy = g_memdup2(data, size);
	if (!axis_volume_marker_parse(copy, size, &marker, &error)) {
		if (error == NULL || error->domain != AXIS_VOLUME_MARKER_ERROR)
			abort();
		g_error_free(error);
		g_free(copy);
		return 0;
	}
	if (error != NULL)
		abort();

	if (!axis_volume_uuid_is_valid(marker.uuid) || marker.created < 0 ||
	    marker.created > AXIS_VOLUME_MARKER_MAX_TIME)
		abort();

	length = axis_volume_marker_format(&marker, buffer);
	if (length != size || memcmp(buffer, data, size) != 0)
		abort();
	if (!axis_volume_marker_parse((const guint8 *)buffer, length, &again, NULL) ||
	    strcmp(again.uuid, marker.uuid) != 0 || again.created != marker.created)
		abort();

	g_free(copy);
	return 0;
}
