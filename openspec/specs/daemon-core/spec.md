# daemon-core Specification

## Purpose
How the `axisd` daemon starts, is configured, logs, reports its health, registers modules, and shuts down.
## Requirements
### Requirement: Configuration file
The daemon SHALL read its bootstrap settings (listen host, listen port, database URL, log level) from an INI-style configuration file whose path is given with `--config`, and SHALL apply documented defaults for settings that are absent.

#### Scenario: Defaults apply when no file is given
- **WHEN** the daemon is started without `--config`
- **THEN** it listens on 127.0.0.1 port 7800 with log level `info`

#### Scenario: File values are used
- **WHEN** the configuration file sets the listen port to 9000
- **THEN** the daemon listens on port 9000

#### Scenario: Unknown key is rejected
- **WHEN** the configuration file contains a group or key the daemon does not recognise
- **THEN** the daemon exits with a non-zero status and an error naming the group and key

#### Scenario: Invalid value is rejected
- **WHEN** the configuration file sets the listen port to a value outside 1 to 65535, or the log level to an unknown name
- **THEN** the daemon exits with a non-zero status and an error naming the setting

#### Scenario: Missing file is an error
- **WHEN** `--config` names a file that does not exist
- **THEN** the daemon exits with a non-zero status and an error naming the path

### Requirement: Environment overrides
The daemon SHALL let the environment variables `AXIS_LISTEN_HOST`, `AXIS_LISTEN_PORT`, `AXIS_DATABASE_URL` and `AXIS_LOG_LEVEL` override the corresponding file settings, validated by the same rules.

#### Scenario: Environment wins over file
- **WHEN** the file sets the listen port to 9000 and `AXIS_LISTEN_PORT` is 9100
- **THEN** the daemon listens on port 9100

#### Scenario: Invalid environment value is rejected
- **WHEN** `AXIS_LISTEN_PORT` is not a valid port number
- **THEN** the daemon exits with a non-zero status and an error naming the variable

### Requirement: Secrets are not logged
The daemon SHALL NOT write the database URL's password to its log output.

#### Scenario: Database URL is redacted
- **WHEN** the daemon logs its effective configuration and the database URL contains a password
- **THEN** the logged URL has the password replaced with a placeholder

### Requirement: Log level filtering
The daemon SHALL write log messages to standard error, one per line, and SHALL suppress messages below the configured level (`debug`, `info`, `warning`, `error`).

#### Scenario: Debug messages hidden at info
- **WHEN** the log level is `info` and a debug message is emitted
- **THEN** nothing is written for that message

### Requirement: Health endpoint
The daemon SHALL answer `GET /healthz` with a JSON body whose `status` is `ok` and HTTP status 200 when the database is reachable, and `degraded` and HTTP status 503 when it is not. To a request with a session, or carrying the metrics token as a bearer token, the body SHALL also contain the daemon `version` and `database` (`ok` or `unreachable`); to any other request it SHALL contain `status` and nothing else. The right token SHALL always be answered in full; a bearer token of the metrics token's form that is not it SHALL count against the client address as at `/metrics`; any other `Authorization` header SHALL be ignored, neither counted nor logged. To a client on a loopback address, every probe's answer (`/livez`, `/readyz`, `/healthz`) SHALL name the answering process in an `X-Axis-Process` header. The check SHALL NOT block the main loop, and the probes' database questions SHALL be shared: at most one of each kind (the health ping, the readiness schema check) SHALL be asked at a time, every probe that arrives meanwhile SHALL wait for its answer, and an answer SHALL be reused for one second.

#### Scenario: Health check succeeds
- **WHEN** a client requests `GET /healthz` from a running daemon whose database is reachable
- **THEN** the response status is 200, the content type is `application/json`, and the body is `{"status":"ok"}`

#### Scenario: Database goes away
- **WHEN** the database becomes unreachable while the daemon is running and a client requests `GET /healthz`
- **THEN** the response status is 503 and the body is `{"status":"degraded"}`

#### Scenario: The version to those who may know it
- **WHEN** a client with a session, or with the metrics token, requests `GET /healthz`
- **THEN** the body contains `"status":"ok"`, `"database":"ok"` and the daemon's `version`, and a client with a wrong token gets the status word only

#### Scenario: Another Authorization header is ignored
- **WHEN** twenty requests to `/healthz` carry a Basic `Authorization` header, then eleven a wrong bearer token of the metrics token's form, then one the right token
- **THEN** the first twenty get the status word, nothing about a token is logged, and the last gets the version although its address is delayed

