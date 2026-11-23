/*
 * axis-module-state.c - keeping which modules are switched on
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-module-state.h"

gboolean
axis_module_state_load(
	AxisModuleRegistry  *registry,
	AxisDbPool          *pool,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree const gchar **ids = NULL;
	g_autofree gboolean *states = NULL;
	guint n;
	guint i;

	g_return_val_if_fail(registry != NULL, FALSE);
	g_return_val_if_fail(pool != NULL, FALSE);

	if (!axis_db_pool_exec(pool, "SELECT module_id, enabled FROM module_state ORDER BY module_id",
	                       0, NULL, &result, error))
		return FALSE;

	n = axis_db_result_get_n_rows(result);
	ids = g_new0(const gchar *, n + 1);
	states = g_new0(gboolean, n + 1);
	for (i = 0; i < n; i++) {
		ids[i] = axis_db_result_get_text(result, i, 0);
		axis_db_result_get_boolean(result, i, 1, &states[i]);
	}

	axis_module_registry_load_states(registry, ids, states, n);
	return TRUE;
}

gboolean
axis_module_state_store(
	AxisDbPool   *pool,
	const gchar  *id,
	gboolean      enabled,
	GError      **error
){
	const gchar *params[2];

	g_return_val_if_fail(pool != NULL, FALSE);
	g_return_val_if_fail(id != NULL, FALSE);

	params[0] = id;
	params[1] = enabled ? "true" : "false";
	return axis_db_pool_exec(pool,
		"INSERT INTO module_state (module_id, enabled) VALUES ($1, $2::boolean)"
		" ON CONFLICT (module_id) DO UPDATE SET enabled = EXCLUDED.enabled, updated_at = now()",
		2, params, NULL, error);
}
