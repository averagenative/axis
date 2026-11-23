/*
 * axis-meta-image.h - what an image file is, from its first bytes alone
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Artwork from a provider is kept only if it is a JPEG, PNG or WebP image
 * of sensible dimensions.  The daemon never decodes an image (the browser
 * that shows it does, as it would on the provider's own site): this reads
 * the type from the magic bytes and the dimensions from the header, and
 * checks that the header is well formed:
 *
 *  - JPEG: SOI, then segments with valid lengths up to the first frame
 *    header (SOF0 to SOF15 but DHT, JPG and DAC), whose height and width
 *    are not zero; a scan before any frame header is refused.
 *  - PNG: the signature, then IHDR first, of length 13, with a width and a
 *    height from 1 to 2^31 - 1, and a valid bit depth, colour type,
 *    compression, filter and interlace method.
 *  - WebP: RIFF with a size that fits the data, "WEBP", then a VP8 (key
 *    frame start code), VP8L (signature byte, version 0) or VP8X (canvas
 *    size) chunk that fits too.
 *
 * GLib only, no I/O; fuzzed (tests/fuzz/fuzz-meta-image.c).
 */

#ifndef AXIS_META_IMAGE_H
#define AXIS_META_IMAGE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
	AXIS_META_IMAGE_NONE = 0,
	AXIS_META_IMAGE_JPEG,
	AXIS_META_IMAGE_PNG,
	AXIS_META_IMAGE_WEBP
} AxisMetaImageType;

typedef struct
{
	AxisMetaImageType type;
	guint32           width;
	guint32           height;
} AxisMetaImageInfo;

/**
 * axis_meta_image_sniff:
 * @data: the file's bytes (or its beginning: only the header is read)
 * @length: how many
 * @info: (out): its type and dimensions; zero when it is not one
 *
 * Returns: %TRUE if @data starts with a well-formed JPEG, PNG or WebP header
 */
gboolean
axis_meta_image_sniff(
	const guint8      *data,
	gsize              length,
	AxisMetaImageInfo *info
);

/* "image/jpeg", "image/png", "image/webp"; %NULL for none. */
const gchar *
axis_meta_image_type_to_mime(AxisMetaImageType type);

/* The type a Content-Type names ("image/jpeg", also "image/jpg",
 * "image/pjpeg", "image/png", "image/webp"); %AXIS_META_IMAGE_NONE for
 * anything else. */
AxisMetaImageType
axis_meta_image_type_from_mime(const gchar *mime);

G_END_DECLS

#endif /* AXIS_META_IMAGE_H */
