/*
 * axis-test-relay.c - test support: a TCP relay that can make one
 * connection go silent without closing it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Blocking sockets, one acceptor thread and two pump threads per relayed
 * connection; every wait is a timed one (POLL_US), so the threads see a
 * stall, a close or the end within that.
 */

#include "axis-test-relay.h"

#include <gio/gio.h>

#define POLL_US (20 * 1000)

typedef struct _Conn Conn;

typedef struct
{
	Conn    *conn;
	GSocket *from;
	GSocket *to;
	GThread *thread;
} Pump;

struct _Conn
{
	AxisTestRelay     *relay;
	guint              number;
	GSocket           *client;
	GSocketConnection *upstream;	/* NULL while held */
	gint               stalled;	/* atomic */
	gint               closed;	/* atomic */
	Pump               up;
	Pump               down;
};

struct _AxisTestRelay
{
	GSocket   *listener;
	gchar     *upstream_host;
	guint16    upstream_port;
	guint16    port;
	GThread   *acceptor;
	gint       stopping;	/* atomic */
	gint       hold_new;	/* atomic */
	GMutex     lock;
	GPtrArray *conns;		/* (lock) of Conn */
	guint      count;		/* (lock) */
};

static void
conn_close(Conn *conn)
{
	if (g_atomic_int_exchange(&conn->closed, TRUE))
		return;
	g_socket_shutdown(conn->client, TRUE, TRUE, NULL);
	if (conn->upstream != NULL)
		g_socket_shutdown(g_socket_connection_get_socket(conn->upstream), TRUE, TRUE, NULL);
}

static gpointer
pump_main(gpointer data)
{
	Pump *pump = data;
	Conn *conn = pump->conn;
	gchar buffer[65536];

	while (!g_atomic_int_get(&conn->relay->stopping) && !g_atomic_int_get(&conn->closed)) {
		gssize n;
		gssize off = 0;

		if (g_atomic_int_get(&conn->stalled)) {
			g_usleep(POLL_US);
			continue;
		}
		if (!g_socket_condition_timed_wait(pump->from, G_IO_IN, POLL_US, NULL, NULL))
			continue;
		/* Stalled while it waited: leave the data where it is. */
		if (g_atomic_int_get(&conn->stalled))
			continue;
		n = g_socket_receive(pump->from, buffer, sizeof(buffer), NULL, NULL);
		if (n <= 0)
			break;
		while (off < n) {
			gssize m = g_socket_send(pump->to, buffer + off, (gsize)(n - off), NULL, NULL);

			if (m <= 0)
				break;
			off += m;
		}
		if (off < n)
			break;
	}

	conn_close(conn);
	return NULL;
}

static void
conn_free(gpointer data)
{
	Conn *conn = data;

	conn_close(conn);
	if (conn->up.thread != NULL)
		g_thread_join(conn->up.thread);
	if (conn->down.thread != NULL)
		g_thread_join(conn->down.thread);
	g_socket_close(conn->client, NULL);
	g_object_unref(conn->client);
	if (conn->upstream != NULL) {
		g_io_stream_close(G_IO_STREAM(conn->upstream), NULL, NULL);
		g_object_unref(conn->upstream);
	}
	g_free(conn);
}

static void
relay_start(
	AxisTestRelay *self,
	Conn          *conn
){
	g_autoptr(GSocketClient) connector = g_socket_client_new();
	GSocket *upstream;

	conn->upstream = g_socket_client_connect_to_host(connector, self->upstream_host, self->upstream_port, NULL, NULL);
	if (conn->upstream == NULL) {
		conn_close(conn);
		return;
	}
	upstream = g_socket_connection_get_socket(conn->upstream);
	conn->up.conn = conn;
	conn->up.from = conn->client;
	conn->up.to = upstream;
	conn->down.conn = conn;
	conn->down.from = upstream;
	conn->down.to = conn->client;
	conn->up.thread = g_thread_new("relay-up", pump_main, &conn->up);
	conn->down.thread = g_thread_new("relay-down", pump_main, &conn->down);
}

static gpointer
accept_main(gpointer data)
{
	AxisTestRelay *self = data;

	while (!g_atomic_int_get(&self->stopping)) {
		GSocket *client;
		Conn *conn;

		if (!g_socket_condition_timed_wait(self->listener, G_IO_IN, POLL_US, NULL, NULL))
			continue;
		client = g_socket_accept(self->listener, NULL, NULL);
		if (client == NULL)
			continue;
		g_socket_set_blocking(client, TRUE);

		conn = g_new0(Conn, 1);
		conn->relay = self;
		conn->client = client;
		g_mutex_lock(&self->lock);
		conn->number = ++self->count;
		g_ptr_array_add(self->conns, conn);
		g_mutex_unlock(&self->lock);

		/* Held: accepted, never answered. */
		if (!g_atomic_int_get(&self->hold_new))
			relay_start(self, conn);
	}

	return NULL;
}

AxisTestRelay *
axis_test_relay_new(
	const gchar *upstream_host,
	guint16      upstream_port
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketAddress) bound = NULL;
	AxisTestRelay *self = g_new0(AxisTestRelay, 1);

	self->upstream_host = g_strdup(upstream_host);
	self->upstream_port = upstream_port;
	g_mutex_init(&self->lock);
	self->conns = g_ptr_array_new_with_free_func(conn_free);

	self->listener = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
	g_assert_no_error(error);
	address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_assert_true(g_socket_bind(self->listener, address, TRUE, &error));
	g_assert_true(g_socket_listen(self->listener, &error));
	g_assert_no_error(error);
	bound = g_socket_get_local_address(self->listener, &error);
	g_assert_no_error(error);
	self->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));

	self->acceptor = g_thread_new("relay-accept", accept_main, self);
	return self;
}

void
axis_test_relay_free(AxisTestRelay *self)
{
	if (self == NULL)
		return;

	g_atomic_int_set(&self->stopping, TRUE);
	g_thread_join(self->acceptor);
	g_ptr_array_unref(self->conns);
	g_socket_close(self->listener, NULL);
	g_object_unref(self->listener);
	g_mutex_clear(&self->lock);
	g_free(self->upstream_host);
	g_free(self);
}

guint16
axis_test_relay_get_port(AxisTestRelay *self)
{
	return self->port;
}

guint
axis_test_relay_count(AxisTestRelay *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->count;
	g_mutex_unlock(&self->lock);
	return n;
}

void
axis_test_relay_stall(
	AxisTestRelay *self,
	guint          n,
	gboolean       stalled
){
	guint i;
	gboolean found = FALSE;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->conns->len; i++) {
		Conn *conn = g_ptr_array_index(self->conns, i);

		if (conn->number == n) {
			g_atomic_int_set(&conn->stalled, stalled);
			found = TRUE;
		}
	}
	g_mutex_unlock(&self->lock);
	g_assert_true(found);
}

void
axis_test_relay_hold_new(
	AxisTestRelay *self,
	gboolean       hold
){
	g_atomic_int_set(&self->hold_new, hold);
}
