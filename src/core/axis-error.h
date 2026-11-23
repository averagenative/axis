/*
 * axis-error.h - error domain shared by the whole engine
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_ERROR_H
#define AXIS_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_ERROR (axis_error_quark())

/**
 * AxisError:
 * @AXIS_ERROR_FAILED: unspecified failure
 * @AXIS_ERROR_CONFIG: the configuration file or an environment override is invalid
 * @AXIS_ERROR_MODULE: a module could not be registered or started
 * @AXIS_ERROR_LISTEN: the listen address could not be bound
 * @AXIS_ERROR_SETTING: a setting declaration is wrong, a setting is unknown,
 *   or a value is not acceptable for it (the message names the setting and
 *   never holds a value)
 * @AXIS_ERROR_DATA_DIR: the data directory cannot be used (the message names
 *   it), or a file in it cannot be read or written
 * @AXIS_ERROR_DATABASE_IN_USE: another daemon holds the database (the
 *   single-daemon lock; the message says who)
 */
typedef enum
{
	AXIS_ERROR_FAILED,
	AXIS_ERROR_CONFIG,
	AXIS_ERROR_MODULE,
	AXIS_ERROR_LISTEN,
	AXIS_ERROR_SETTING,
	AXIS_ERROR_DATA_DIR,
	AXIS_ERROR_DATABASE_IN_USE
} AxisError;

GQuark
axis_error_quark(void);

G_END_DECLS

#endif /* AXIS_ERROR_H */
