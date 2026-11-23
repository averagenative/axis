/*
 * axis-size.h - sizes in words: the one way the interface writes them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Binary units (KiB, MiB, GiB, ...) everywhere, one decimal at most, and a
 * whole number without a fraction: "25 GiB", "2.4 GiB", "512 MiB", never
 * "25.0 GiB" and never decimal gigabytes, with a no-break space (U+00A0)
 * between the number and its unit (but for "bytes", as g_format_size() has
 * it).  Pure GLib, so the quality layer
 * (which the fuzz targets link without the web code) uses it too.
 */

#ifndef AXIS_SIZE_H
#define AXIS_SIZE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * axis_size_format:
 * @bytes: any size
 *
 * Returns: (transfer full): @bytes in the largest binary unit that keeps it
 *   at 1 or more, rounded to one decimal, the decimal dropped when it is
 *   zero: "512 bytes", "1 byte", "1.5 KiB", "25 GiB"
 */
gchar *
axis_size_format(guint64 bytes);

/**
 * axis_size_unit_of:
 * @bytes: any size
 *
 * Returns: the unit axis_size_format() would use for @bytes: 0 for bytes, 1
 *   for KiB, 2 for MiB, up to 6 for EiB
 */
guint
axis_size_unit_of(guint64 bytes);

/**
 * axis_size_format_as:
 * @bytes: any size
 * @unit: 0 (bytes) to 6 (EiB), as axis_size_unit_of() gives
 * @decimals: at most 6 decimals to keep; zeros at the end are dropped
 *
 * @bytes in @unit, so that two sizes can be written in the same unit with as
 * many decimals as it takes for them to read differently.
 *
 * Returns: (transfer full): the text, such as "2.43 GiB"
 */
gchar *
axis_size_format_as(
	guint64 bytes,
	guint   unit,
	guint   decimals
);

/**
 * axis_size_number_as:
 * @bytes: any size
 * @unit: 0 (bytes) to 6 (EiB)
 * @decimals: at most 6 decimals to keep; zeros at the end are dropped
 *
 * Just the number of axis_size_format_as(), for a field that carries its
 * unit in its label ("25", "5.8594").
 *
 * Returns: (transfer full): the text
 */
gchar *
axis_size_number_as(
	guint64 bytes,
	guint   unit,
	guint   decimals
);

G_END_DECLS

#endif /* AXIS_SIZE_H */
