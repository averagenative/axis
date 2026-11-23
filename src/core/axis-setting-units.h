/*
 * axis-setting-units.h - durations and byte sizes as people type them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The parsers take text from the Settings page (outside input, so they
 * have fuzz targets) and are strict: whole numbers only, a unit written
 * once, nothing left over.  They depend on GLib only.
 */

#ifndef AXIS_SETTING_UNITS_H
#define AXIS_SETTING_UNITS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * axis_setting_parse_duration:
 * @text: for example "90", "45s", "15m", "2h", "7d", "2w" or "1h 30m"
 * @seconds: (out) (optional): the length in seconds
 *
 * One or more whole numbers each followed by a unit (s, m, h, d or w,
 * either case), at most one of each unit, optionally separated by spaces.
 * A single number without a unit is seconds.  Surrounding spaces are
 * ignored.  Fails on anything else, and on a total that does not fit in 63
 * bits.
 *
 * Returns: %TRUE if @text is a duration
 */
gboolean
axis_setting_parse_duration(
	const gchar *text,
	gint64      *seconds
);

/**
 * axis_setting_format_duration:
 * @seconds: 0 or more
 *
 * Returns: (transfer full): @seconds written with days, hours, minutes and
 *   seconds ("1d 2h", "90s" is "1m 30s", 0 is "0s"), which
 *   axis_setting_parse_duration() reads back to the same value
 */
gchar *
axis_setting_format_duration(gint64 seconds);

/**
 * axis_setting_parse_byte_size:
 * @text: for example "512", "64K", "10MiB", "2 GB" or "1T"
 * @bytes: (out) (optional): the size in bytes
 *
 * A whole number and an optional unit: B, K, M, G, T or P, each also
 * written KB or KiB and so on, either case.  Every unit is a power of
 * 1024.  Surrounding spaces, and spaces between the number and the unit,
 * are ignored.  Fails on anything else, and on a size that does not fit in
 * 63 bits.
 *
 * Returns: %TRUE if @text is a size
 */
gboolean
axis_setting_parse_byte_size(
	const gchar *text,
	gint64      *bytes
);

/**
 * axis_setting_format_byte_size:
 * @bytes: 0 or more
 *
 * Returns: (transfer full): @bytes in the largest of B, KiB, MiB, GiB, TiB
 *   and PiB that divides it exactly ("10GiB", "1536MiB", "0B"), which
 *   axis_setting_parse_byte_size() reads back to the same value
 */
gchar *
axis_setting_format_byte_size(gint64 bytes);

G_END_DECLS

#endif /* AXIS_SETTING_UNITS_H */
