# Run the checks in parallel

## Why

`make check` took 73 minutes and `make fuzz FUZZ_SECONDS=20` took 20 minutes on a machine with 16 processors, because everything ran one at a time: about 700 objects compiled one after another, 138 test programs ran one after another under the sanitizers, and 57 fuzz targets ran for 20 seconds each in a loop. A gate that takes an hour and a half is run less often than it should be, and a failure in the fiftieth minute hides the ones behind it, because the run stopped there.

## What changes

- `make test`, and so `make check`, runs the test programs `TEST_JOBS` at a time. `make fuzz` runs the fuzz targets `FUZZ_JOBS` at a time. Both default to the number of processors, and to 16 at most.
- Every program runs, whatever the others did. Each writes to a log of its own under the build directory. The terminal shows one line as each program ends, with the time it took. At the end come the whole log of each program that failed, the ten slowest programs, and the names of those that failed. The run exits non-zero when any program failed.
- `TEST_JOBS=1` and `FUZZ_JOBS=1` run the programs one after another on the terminal and stop at the first failure, as before.
- A fuzz target still runs for `FUZZ_SECONDS`, with its own corpus directory and artifact names. A crash still fails the run, and the run names the target.
- The throwaway PostgreSQL of the tests keeps its data in memory, and takes 1000 connections instead of 200. `AXIS_TEST_PG_ON_DISK=1` keeps the data on disk; `make check-scale` does.
- `make -jN check` is documented as the way to run the gate. `make test-build` builds what `make test` runs and runs nothing.
- The image build runs its tests one after another (`TEST_JOBS=1`), as before.

## What was measured

On a machine with 16 processors and 32 GB of memory, with `make -j16` and the default of 16 jobs:

| | Before | After |
| --- | --- | --- |
| `make check`, from an empty build directory | 73 min | 8 min 44 s |
| of that, the test programs | about an hour | 5 min 22 s |
| `make fuzz FUZZ_SECONDS=20`, targets already compiled | 20 min | 1 min 25 s |

Three runs of `make -j16 check` in a row passed, with the test programs taking 5 min 22 s, 5 min 18 s and 5 min 17 s; with the fuzz targets compiled already, the whole of `make -j16 check` took 5 min 54 s and 5 min 27 s. The compile of the tests takes 28 seconds with `-j16`. The fuzz run used 18 min 41 s of processor time for 57 targets of 20 seconds each, so each target had a processor for its whole time.

The slowest test program, `test-conversion` (5 min 22 s among the others), sets the time of the tests. Splitting it is the next step for anyone who wants less.

## What stood in the way

- **The database on disk.** With 16 test programs at once, each making, filling and dropping databases, the PostgreSQL container wrote about 100 MB a second. The processors waited for the disk 30 to 40 percent of the time, the run took nine minutes, and 11 to 13 programs failed on one deadline or another, different ones in each run. With the data in a tmpfs the processors are busy, the tests take five minutes, and those failures are gone. The data directory was 300 to 650 MB during a run.
- **A pipe that filled.** `test-daemon` sent the daemon's standard error to a pipe that it read only when the daemon had ended. Linux gives a pipe 64 KiB, and only two pages once one user's programs hold many pipes, as when 16 test programs run at once. The daemon's start-up log filled the pipe, the daemon stood still in `write()`, and `/healthz` was not answered. It failed in every parallel run, in a different test each time. The log now goes to a file.
- **Ports.** Five test programs found a free port by binding port 0 and closing it, a port the kernel may give to another program a moment later. They now use one helper, `axis_test_free_port()`, which picks below the kernel's ephemeral range, as `test-daemon` already did for itself.

No test program needed the list of programs that run alone (`TEST_SERIAL`); the list is there, empty, for the day one does.

## Why `xargs -P`, and why 16

The programs are run by a small script, `tests/run-parallel.sh`, with `xargs -P`. The other way was one make target per program under make's own `-j`. The script keeps the number of test programs apart from the number of compilers, gives the summary at the end in one place, and is 140 lines of sh. `xargs -P` is in GNU findutils and in the BSDs.

One job per processor, because a fuzz target is one busy thread, and a test program with its database sessions and the daemons it starts is about one too: with 16 jobs the 16 processors were about 90 percent busy. No more than 16, because each test program runs under the sanitizers and all of them share one PostgreSQL: a machine with 64 processors does not always have the memory for 64.

## Impact

- Capabilities: `deployment` (a requirement on how the checks run).
- Build: `Makefile`, `config.mk`, `tests/run-parallel.sh` (new), `tests/fuzz/run-target.sh` (new), `tests/with-postgres.sh`, `deploy/Containerfile`.
- Tests: `tests/axis-test-db.c` and `.h` (the port helper), `tests/test-daemon.c`, `tests/test-daemon-lock.c`, `tests/test-daemon-stop.c`, `tests/test-worker-daemon.c`, `tests/test-web-limits.c`, `tests/test-db-unit.c`.
- Documents: `README.md`, `openspec/project.md`.
- Behavior that differs: `make check` no longer stops at the first failing test program unless `TEST_JOBS=1`; a test program's output is in its log, not on the terminal; the test database uses memory, about 1 GB, instead of disk.
- Not changed: nothing under `deps/`; no program of Axis itself.
