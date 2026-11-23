/*
 * axis-test-convert.h - test support for conversion plans: probe records
 * built by hand, capabilities, hostile text
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_TEST_CONVERT_H
#define AXIS_TEST_CONVERT_H

#include <glib.h>

#include "probe/axis-probe-model.h"
#include "transcode/axis-convert.h"

G_BEGIN_DECLS

/* A probed file as the probe would record it: status ok, progressive,
 * classified as the streams say.  @container is ffprobe's name. */
AxisMediaInfo *
axis_test_convert_info(const gchar *container);

/* The picture: stream @index, codec, size, pixel format and depth, the four
 * colour properties (NULL: unstated) and the HDR class. */
AxisMediaStream *
axis_test_convert_video(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	gint           width,
	gint           height,
	const gchar   *pixel_format,
	gint           bit_depth,
	const gchar   *primaries,
	const gchar   *transfer,
	const gchar   *matrix,
	const gchar   *range,
	AxisHdrFormat  hdr
);

/* The HDR10 values the tests use (BT.2020 primaries, 1000 cd/m2). */
void
axis_test_convert_hdr10_values(AxisHdr10Metadata *m);

AxisMediaStream *
axis_test_convert_audio(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	gint           channels,
	const gchar   *language,
	gboolean       is_default
);

AxisMediaStream *
axis_test_convert_subtitle(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	const gchar   *language,
	gboolean       is_default,
	gboolean       is_forced
);

/* An attachment (a font) or a cover picture. */
AxisMediaStream *
axis_test_convert_attachment(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec
);

AxisMediaStream *
axis_test_convert_cover(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec
);

/* Sets the record's derived facts from its streams (call last). */
void
axis_test_convert_finish(AxisMediaInfo *info);

/* Capabilities: every encoder of the table measured and capable of all it
 * is asked (10 bits and HDR10 for HEVC), the helpers there. */
AxisEncoderCapabilities *
axis_test_convert_caps_all(void);

/* Text of the kind a file's author can put in names, titles, tags and
 * languages, to show that none reaches an argument vector; ended by %NULL. */
const gchar *const *
axis_test_convert_hostile_texts(void);

G_END_DECLS

#endif /* AXIS_TEST_CONVERT_H */
