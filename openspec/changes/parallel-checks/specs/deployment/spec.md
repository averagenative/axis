## ADDED Requirements

### Requirement: The checks run several at a time
`make test` (and so `make check`) SHALL run the test programs `TEST_JOBS` at a time against the one throwaway PostgreSQL, and `make fuzz` SHALL run the fuzz targets `FUZZ_JOBS` at a time, each for `FUZZ_SECONDS` with its own corpus and artifact names. Both numbers SHALL default to the number of processors, at most 16. Every program SHALL be run whatever the others did, with its output in a log of its own under the build directory; the terminal SHALL get one line per program with the time it took, then the whole log of each program that failed, the ten that took longest and the names of those that failed, and the run SHALL exit non-zero when any failed. A value of 1 SHALL run the programs one after another with their output on the terminal, stopping at the first failure. A test program SHALL NOT depend on running alone: it makes its own database and scratch directories and listens only on ports it finds free; a program that cannot hold on a busy machine SHALL be named, with the reason, in the Makefile's list of programs that run one at a time after the others. The compile SHALL be correct under `make -jN`, and the Makefile's header SHALL say how to run all three in parallel.

#### Scenario: A test fails among many
- **WHEN** `make check` runs with `TEST_JOBS=16` and one test program fails
- **THEN** every other program still runs, the failed program's whole log is printed at the end with its name, and `make check` exits non-zero

#### Scenario: A fuzz target crashes
- **WHEN** `make fuzz` runs with `FUZZ_JOBS=16` and one target finds a crash
- **THEN** the crash is saved under `build/fuzz/artifacts` with the target's name, the run names the target as failed and exits non-zero, and the other targets have had their `FUZZ_SECONDS`

#### Scenario: One at a time
- **WHEN** `make check TEST_JOBS=1` runs
- **THEN** the test programs run one after another with their output on the terminal, and the run stops at the first one that fails

#### Scenario: A parallel compile
- **WHEN** `make -j16 check` runs in a tree with nothing built
- **THEN** the generated headers and the bundled htmx-glib are built before anything that includes them, htmx-glib is built once, and the result is the same as that of `make check`
