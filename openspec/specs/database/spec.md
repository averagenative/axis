# database Specification

## Purpose
How Axis reaches PostgreSQL: the connection pool, query safety, recovery from dropped connections, and the rules for schema migrations.
## Requirements
### Requirement: Connection pool
The database layer SHALL provide a pool with a fixed number of connections, and a caller that requests a connection when none is free SHALL wait until one is released or a timeout expires.

#### Scenario: Connections are reused
- **WHEN** a caller acquires a connection, releases it, and acquires again
- **THEN** no new server connection is opened for the second acquire

#### Scenario: Waiter is served on release
- **WHEN** all connections are in use and another caller is waiting
- **THEN** the waiting caller receives a connection as soon as one is released

#### Scenario: Acquire times out
- **WHEN** all connections remain in use for longer than the acquire timeout
- **THEN** the waiting caller receives an error and no connection

#### Scenario: Unreachable database is reported
- **WHEN** the pool is created with a URL for a server that is not accepting connections
- **THEN** creation fails with an error that does not contain the password

### Requirement: Parameterised queries
The database layer SHALL send query parameters separately from the SQL text, so that a parameter value can never change the structure of a statement.

#### Scenario: Hostile parameter is stored verbatim
- **WHEN** a value containing quotes, semicolons and SQL keywords is passed as a parameter to an insert
- **THEN** the stored value equals the input exactly and no other statement runs

#### Scenario: NULL parameters and results
- **WHEN** a NULL parameter is inserted and read back
- **THEN** the result reports the field as NULL, distinct from an empty string

### Requirement: Transactions
The database layer SHALL provide transactions that either commit all their statements or none.

#### Scenario: Rollback discards changes
- **WHEN** a transaction inserts a row and is rolled back
- **THEN** the row is not present afterwards

#### Scenario: Released connection never carries an open transaction
- **WHEN** a connection is released while a transaction is open
- **THEN** the transaction is rolled back before the connection can be acquired again

### Requirement: Recovery from a dropped connection
The database layer SHALL recover when the server closes a pooled connection, without restarting the daemon.

#### Scenario: Connection killed while idle
- **WHEN** the server terminates an idle pooled connection and a caller then acquires it
- **THEN** the caller's query succeeds on a re-established connection

#### Scenario: Connection killed mid-query
- **WHEN** the server terminates a connection while a query is running on it
- **THEN** that query returns an error, and the next acquire yields a working connection

### Requirement: Ordered, atomic migrations
The migration runner SHALL apply pending migrations in ascending version order, each atomically together with its bookkeeping record.

#### Scenario: Empty database is brought up to date
- **WHEN** the runner runs against an empty database
- **THEN** every compiled-in migration is applied in order and recorded with its version, name and checksum

#### Scenario: Running again changes nothing
- **WHEN** the runner runs against an up-to-date database
- **THEN** no migration is applied and it reports success

#### Scenario: Failing migration leaves no trace
- **WHEN** a migration fails part-way through
- **THEN** none of its statements take effect, it is not recorded, earlier migrations remain applied, and the runner reports an error naming the migration

### Requirement: Concurrent migration safety
The migration runner SHALL ensure that when two processes start against the same database, each migration is applied exactly once.

#### Scenario: Two runners at once
- **WHEN** two runners start at the same time against an empty database
- **THEN** both report success and each migration is recorded exactly once

### Requirement: Schema history is verified
The migration runner SHALL refuse to proceed when the database's recorded history is inconsistent with the migrations compiled into the binary.

#### Scenario: Applied migration was edited
- **WHEN** a recorded migration's checksum differs from the compiled-in migration of the same version
- **THEN** the runner applies nothing and reports an error naming that migration

#### Scenario: Database is newer than the binary
- **WHEN** the database records a migration version the binary does not contain
- **THEN** the runner applies nothing and reports that the database was written by a newer version

### Requirement: Minimum server version
The database layer SHALL refuse to use a PostgreSQL server older than version 15.

