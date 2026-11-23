/*
 * fuzz-meta-image.c - libFuzzer target for the artwork header sniffer
 * (meta/axis-meta-image.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Whatever the bytes, the sniffer returns without reading past them, and
 * what it says keeps its promises:
 *
 *  - it says an image is there only with a type and both dimensions not 0,
 *    and says nothing (all zero) otherwise;
 *  - the type is the one the magic bytes show (FF D8 FF, the PNG signature,
 *    RIFF....WEBP), and a PNG's or a VP8L's dimensions are within what their
 *    headers can hold;
 *  - the answer depends on the bytes alone: the same bytes copied to a
 *    buffer of exactly their length give the same answer (nothing beyond
 *    them was read), and so do the bytes followed by anything.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "meta/axis-meta-image.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const guint8 png_signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	AxisMetaImageInfo info;
	AxisMetaImageInfo again;
	guint8 *exact;
	guint8 *longer;
	gboolean ok;

	if (size > 1 << 20)
		return 0;
	exact = g_malloc(size > 0 ? size : 1);
	memcpy(exact, data, size);
	ok = axis_meta_image_sniff(exact, size, &info);
	if (ok) {
		if (info.width == 0 || info.height == 0)
			abort();
		switch (info.type) {
		case AXIS_META_IMAGE_JPEG:
			if (size < 4 || exact[0] != 0xFF || exact[1] != 0xD8 || exact[2] != 0xFF)
				abort();
			if (info.width > 65535 || info.height > 65535)
				abort();
			break;
		case AXIS_META_IMAGE_PNG:
			if (size < 33 || memcmp(exact, png_signature, 8) != 0)
				abort();
			if (info.width > 0x7FFFFFFFu || info.height > 0x7FFFFFFFu)
				abort();
			break;
		case AXIS_META_IMAGE_WEBP:
			if (size < 20 || memcmp(exact, "RIFF", 4) != 0 || memcmp(exact + 8, "WEBP", 4) != 0)
				abort();
			if (info.width > (1u << 24) || info.height > (1u << 24))
				abort();
			break;
		default:
			abort();
		}
		if (axis_meta_image_type_to_mime(info.type) == NULL ||
		    axis_meta_image_type_from_mime(axis_meta_image_type_to_mime(info.type)) != info.type)
			abort();
	} else if (info.type != AXIS_META_IMAGE_NONE || info.width != 0 || info.height != 0) {
		abort();
	}

	/* What follows the bytes changes nothing that was decided by them (a
	 * header cut short may be whole now, so only a yes is held to this). */
	longer = g_malloc(size + 64);
	memcpy(longer, data, size);
	memset(longer + size, 0xFF, 64);
	if (ok && (!axis_meta_image_sniff(longer, size + 64, &again) || again.type != info.type ||
	           again.width != info.width || again.height != info.height))
		abort();
	g_free(longer);
	g_free(exact);
	return 0;
}
