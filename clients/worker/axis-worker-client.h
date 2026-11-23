/*
 * axis-worker-client.h - axis-worker's one HTTP client: POSTs of JSON to
 * the worker API
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What it does and does not do:
 *
 *  - only POST, to the configured daemon's AXIS_WORKER_API_PREFIX
 *    followed by the request's name, with "Authorization: Bearer <token>",
 *    a JSON body and nothing a browser would send;
 *  - certificates are verified (the system's store, or only the configured
 *    CA file); there is no way to turn that off;
 *  - no proxy (the environment's http_proxy and the like are not read), no
 *    cookies, no content decoding, no redirect (an answer 3xx is a
 *    failure: the token is never sent anywhere else);
 *  - an answer is read up to AXIS_WORKER_MAX_RESPONSE_BYTES (more fails),
 *    and the whole exchange has a deadline;
 *  - no message it makes contains the token.
 *
 * Asynchronous, on the thread-default main context of the caller, so that
 * a slow daemon never keeps the worker from killing an encoder on time.
 */

#ifndef AXIS_WORKER_CLIENT_H
#define AXIS_WORKER_CLIENT_H

#include <gio/gio.h>

#include "axis-worker-config.h"

G_BEGIN_DECLS

typedef struct _AxisWorkerClient AxisWorkerClient;

AxisWorkerClient *
axis_worker_client_new(
	const AxisWorkerConfig  *config,
	GError                 **error
);

void
axis_worker_client_free(AxisWorkerClient *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerClient, axis_worker_client_free)

/**
 * axis_worker_client_post:
 * @request: "register", "claim", "heartbeat", "complete" or "fail"
 * @json: the body
 * @timeout_ms: the deadline for the whole exchange
 * @callback: called once, on the caller's main context
 *
 * Starts one exchange.
 */
void
axis_worker_client_post(
	AxisWorkerClient    *self,
	const gchar         *request,
	const gchar         *json,
	guint                timeout_ms,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * axis_worker_client_post_finish:
 * @status: (out): the HTTP status (any: 4xx and 5xx are answers too)
 * @body: (out) (transfer full): the answer's bytes
 *
 * Returns: %FALSE (and @error) when there is no answer: the daemon could
 *   not be reached, the certificate was refused, the deadline passed, the
 *   answer was too long or a redirect
 */
gboolean
axis_worker_client_post_finish(
	AxisWorkerClient  *self,
	GAsyncResult      *result,
	guint             *status,
	GBytes           **body,
	GError           **error
);

G_END_DECLS

#endif /* AXIS_WORKER_CLIENT_H */
