/*
 * axis-test-db.c - test support: a fresh database per test, and log checks
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-db.h"

#include <arpa/inet.h>
#include <libpq-fe.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct _AxisTestDb
{
	gchar  *base_url;	/* AXIS_TEST_DATABASE_URL */
	gchar  *name;
	gchar  *url;
	gchar  *host;
	gchar  *port;
	gchar  *user;
	gchar  *password;
	gchar  *admin_db;
	PGconn *conn;		/* to the test database, opened on demand */
	PGconn *admin;		/* to admin_db */
};

static void
quiet_notice(
	void       *arg,
	const char *message
){
}

static PGconn *
connect_or_fail(const gchar *url)
{
	PGconn *conn = PQconnectdb(url);

	if (PQstatus(conn) != CONNECTION_OK) {
		g_test_message("cannot connect to the test server: %s", PQerrorMessage(conn));
		PQfinish(conn);
		g_assert_not_reached();
	}

	/* The helper's own notices are not the code under test's. */
	PQsetNoticeProcessor(conn, quiet_notice, NULL);
	return conn;
}

static gchar *
query(
	PGconn      *conn,
	const gchar *sql
){
	PGresult *res = PQexec(conn, sql);
	ExecStatusType status = PQresultStatus(res);
	gchar *value = NULL;

	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
		g_test_message("test helper query failed: %s\n  %s", PQresultErrorMessage(res), sql);
		PQclear(res);
		g_assert_not_reached();
	}

	if (status == PGRES_TUPLES_OK && PQntuples(res) > 0 && PQnfields(res) > 0 &&
	    !PQgetisnull(res, 0, 0))
		value = g_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	return value;
}

static gchar *
build_url(
	AxisTestDb  *self,
	const gchar *dbname
){
	g_autofree gchar *user = g_uri_escape_string(self->user, NULL, FALSE);
	g_autofree gchar *password = g_uri_escape_string(self->password, NULL, FALSE);
	g_autofree gchar *db = g_uri_escape_string(dbname, NULL, FALSE);

	/* A host that is a socket directory goes in the query instead. */
	if (self->host[0] == '/') {
		g_autofree gchar *host = g_uri_escape_string(self->host, NULL, FALSE);

		return g_strdup_printf("postgresql://%s:%s@/%s?host=%s&port=%s",
			user, password, db, host, self->port);
	}

	return g_strdup_printf("postgresql://%s:%s@%s:%s/%s",
		user, password, self->host, self->port, db);
}

AxisTestDb *
axis_test_db_new(void)
{
	static gint counter = 0;
	AxisTestDb *self;
	const gchar *base = g_getenv("AXIS_TEST_DATABASE_URL");
	PQconninfoOption *options;
	PQconninfoOption *option;
	g_autofree gchar *create = NULL;
	gchar *drop;

	if (g_strcmp0(g_getenv("AXIS_SKIP_DB_TESTS"), "1") == 0) {
		g_test_skip("AXIS_SKIP_DB_TESTS=1");
		return NULL;
	}
	if (base == NULL || *base == '\0') {
		g_test_message(
			"This test needs PostgreSQL.  Run it through tests/with-postgres.sh\n"
			"(make test and make check do), set AXIS_TEST_DATABASE_URL to a server\n"
			"the tests may create and drop databases on, or set AXIS_SKIP_DB_TESTS=1\n"
			"to skip it.");
		g_test_fail();
		return NULL;
	}

	self = g_new0(AxisTestDb, 1);
	self->base_url = g_strdup(base);

	options = PQconninfoParse(base, NULL);
	g_assert_nonnull(options);
	for (option = options; option->keyword != NULL; option++) {
		const gchar *val = option->val;

		if (strcmp(option->keyword, "host") == 0)
			self->host = g_strdup(val != NULL ? val : "127.0.0.1");
		else if (strcmp(option->keyword, "port") == 0)
			self->port = g_strdup(val != NULL ? val : "5432");
		else if (strcmp(option->keyword, "user") == 0)
			self->user = g_strdup(val != NULL ? val : g_get_user_name());
		else if (strcmp(option->keyword, "password") == 0)
			self->password = g_strdup(val != NULL ? val : "");
		else if (strcmp(option->keyword, "dbname") == 0)
			self->admin_db = g_strdup(val != NULL ? val : "postgres");
	}
	PQconninfoFree(options);

	self->name = g_strdup_printf("axis_test_%d_%d_%08x",
		(int)getpid(), g_atomic_int_add(&counter, 1), g_random_int());
	self->url = build_url(self, self->name);

	self->admin = connect_or_fail(base);
	create = g_strdup_printf("CREATE DATABASE %s", self->name);
	drop = query(self->admin, create);
	g_free(drop);

	return self;
}

