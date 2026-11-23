/*
 * axis-module-state.h - keeping which modules are switched on
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The registry (axis-module.h) holds the state while the daemon runs; this
 * is the part that remembers it across restarts, in the module_state
 * table.  Both functions block on the database.
 */

#ifndef AXIS_MODULE_STATE_H
#define AXIS_MODULE_STATE_H

#include <glib.h>

#include "core/axis-module.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

/**
 * axis_module_state_load:
 * @registry: the registry, with every module registered and not yet
 *   started
 * @pool: the database
 * @error: (nullable): return location for an error
 *
 * Applies the stored states (see axis_module_registry_load_states()).
 * Call it before axis_module_registry_start_all().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_module_state_load(
	AxisModuleRegistry  *registry,
	AxisDbPool          *pool,
	GError             **error
);

/**
 * axis_module_state_store:
 * @pool: the database
 * @id: a module id
 * @enabled: its state
 * @error: (nullable): return location for an error
 *
 * Stores one module's state.  Callable from a worker thread.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_module_state_store(
	AxisDbPool   *pool,
	const gchar  *id,
	gboolean      enabled,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_MODULE_STATE_H */