#### Scenario: Old server
- **WHEN** the server reports a version below 15
- **THEN** pool creation fails with an error stating the found and required versions

### Requirement: One daemon per database
Only one daemon SHALL serve a database at a time. A daemon SHALL hold the database with a session advisory lock on a connection of its own, taken after connecting and before applying any migration, and held for its life; the lock's session SHALL carry a name that says which daemon holds it (instance, version, process and host). A daemon that finds the lock held SHALL serve only the probes, SHALL answer `/readyz` and `/healthz` with 503 and the status `held by another daemon` and every other path with 503, SHALL apply no migration and run no job, SHALL log who holds the lock (the holding session's name, address and connection time, where the server gives them), and SHALL take the database once the lock is free, within `database.wait`. With `database.wait` 0 it SHALL exit at once with a non-zero status and a message naming the holder.

#### Scenario: A second daemon starts
- **WHEN** a daemon starts against a database that another running daemon holds
- **THEN** it is alive, `/readyz` answers 503 with `{"status":"held by another daemon"}`, every other path answers 503, its log names the running daemon's process, and it applies no migration and claims no job

#### Scenario: The first daemon stops
- **WHEN** the daemon that holds the database stops while a second one waits
- **THEN** the second takes the lock within a few seconds, serves, becomes ready and runs jobs

#### Scenario: No wait
- **WHEN** a second daemon starts with `database.wait` 0
- **THEN** it exits at once with a non-zero status and a message naming the daemon that holds the database

### Requirement: A daemon that loses its lock stops working
A daemon SHALL watch its lock's connection and send a check every 5 seconds, counted from the previous check's sending, asking whether the session behind the connection (by its process id and start) still holds the lock. A check that is not answered SHALL NOT by itself count as a loss: the connection SHALL stay open, and the daemon SHALL ask on another connection. The daemon SHALL count the lock as lost when the session ends, when a question is answered no, or when no question sent in the last 20 seconds has been answered yes. A daemon whose lock is lost SHALL at once stop starting work: every path but the probes and the metrics SHALL answer 503, `/readyz` SHALL answer 503 with the status `database lock lost`, no job SHALL be claimed, no lease renewed and no schedule fired, and every running job handler SHALL be told to stop as for a shutdown, so that a handler that stops hands its job back. A handler that has not returned 25 seconds after the loss SHALL get the process ended at once with status 3. The daemon SHALL then take the lock again: held again, it SHALL resume without a restart; held by another session, it SHALL stop with status 1; held by an earlier session of its own, it SHALL try again.

#### Scenario: The lock's connection is lost
- **WHEN** the session holding a running daemon's lock is ended, and new connections are refused for a while
- **THEN** the daemon answers `/readyz` with `{"status":"database lock lost"}`, serves no page and claims no job, and once it holds the lock again it is ready and runs jobs, without a restart

#### Scenario: The lock is taken by another while it is lost
- **WHEN** another session takes the lock between the loss and the daemon's attempt to take it again
- **THEN** the daemon stops with status 1, saying which session took it, and a daemon that was waiting takes over once that session lets go

#### Scenario: A handler that does not stop
- **WHEN** a job handler is still running 25 seconds after the daemon's lock was lost
- **THEN** the process ends with status 3, and the lock is not taken again by it

#### Scenario: A slow server
- **WHEN** the lock's connection does not answer for several seconds, and another connection confirms that the lock's session still holds the lock
- **THEN** the lock is not counted as lost, and the daemon goes on serving and running jobs

#### Scenario: Silence that lasts
- **WHEN** neither the lock's connection nor any new connection gets an answer
- **THEN** the lock is counted as lost no later than 20 seconds after the last question that was answered yes was sent, and taken again once the server answers

#### Scenario: An earlier session of its own
- **WHEN** the daemon takes its lock again and a session of its own name still holds it after the earlier one was ended
- **THEN** the daemon tries again later and does not stop as if another daemon held the lock