void
axis_test_db_free(AxisTestDb *self)
{
	g_autofree gchar *drop = NULL;
	gchar *out;

	if (self == NULL)
		return;

	axis_test_db_disconnect(self);

	/* WITH (FORCE) disconnects whatever the test left behind. */
	drop = g_strdup_printf("DROP DATABASE IF EXISTS %s WITH (FORCE)", self->name);
	out = query(self->admin, drop);
	g_free(out);
	PQfinish(self->admin);

	g_free(self->base_url);
	g_free(self->name);
	g_free(self->url);
	g_free(self->host);
	g_free(self->port);
	g_free(self->user);
	g_free(self->password);
	g_free(self->admin_db);
	g_free(self);
}

const gchar *
axis_test_db_get_url(AxisTestDb *self)
{
	return self->url;
}

const gchar *
axis_test_db_get_name(AxisTestDb *self)
{
	return self->name;
}

const gchar *
axis_test_db_get_password(AxisTestDb *self)
{
	return self->password;
}

gchar *
axis_test_db_url_for(
	AxisTestDb  *self,
	const gchar *dbname
){
	return build_url(self, dbname);
}

gchar *
axis_test_db_url_via(
	AxisTestDb  *self,
	guint16      port,
	const gchar *query
){
	g_autofree gchar *user = g_uri_escape_string(self->user, NULL, FALSE);
	g_autofree gchar *password = g_uri_escape_string(self->password, NULL, FALSE);

	return g_strdup_printf("postgresql://%s:%s@127.0.0.1:%u/%s%s%s",
		user, password, (guint)port, self->name,
		query != NULL ? "?" : "", query != NULL ? query : "");
}

const gchar *
axis_test_db_get_host(AxisTestDb *self)
{
	return self->host;
}

guint16
axis_test_db_get_port(AxisTestDb *self)
{
	return (guint16)g_ascii_strtoull(self->port, NULL, 10);
}

static PGconn *
test_conn(AxisTestDb *self)
{
	if (self->conn == NULL)
		self->conn = connect_or_fail(self->url);

	return self->conn;
}

void
axis_test_db_exec(
	AxisTestDb  *self,
	const gchar *sql
){
	g_free(query(test_conn(self), sql));
}

gchar *
axis_test_db_query(
	AxisTestDb  *self,
	const gchar *sql
){
	return query(test_conn(self), sql);
}

gchar *
axis_test_db_admin_query(
	AxisTestDb  *self,
	const gchar *sql
){
	return query(self->admin, sql);
}

void
axis_test_db_disconnect(AxisTestDb *self)
{
	if (self->conn != NULL) {
		PQfinish(self->conn);
		self->conn = NULL;
	}
}

/*
 * Log checks
 */

typedef struct
{
	GLogLevelFlags level;
	gchar         *message;
} Record;

typedef struct
{
	GLogLevelFlags  level;
	GPatternSpec   *pattern;
} Allowance;

static GMutex log_lock;
static GPtrArray *log_records;		/* of Record */
static GPtrArray *log_allowances;	/* of Allowance */

static void
record_free(gpointer data)
{
	Record *record = data;

	g_free(record->message);
	g_free(record);
}

static void
allowance_free(gpointer data)
{
	Allowance *allowance = data;

	g_pattern_spec_free(allowance->pattern);
	g_free(allowance);
}

static GLogLevelFlags
level_only(GLogLevelFlags flags)
{
	return flags & G_LOG_LEVEL_MASK;
}

