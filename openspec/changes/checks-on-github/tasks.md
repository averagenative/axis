# Tasks

## 1. The workflow

- [x] 1.1 Add `.github/workflows/checks.yml`: four jobs, one for each gate, on every push to `main` and by hand; not on pull requests; no `concurrency` group.
- [x] 1.2 Install the packages of the build stage of `deploy/Containerfile`, with clang, ffmpeg and strace for the checks.
- [x] 1.3 Give the runner what a developer's machine has: user namespaces for the tests that mount a private tmpfs, a systemd session for the runner's user, and podman-compose as the Compose front end.
- [x] 1.4 On a failure, print the end of the log again and keep the whole log as an artifact for seven days.
- [x] 1.5 Name the tests that skipped themselves in the run's summary; fail `check-examples` when a part is skipped.
- [x] 1.6 Set `TEST_JOBS` and `FUZZ_JOBS` in one marked place (4 and 4, the processors of a standard runner).
- [x] 1.7 `tests/test-web-jobs.c`: wait for the message that follows the job's end, not for the first one that says 0 (it failed once on the runner).

## 2. Documents

- [x] 2.1 `README.md`: the badge, and "The checks on GitHub" with the four commands.
- [x] 2.2 `deploy/README.md`: the image workflow is no longer the only one; the checks also run on GitHub.
- [x] 2.3 `openspec/project.md`: where the gates run.
- [x] 2.4 `.github/workflows/images.yml`: read; its header says nothing about where the checks run, so it is not changed.

## 3. Proof

- [x] 3.1 Scenario "A push to main": three runs in a row with all four jobs green on the runner (on the branch, through a trigger that was removed before the merge), and each job's time noted in the workflow.
- [x] 3.2 Scenario "One gate fails": seen while the workflow was written, when one job failed and the others ran to their end with the failed job's log kept.
- [x] 3.3 Scenario "A part of the examples cannot run": the step that fails on a skipped part, read against the script's own wording of a skip.
- [ ] 3.4 Scenario "Two pushes in a row": not tried; it follows from the workflow having no `concurrency` group.
- [ ] 3.5 Scenario "A pull request from a fork": not tried; it follows from the workflow having no `pull_request` trigger.
- [x] 3.6 `openspec validate checks-on-github --strict`.
