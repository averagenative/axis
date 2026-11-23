/*
 * axis-meta-image.c - what an image file is, from its first bytes alone
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-image.h"

#include <string.h>

static guint32
be16(const guint8 *p)
{
	return ((guint32)p[0] << 8) | p[1];
}

static guint32
be32(const guint8 *p)
{
	return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

static guint32
le16(const guint8 *p)
{
	return ((guint32)p[1] << 8) | p[0];
}

static guint32
le24(const guint8 *p)
{
	return ((guint32)p[2] << 16) | ((guint32)p[1] << 8) | p[0];
}

static guint32
le32(const guint8 *p)
{
	return ((guint32)p[3] << 24) | ((guint32)p[2] << 16) | ((guint32)p[1] << 8) | p[0];
}

/* ── JPEG ───────────────────────────────────────────────────────── */

static gboolean
is_frame_marker(guint8 marker)
{
	/* SOF0..SOF15, except DHT (C4), JPG (C8) and DAC (CC). */
	return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

static gboolean
sniff_jpeg(
	const guint8      *data,
	gsize              length,
	AxisMetaImageInfo *info
){
	gsize pos = 2;

	if (length < 4 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
		return FALSE;
	while (pos < length) {
		guint8 marker;
		guint32 segment;

		if (data[pos] != 0xFF)
			return FALSE;
		/* Fill bytes before a marker. */
		while (pos < length && data[pos] == 0xFF)
			pos++;
		if (pos >= length)
			return FALSE;
		marker = data[pos++];
		if (marker == 0x00 || marker == 0xD8 || marker == 0xD9)
			return FALSE;
		/* Markers that stand alone. */
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
			continue;
		if (marker == 0xDA)
			return FALSE;	/* a scan before any frame header */
		if (pos + 2 > length)
			return FALSE;
		segment = be16(data + pos);
		if (segment < 2 || pos + segment > length)
			return FALSE;
		if (is_frame_marker(marker)) {
			guint32 height;
			guint32 width;
			guint8 components;

			/* precision, height, width, components, then 3 bytes each */
			if (segment < 8)
				return FALSE;
			height = be16(data + pos + 3);
			width = be16(data + pos + 5);
			components = data[pos + 7];
			if (height == 0 || width == 0 || components == 0 || segment < 8 + 3 * (guint32)components)
				return FALSE;
			info->type = AXIS_META_IMAGE_JPEG;
			info->width = width;
			info->height = height;
			return TRUE;
		}
		pos += segment;
	}
	return FALSE;
}

/* ── PNG ────────────────────────────────────────────────────────── */

static gboolean
png_depth_valid(
	guint8 colour,
	guint8 depth
){
	switch (colour) {
	case 0:
		return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
	case 3:
		return depth == 1 || depth == 2 || depth == 4 || depth == 8;
	case 2:
	case 4:
	case 6:
		return depth == 8 || depth == 16;
	default:
		return FALSE;
	}
}

static gboolean
sniff_png(
	const guint8      *data,
	gsize              length,
	AxisMetaImageInfo *info
){
	static const guint8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	guint32 width;
	guint32 height;

	/* signature, length, "IHDR", 13 bytes, CRC */
	if (length < 33 || memcmp(data, signature, 8) != 0)
		return FALSE;
	if (be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0)
		return FALSE;
	width = be32(data + 16);
	height = be32(data + 20);
	if (width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu)
		return FALSE;
	if (!png_depth_valid(data[25], data[24]) || data[26] != 0 || data[27] != 0 || data[28] > 1)
		return FALSE;
	info->type = AXIS_META_IMAGE_PNG;
	info->width = width;
	info->height = height;
	return TRUE;
}

/* ── WebP ───────────────────────────────────────────────────────── */

static gboolean
sniff_webp(
	const guint8      *data,
	gsize              length,
	AxisMetaImageInfo *info
){
	guint32 riff;
	guint32 chunk;
	const guint8 *payload;
	guint32 width;
	guint32 height;

	if (length < 20 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
		return FALSE;
	riff = le32(data + 4);
	/* The RIFF size counts from "WEBP"; it must fit what is here. */
	if (riff < 12 || (guint64)riff + 8 > length)
		return FALSE;
	chunk = le32(data + 16);
	if ((guint64)chunk + 20 > (guint64)riff + 8)
		return FALSE;
	payload = data + 20;
	if (memcmp(data + 12, "VP8 ", 4) == 0) {
		/* frame tag (3 bytes, key frame when bit 0 is 0), start code,
		 * then 14-bit width and height */
		if (chunk < 10 || (payload[0] & 0x01) != 0)
			return FALSE;
		if (payload[3] != 0x9D || payload[4] != 0x01 || payload[5] != 0x2A)
			return FALSE;
		width = le16(payload + 6) & 0x3FFF;
		height = le16(payload + 8) & 0x3FFF;
	} else if (memcmp(data + 12, "VP8L", 4) == 0) {
		guint32 bits;

		if (chunk < 5 || payload[0] != 0x2F)
			return FALSE;
		bits = le32(payload + 1);
		if ((bits >> 29) != 0)
			return FALSE;	/* version */
		width = (bits & 0x3FFF) + 1;
		height = ((bits >> 14) & 0x3FFF) + 1;
	} else if (memcmp(data + 12, "VP8X", 4) == 0) {
		if (chunk < 10)
			return FALSE;
		width = le24(payload + 4) + 1;
		height = le24(payload + 7) + 1;
	} else {
		return FALSE;
	}
	if (width == 0 || height == 0)
		return FALSE;
	info->type = AXIS_META_IMAGE_WEBP;
	info->width = width;
	info->height = height;
	return TRUE;
}

gboolean
axis_meta_image_sniff(
	const guint8      *data,
	gsize              length,
	AxisMetaImageInfo *info
){
	memset(info, 0, sizeof(*info));
	if (data == NULL || length == 0)
		return FALSE;
	if (sniff_jpeg(data, length, info) || sniff_png(data, length, info) || sniff_webp(data, length, info))
		return TRUE;
	memset(info, 0, sizeof(*info));
	return FALSE;
}

const gchar *
axis_meta_image_type_to_mime(AxisMetaImageType type)
{
	switch (type) {
	case AXIS_META_IMAGE_JPEG:
		return "image/jpeg";
	case AXIS_META_IMAGE_PNG:
		return "image/png";
	case AXIS_META_IMAGE_WEBP:
		return "image/webp";
	default:
		return NULL;
	}
}

AxisMetaImageType
axis_meta_image_type_from_mime(const gchar *mime)
{
	if (mime == NULL)
		return AXIS_META_IMAGE_NONE;
	if (g_ascii_strcasecmp(mime, "image/jpeg") == 0 || g_ascii_strcasecmp(mime, "image/jpg") == 0 ||
	    g_ascii_strcasecmp(mime, "image/pjpeg") == 0)
		return AXIS_META_IMAGE_JPEG;
	if (g_ascii_strcasecmp(mime, "image/png") == 0)
		return AXIS_META_IMAGE_PNG;
	if (g_ascii_strcasecmp(mime, "image/webp") == 0)
		return AXIS_META_IMAGE_WEBP;
	return AXIS_META_IMAGE_NONE;
}