static void
capture_handler(
	const gchar    *domain,
	GLogLevelFlags  level,
	const gchar    *message,
	gpointer        user_data
){
	Record *record = g_new0(Record, 1);

	record->level = level_only(level);
	record->message = g_strdup(message);

	g_mutex_lock(&log_lock);
	g_ptr_array_add(log_records, record);
	g_mutex_unlock(&log_lock);

	g_log_default_handler(domain, level, message, user_data);
}

/* Returns FALSE (do not abort) for an allowed message. */
static gboolean
fatal_handler(
	const gchar    *domain,
	GLogLevelFlags  level,
	const gchar    *message,
	gpointer        user_data
){
	gboolean fatal = TRUE;
	guint i;

	g_mutex_lock(&log_lock);
	for (i = 0; i < log_allowances->len; i++) {
		Allowance *allowance = g_ptr_array_index(log_allowances, i);

		if (allowance->level == level_only(level) &&
		    g_pattern_spec_match_string(allowance->pattern, message)) {
			fatal = FALSE;
			break;
		}
	}
	g_mutex_unlock(&log_lock);

	return fatal;
}

void
axis_test_log_init(void)
{
	if (log_records == NULL) {
		log_records = g_ptr_array_new_with_free_func(record_free);
		log_allowances = g_ptr_array_new_with_free_func(allowance_free);
		g_log_set_handler("axis", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
			capture_handler, NULL);
	}

	/* GLib clears the fatal handler after every test case. */
	g_test_log_set_fatal_handler(fatal_handler, NULL);
}

void
axis_test_log_allow(
	GLogLevelFlags  level,
	const gchar    *pattern
){
	Allowance *allowance = g_new0(Allowance, 1);

	allowance->level = level;
	allowance->pattern = g_pattern_spec_new(pattern);

	g_mutex_lock(&log_lock);
	g_ptr_array_add(log_allowances, allowance);
	g_mutex_unlock(&log_lock);
}

void
axis_test_log_reset(void)
{
	g_mutex_lock(&log_lock);
	g_ptr_array_set_size(log_records, 0);
	g_ptr_array_set_size(log_allowances, 0);
	g_mutex_unlock(&log_lock);
}

guint
axis_test_log_count(
	GLogLevelFlags  level,
	const gchar    *pattern
){
	GPatternSpec *spec = g_pattern_spec_new(pattern);
	guint count = 0;
	guint i;

	g_mutex_lock(&log_lock);
	for (i = 0; i < log_records->len; i++) {
		Record *record = g_ptr_array_index(log_records, i);

		if (record->level == level && g_pattern_spec_match_string(spec, record->message))
			count++;
	}
	g_mutex_unlock(&log_lock);

	g_pattern_spec_free(spec);
	return count;
}

gchar *
axis_test_log_find(
	GLogLevelFlags  level,
	const gchar    *pattern
){
	GPatternSpec *spec = g_pattern_spec_new(pattern);
	gchar *found = NULL;
	guint i;

	g_mutex_lock(&log_lock);
	for (i = log_records->len; i > 0; i--) {
		Record *record = g_ptr_array_index(log_records, i - 1);

		if (record->level == level && g_pattern_spec_match_string(spec, record->message)) {
			found = g_strdup(record->message);
			break;
		}
	}
	g_mutex_unlock(&log_lock);

	g_pattern_spec_free(spec);
	return found;
}

/* Whether 127.0.0.1:@port (0: any, which is put in @port) can be bound now. */
static gboolean
port_is_free(guint16 *port)
{
	struct sockaddr_in sin;
	socklen_t length = sizeof sin;
	gboolean free;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return FALSE;
	memset(&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(*port);
	free = bind(fd, (struct sockaddr *)&sin, sizeof sin) == 0
		&& getsockname(fd, (struct sockaddr *)&sin, &length) == 0;
	if (free)
		*port = ntohs(sin.sin_port);
	close(fd);

	return free;
}

guint16
axis_test_free_port(void)
{
	guint16 port = 0;
	guint tries;

	for (tries = 0; tries < 200; tries++) {
		port = (guint16)g_random_int_range(20000, 32000);
		if (port_is_free(&port))
			return port;
	}

	/* Nothing free there, 200 times over: whatever the kernel gives. */
	port = 0;
	g_assert_true(port_is_free(&port));
	return port;
}