#### Scenario: The process that answered
- **WHEN** a test starts a daemon and asks its probes from the loopback address
- **THEN** each answer names the daemon's process id, so an answer from another daemon on a reused port is told apart

#### Scenario: A flood of probes asks once
- **WHEN** twenty probes of `/healthz` arrive while the database is slow to answer
- **THEN** the database is asked once, all twenty get its answer, probes within the next second are answered from it, and the readiness check is a question of its own

### Requirement: Clean shutdown
The daemon SHALL stop accepting connections, shut down its modules in reverse order of registration, and exit with status 0 when it receives SIGINT or SIGTERM.

#### Scenario: SIGTERM stops the daemon
- **WHEN** a running daemon receives SIGTERM
- **THEN** it exits with status 0

#### Scenario: Modules shut down in reverse order
- **WHEN** modules A then B are registered and started, and the daemon shuts down
- **THEN** B's shutdown runs before A's

### Requirement: Listen failure is fatal
The daemon SHALL exit with a non-zero status and an error naming the address if it cannot bind its listen address.

#### Scenario: Port already in use
- **WHEN** the configured port is already bound by another process
- **THEN** the daemon exits with a non-zero status and an error naming the host and port

### Requirement: Module registration
The daemon SHALL keep a registry of modules, each with a unique identifier, and SHALL refuse to register a second module with an identifier already in use.

#### Scenario: Duplicate identifier refused
- **WHEN** a module is registered with an identifier that is already registered
- **THEN** registration fails with an error and the first module remains registered

#### Scenario: Failed module start aborts start-up
- **WHEN** a module's start-up reports an error
- **THEN** the modules started before it are shut down in reverse order and the error is returned to the caller

### Requirement: Database is required at start-up
The daemon SHALL require a database URL, connect to the database, and apply pending migrations before it serves any page or API; until then it SHALL answer only its liveness and readiness probes (alive, not ready) and refuse every other request with 503. It SHALL keep trying to connect, with back-off, for as long as `database.wait` (`AXIS_DATABASE_WAIT`, default forever) allows, and SHALL give up at once on a URL that cannot be read or a server that is too old. If it gives up, or a migration fails, it SHALL exit with a non-zero status and an error that does not reveal the password.

#### Scenario: No database URL
- **WHEN** the daemon is started with no database URL in the file or environment
- **THEN** it exits with a non-zero status and an error saying the database URL is required

#### Scenario: Database unreachable
- **WHEN** the database URL points at a server that is not accepting connections and the wait is 0
- **THEN** the daemon exits with a non-zero status, and never serves a page

#### Scenario: Database comes up later
- **WHEN** the database URL points at a server that starts accepting connections only after the daemon has started
- **THEN** the daemon answers its liveness probe throughout, is not ready until the database is, and then serves without a restart

#### Scenario: Migrations applied before serving
- **WHEN** the daemon starts against an empty database
- **THEN** all migrations are applied before the first page is served

#### Scenario: Stopped while waiting
- **WHEN** the daemon is asked to stop while it waits for its database
- **THEN** it exits with status 0 without waiting further

