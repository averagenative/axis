/*
 * axis-portal-client.h - axis-portal's one way to the daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  Every request the portal makes of the
 * daemon is made here, and it is always the same request: a POST of one
 * JSON object to the configured address, the portal API's prefix and one of
 * the eleven operations' names (an AxisPortalOp, not a text: no caller can
 * name a path), with the portal's token and no other header of anyone's
 * choosing.  Nothing of a browser's request is in it but what the caller put
 * into the body through axis-portal-wire.h.
 *
 * No redirect is followed, no proxy is used, no cookie is kept, and an
 * answer is read up to a fixed size (a poster up to 4 MiB, anything else up
 * to AXIS_PORTAL_WIRE_MAX_ANSWER).  At most AXIS_PORTAL_CLIENT_MAX_CALLS run
 * at once; one more is answered as "not reached" at once, so that a flood of
 * pages cannot queue without bound.
 *
 * Main loop only.
 */

#ifndef AXIS_PORTAL_CLIENT_H
#define AXIS_PORTAL_CLIENT_H

#include <glib.h>

#include "api/axis-portal-proto.h"
#include "axis-portal-config.h"

G_BEGIN_DECLS

#define AXIS_PORTAL_CLIENT_MAX_CALLS     64
#define AXIS_PORTAL_CLIENT_MAX_ART_BYTES (4 * 1024 * 1024)

typedef struct _AxisPortalClient AxisPortalClient;

/**
 * AxisPortalAnswerFunc:
 * @status: the daemon's status; 0 when it was not reached, did not answer in
 *   time, or answered what cannot be taken (a redirect, too many bytes, a
 *   type that is neither JSON nor, for a poster, an image)
 * @body: (nullable): the answer's bytes; %NULL with @status 0
 * @content_type: (nullable): its media type, lower case, without parameters
 * @user_data: (closure)
 *
 * Called once for each call, on the main loop, never before
 * axis_portal_client_call() has returned.
 */
typedef void (*AxisPortalAnswerFunc)(
	guint        status,
	GBytes      *body,
	const gchar *content_type,
	gpointer     user_data
);

AxisPortalClient *
axis_portal_client_new(
	const AxisPortalConfig  *config,
	GError                 **error
);

/* Ends every call in flight (each is answered as not reached) and frees. */
void
axis_portal_client_free(AxisPortalClient *self);

/**
 * axis_portal_client_call:
 * @op: the operation
 * @json: (transfer full) (nullable): its body, from axis-portal-wire.h; wiped
 *   when it has been sent.  %NULL (a value the writer refused) is answered
 *   as not reached, and nothing is sent
 */
void
axis_portal_client_call(
	AxisPortalClient     *self,
	AxisPortalOp          op,
	gchar                *json,
	AxisPortalAnswerFunc  func,
	gpointer              user_data
);

/* "hello", "login", ...: the last part of the operation's path. */
const gchar *
axis_portal_client_op_name(AxisPortalOp op);

/* How many calls are in flight. */
guint
axis_portal_client_get_pending(AxisPortalClient *self);

G_END_DECLS

#endif /* AXIS_PORTAL_CLIENT_H */
