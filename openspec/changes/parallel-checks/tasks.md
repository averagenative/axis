# Tasks

## 1. The test programs, several at a time

- [x] 1.1 `tests/run-parallel.sh`: run programs `JOBS` at a time with `xargs -P`, a log and a status file for each, one line on the terminal as each ends with its time, every program run whatever the others did, the logs of the failed ones at the end, the ten slowest, the names of the failed, exit 1 when any failed; a `--serial` group that runs one at a time afterwards
- [x] 1.2 `TEST_JOBS` in `config.mk`, by default the number of processors and at most 16; `make test` runs the programs through `tests/run-parallel.sh`, and with `TEST_JOBS=1` through the loop it had
- [x] 1.3 `TEST_SERIAL` in the Makefile: the programs that run alone after the others, each with its reason (empty: none needed it)
- [x] 1.4 `make test-build`: everything `make test` runs, built and not run
- [x] 1.5 The image build runs its tests with `TEST_JOBS=1`, as before

## 2. The fuzz targets, several at a time

- [x] 2.1 `tests/fuzz/run-target.sh`: one target for `FUZZ_SECONDS`, with its dictionary, its corpus under `build/fuzz/corpus` and its artifacts under `build/fuzz/artifacts`, as the Makefile's loop ran it
- [x] 2.2 `FUZZ_JOBS` in `config.mk`; `make fuzz` runs the targets through `tests/run-parallel.sh`, and with `FUZZ_JOBS=1` one after another on the terminal

## 3. Nothing that only holds when a program runs alone

- [x] 3.1 The throwaway PostgreSQL takes 1000 connections, and keeps its data in memory (`AXIS_TEST_PG_ON_DISK=1` for the disk; `make check-scale` uses the disk)
- [x] 3.2 `axis_test_free_port()` in `tests/axis-test-db.c`: a free port below the kernel's ephemeral range; `test-daemon`, `test-daemon-lock`, `test-daemon-stop`, `test-worker-daemon`, `test-web-limits` and `test-db-unit` take their ports from it instead of binding port 0 and closing it
- [x] 3.3 `test-daemon`: the daemon's standard error goes to a file in the test's scratch directory, not to a pipe that is read only when the daemon has ended
- [x] 3.4 The tests read for fixed paths, fixed ports and statements that reach beyond the test's own database: none found besides the above

## 4. The compile

- [x] 4.1 `make -j16 check` from an empty build directory: the generated headers and htmx-glib before their users, htmx-glib built once
- [x] 4.2 `make -jN check fuzz` in one command: the fuzz targets wait for `check`, which compiles them itself

## 5. Documents

- [x] 5.1 The Makefile's header: the three numbers and the command
- [x] 5.2 `README.md` and `openspec/project.md`: how to run the two gates in parallel, the logs, the database in memory
- [x] 5.3 The spec delta for `deployment`

## 6. Verification

- [x] 6.1 `make -j16 check` three times with 16 jobs, all passing, and `make -j16 fuzz FUZZ_SECONDS=20`
- [x] 6.2 The run of old, `TEST_JOBS=1`, as the image build runs it: `make -j16 test TEST_JOBS=1 RELEASE=1 AXIS_SKIP_DB_TESTS=1`
- [x] 6.3 `tests/run-parallel.sh` with stand-in programs, one of which fails: the others run, the failed one's log and name come at the end, exit 1
- [ ] 6.4 Not done: a whole `make check TEST_JOBS=1` against the database (about an hour), and a fuzz target made to crash for real (a crash is libFuzzer's non-zero exit, which the runner reports as any failure)