### Requirement: Pool size setting
The daemon SHALL accept a `database.pool_size` setting (and `AXIS_DATABASE_POOL_SIZE`) between 1 and 64. When it is not set, the pool SHALL be what the job queue needs (a connection for each worker thread of the in-process queues at their configured concurrency, and the queue's own two) and 4 more for the pages, the health checks and the worker API, never fewer than 10, so that a build with another queue never stops a setup that set none. When it is set smaller than the job queue needs, the daemon SHALL stop before it changes anything in the database, naming the number of connections it needs.

#### Scenario: Out-of-range pool size
- **WHEN** the pool size is set to 0 or 65
- **THEN** the daemon exits with a non-zero status and an error naming the setting

#### Scenario: Default is enough for the default queues
- **WHEN** the daemon starts with no pool size and no queue concurrency configured
- **THEN** the job queue starts, every in-process queue with one worker

#### Scenario: A pool not set fits the queues
- **WHEN** the daemon starts with no pool size and two queues at a concurrency of 8
- **THEN** it starts, with a pool of the queue's need and 4

#### Scenario: A pool set too small changes nothing
- **WHEN** the pool size is set one short of what the job queue needs
- **THEN** the daemon exits with a non-zero status, the error names the number needed, and no migration was applied

### Requirement: Proxy and cookie settings
The daemon SHALL accept `server.trusted_proxies` (a list of IP addresses, CIDR ranges and host names, default empty) and `server.secure_cookies` (`auto` or `always`, default `auto`), with matching `AXIS_TRUSTED_PROXIES` and `AXIS_SECURE_COOKIES` overrides. A host name SHALL be dot-separated labels of ASCII letters, digits, `-` and `_` (each 1 to 63 characters, none starting or ending with `-`), at most 253 characters, whose last label is not all digits; it SHALL be kept in lower case.

#### Scenario: Invalid proxy entry
- **WHEN** `server.trusted_proxies` contains a value that is not an IP address, a CIDR range or a host name
- **THEN** the daemon exits with a non-zero status and an error naming the setting and the value

#### Scenario: Malformed name
- **WHEN** `server.trusted_proxies` contains `-proxy`, `proxy..example`, `traefik:8080` or `300.1.1.1`
- **THEN** the daemon exits with a non-zero status and an error naming the setting and that value

#### Scenario: Names beside addresses
- **WHEN** `server.trusted_proxies` is ` 10.0.0.0/8 , Traefik, 2001:DB8::1`
- **THEN** the list in effect is `10.0.0.0/8, traefik, 2001:db8::1`

#### Scenario: Effective list is logged
- **WHEN** the daemon starts with trusted proxies configured
- **THEN** it logs the effective list at info level

### Requirement: Modules describe their settings and dependencies
A module description SHALL be able to carry its setting declarations, whether the module is required, whether it is enabled by default, and the ids of modules it depends on.

#### Scenario: Defaults for older descriptions
- **WHEN** a module registers without settings, dependencies or flags
- **THEN** it registers successfully, has no settings, depends on nothing, is not required and is enabled by default

### Requirement: Health-check mode
The daemon binary SHALL offer a mode that checks a running daemon's health endpoint using the same configuration and exits 0 when healthy and non-zero otherwise, without opening database connections or starting jobs.

#### Scenario: Healthy
- **WHEN** health-check mode runs while the daemon is healthy
- **THEN** it exits 0

#### Scenario: Unhealthy or absent
- **WHEN** health-check mode runs while the daemon reports unhealthy or is not listening
- **THEN** it exits non-zero within five seconds

### Requirement: Database URL from a file
The daemon SHALL accept the database URL from a file named by `AXIS_DATABASE_URL_FILE`, SHALL refuse to start when both that and `AXIS_DATABASE_URL` are set, and SHALL NOT log the file's content.

#### Scenario: Read from file
- **WHEN** the variable names a readable file containing a URL followed by a newline
- **THEN** the daemon connects using that URL

#### Scenario: Both set
- **WHEN** both variables are set
- **THEN** the daemon exits with an error naming both variables

#### Scenario: Unreadable file
- **WHEN** the named file cannot be read
- **THEN** the daemon exits with an error naming the path and not its content

### Requirement: Navigation entries carry a group
A module's navigation entry SHALL be able to state the group it belongs to and whether it is for administrators only. An entry that states no group SHALL be placed in the system group.

#### Scenario: Default group
- **WHEN** a module registers an entry without a group
- **THEN** it appears in the system group

### Requirement: Data directory
The daemon SHALL keep files of its own only beneath a configured data directory, SHALL create it if absent, and SHALL refuse to start when it cannot write there.

#### Scenario: Created
- **WHEN** the configured directory does not exist and its parent is writable
- **THEN** the daemon creates it and starts

#### Scenario: Not writable
- **WHEN** the daemon cannot write in the configured directory
- **THEN** it exits with an error naming the directory

### Requirement: Request limits from the open port
From the moment the daemon listens, in every state (waiting for its database, standing by for another daemon's lock, serving, and having lost its lock), it SHALL limit what one request and one client may cost.

Bodies: while the daemon serves, a request body SHALL be limited to 1 MiB (the worker API's to its own limit), and one too large or of unknown length answered 413 before it is kept. While the daemon does not serve, a request with a body SHALL be answered what the start-up gate would say (503, `Retry-After`, a short explanation) without any of the body being read.

Deadlines: a request's head SHALL be limited to `server.head_timeout` (default 30 s) from the start of the connection or, on a kept-alive connection, from the request's first byte; its body to `server.body_timeout` (60 s) after its head; and a kept-alive connection's wait for its next request to `server.idle_timeout` (60 s), except a connection whose last request carried a worker's valid token. A connection that misses a deadline SHALL be closed. An event stream SHALL NOT be cut by these once its head has arrived.

Connections: at most `server.max_connections` (512) connections SHALL be open at once, and at most `server.max_connections_per_address` (64) from one public address (an IPv6 address counted by its `/64`). A loopback, private, unique-local or link-local address SHALL be held to the total only. A request that carries a worker's valid token SHALL NOT be held to the per-address cap. Up to 32 connections over a cap SHALL be let in at once, each with 2 seconds for its head: a health probe without a body from a loopback or private address SHALL be served, and anything else answered 503 with `Retry-After` and a short explanation, without its body being read. A connection beyond those SHALL be closed before anything is read from it. A refusal SHALL be logged at most once a minute, and the Settings and Reverse proxies pages SHALL say how many were refused, the last address and why, and that a reverse proxy at that address belongs in the trusted proxies.

Behind a trusted proxy: the proxy SHALL NOT be held to the per-address cap; each client it forwards, by the trusted-proxy rule, SHALL have at most that many requests in flight, not counting event streams and requests with a worker's valid token, and more SHALL be answered 429 with a retry time.

#### Scenario: A body while the daemon waits for its database
- **WHEN** the daemon waits for a database that does not answer, and a client promises a 96 MiB body and sends 64 KiB of it, and another sends a chunked body
- **THEN** each is answered 503 with `Retry-After` and the start-up page's words within seconds, long before the body deadline, and the rest of the body is not read

#### Scenario: Bodies refused, then accepted
- **WHEN** a shell refuses bodies and is then told to serve
- **THEN** a request with a body is answered 503 with `Retry-After` and the shell's security headers before, a request without one is served before and after, and a body is taken after

#### Scenario: A head that never ends
- **WHEN** a client sends part of a request head and then nothing, and another connection sends nothing at all
- **THEN** both are counted from the moment they open and closed once the head deadline passes

#### Scenario: A head in time
- **WHEN** a client sends its head in two parts within the head deadline
- **THEN** the request is answered

#### Scenario: A body that stops
- **WHEN** a request's head says 100 bytes of body and three arrive
- **THEN** the connection is closed once the body deadline passes

#### Scenario: An idle kept-alive connection
- **WHEN** a client asks twice on one connection and then nothing more
- **THEN** both are answered and the connection is closed once the idle limit passes

#### Scenario: An event stream outlives the deadlines
- **WHEN** an event stream stays open for longer than every deadline
- **THEN** it is not closed, and an event published then reaches it

#### Scenario: Per-address cap
- **WHEN** a public address holds as many connections as it may and opens one more
- **THEN** the new one is answered 503 with `Retry-After`, the others are not touched, the forwarding hint names the refusal and `server.trusted_proxies`, and once one ends a new one is served

#### Scenario: A local address is held to the total only
- **WHEN** a loopback address holds twice as many connections as one address may and opens one more
- **THEN** it is served and nothing is refused

#### Scenario: Total cap
- **WHEN** the daemon holds as many connections as it takes and more arrive
- **THEN** a page is answered 503 with `Retry-After`, a liveness probe from the loopback address is answered 200, a connection let in over the cap that sends nothing is closed after its 2 seconds, and one beyond those let in is closed at once

#### Scenario: Behind a trusted proxy
- **WHEN** a trusted proxy holds more connections than one address may, and one client it forwards has as many requests in flight as it may
- **THEN** every connection of the proxy is taken, the client's next request is answered 429 with a retry time, and another client's requests and the client's earlier ones are not touched

#### Scenario: Streams and workers behind a trusted proxy
- **WHEN** a client forwarded by a trusted proxy has as many requests in flight as it may, and then opens an event stream and sends a worker's request with a valid token
- **THEN** both are served and nothing is refused

#### Scenario: A worker is exempt
- **WHEN** a public address holds as many connections as it may, and a worker with a valid token sends a request from it
- **THEN** the request is served, and its kept-alive connection is not closed when the idle limit passes

### Requirement: Wrong metrics tokens are throttled
Each request to `/metrics` carrying a bearer token of the metrics token's form (24 to 256 printable characters) that is not the metrics token SHALL count against the client address (as the trusted-proxy rule decides it, an IPv6 address by its `/64`); after ten, the address's wrong tokens SHALL be refused with 429 and a retry time for a delay that doubles from 2 seconds up to 15 minutes, while other addresses are not affected. The right token SHALL always be answered. Another `Authorization` header SHALL be answered 401 and not counted. Wrong tokens SHALL be logged at most once a minute, with their count.

#### Scenario: Throttled
- **WHEN** eleven wrong tokens are presented from one address
- **THEN** the next wrong token from that address is answered 429 with a retry time, the right token is still answered 200, and one log line says so

#### Scenario: Not a token
- **WHEN** twenty requests carry a Basic `Authorization` header, and one a bearer token too short to be the metrics token
- **THEN** each is answered 401 and none counts toward the delay

