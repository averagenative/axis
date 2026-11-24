/*
 * axis-web-hub.h - server-sent events: named topics, many subscribers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A page subscribes with GET /events?topic=jobs&topic=job:12 (any number
 * of topics up to AXIS_WEB_HUB_MAX_TOPICS), which needs a session like
 * any other route.  Any code publishes with axis_web_hub_publish(): the
 * event goes to every stream subscribed to the topic and to no other.
 * There is no history: a stream that connects, or reconnects, hears only
 * what is published afterwards, so a fragment should carry the whole of
 * what it shows and a page should fetch its current state when its
 * stream opens.
 *
 * Bounds.  A session has at most %AXIS_WEB_HUB_MAX_STREAMS_PER_SESSION
 * streams (opening one more closes its oldest: a page left behind) and an
 * account, over all its sessions, at most %AXIS_WEB_HUB_MAX_STREAMS_PER_USER
 * (one more is refused: it is answered with a line for the page saying
 * that live updates are off in that tab, and told to try again in a
 * minute), the daemon at most %AXIS_WEB_HUB_MAX_STREAMS in all, of which the last
 * quarter only administrators may open (more are answered 503): it takes
 * twelve accounts to fill what the others may use, and administrators
 * always have streams left.  A stream is
 * dropped when it has data queued that the client has not read for
 * %AXIS_WEB_HUB_STALL_MS, or at once when the queue passes
 * %AXIS_WEB_HUB_MAX_PENDING_BYTES, so what a slow client costs is
 * bounded.  A comment line is sent on a stream idle for
 * %AXIS_WEB_HUB_KEEPALIVE_MS.  A session that ends (logout, revocation,
 * a disabled or deleted account, a password change elsewhere) has its
 * streams closed, and so does every session of an account whose role
 * changes (the browser opens them again, judged by the new role).
 *
 * An event stream's connection is under these limits, not under the
 * request deadlines of axis-web-limits.h once its head has arrived.
 *
 * Main loop thread only, throughout.  Free the hub before the
 * AxisWebAuth it was installed on.
 */

#ifndef AXIS_WEB_HUB_H
#define AXIS_WEB_HUB_H

#include <glib.h>

#include "web/axis-web.h"
#include "web/axis-web-hub-topic.h"

G_BEGIN_DECLS

/* Where a page reads its event stream. */
#define AXIS_WEB_HUB_EVENTS_PATH              AXIS_WEB_LIMITS_STREAM_PATH

/* The id of the line on a page that says its stream was refused. */
#define AXIS_WEB_HUB_STATUS_ID                "live-status"

/* A stream refused at an account's cap tells the browser to try again
 * after this long. */
#define AXIS_WEB_HUB_REFUSED_RETRY_MS         60000

#define AXIS_WEB_HUB_MAX_STREAMS_PER_SESSION  8
#define AXIS_WEB_HUB_MAX_STREAMS_PER_USER     16
#define AXIS_WEB_HUB_MAX_STREAMS              256

/* max_streams / this is kept for administrators, unless the options say. */
#define AXIS_WEB_HUB_ADMIN_RESERVED_PART      4
#define AXIS_WEB_HUB_KEEPALIVE_MS             20000
#define AXIS_WEB_HUB_STALL_MS                 30000
#define AXIS_WEB_HUB_MAX_PENDING_BYTES        (256 * 1024)

/*
 * A topic whose name ends with this is delivered only to administrators:
 * anyone else asking for it gets 403.  Fragments are shared by every
 * subscriber of a topic, so anything only administrators may see goes on
 * a topic of its own with this suffix.
 */
#define AXIS_WEB_HUB_ADMIN_SUFFIX             ":admin"

/* The largest fragment published; a larger one is not sent at all. */
#define AXIS_WEB_HUB_MAX_EVENT_BYTES          (64 * 1024)

/**
 * AxisWebHubOptions:
 *
 * Every field left 0 takes its default.  Tests shorten the times.
 */
typedef struct
{
	guint  max_per_session;
	guint  max_per_user;
	guint  max_streams;
	guint  admin_reserved;	/* of max_streams; 0: a quarter of it */
	guint  keepalive_ms;
	guint  stall_ms;
	gsize  max_pending_bytes;
} AxisWebHubOptions;

typedef struct _AxisWebHub AxisWebHub;

/* @options: (nullable): %NULL for the defaults */
AxisWebHub *
axis_web_hub_new(const AxisWebHubOptions *options);

/* Closes every stream and frees the hub. */
void
axis_web_hub_free(AxisWebHub *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebHub, axis_web_hub_free)

/**
 * axis_web_hub_install:
 * @self: the hub
 * @web: the shell; /events is added to its routes and its pages learn to
 *   open a stream (see axis_web_page_add_topic())
 * @auth: the accounts, whose sessions' ends close streams; one hub per
 *   auth
 *
 * Call after axis_web_new().
 */
void
axis_web_hub_install(
	AxisWebHub  *self,
	AxisWeb     *web,
	AxisWebAuth *auth
);

/**
 * axis_web_hub_publish:
 * @self: the hub
 * @topic: a valid topic (see axis-web-hub-topic.h)
 * @event_name: a valid event name, what the page's sse-swap names
 * @html: the fragment.  A carriage return in it is sent as a line feed
 *   (a bare CR ends a line in an event stream, which would let text that
 *   was escaped for HTML forge fields of the stream); nothing else is
 *   changed.  Larger than %AXIS_WEB_HUB_MAX_EVENT_BYTES, it is not sent.
 *
 * Returns: how many streams it was queued for
 */
guint
axis_web_hub_publish(
	AxisWebHub  *self,
	const gchar *topic,
	const gchar *event_name,
	const gchar *html
);

/* How many streams are subscribed to @topic. */
guint
axis_web_hub_count_subscribers(
	AxisWebHub  *self,
	const gchar *topic
);

/* Streams open now. */
guint
axis_web_hub_get_n_streams(AxisWebHub *self);

/* Bytes queued for clients that have not read them, over all streams. */
gsize
axis_web_hub_get_pending_bytes(AxisWebHub *self);

G_END_DECLS

#endif /* AXIS_WEB_HUB_H */
