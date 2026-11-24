# Run the four gates on GitHub on every push to main

## Why

A change is ready when four gates pass: `make check`, `make fuzz`, `make check-container` and `make check-examples`. Until now they ran only on a developer's machine, one after another and alone on it, which takes well over an hour and keeps that machine busy. Nothing recorded, for a given commit, whether the gates passed.

The repository is public, so GitHub's standard runners cost nothing. Each gate can run there in a job of its own, at the same time as the others, on a machine that does nothing else.

## What changes

- A second workflow, `.github/workflows/checks.yml`, runs the four gates on every push to `main` and when started by hand. Each gate is a job of its own. The jobs do not wait for each other and do not stop each other, and runs for different commits do not cancel each other.
- The workflow does not run on pull requests. A pull request from a fork is code that nobody has read, and the gates compile and run it.
- The workflow asks for no permission except to read the source, uses no secret and publishes nothing.
- A job that fails prints the end of its log again and keeps the whole log as an artifact for a week.
- Nothing is skipped silently. `make check` lists in the run's summary the tests that skipped themselves, with their reasons. A part of `make check-examples` that is skipped fails the job, because the workflow installs every prerequisite.
- The README shows the workflow's result for `main` as a badge, and says how to run the same four commands before opening a pull request.
- The image workflow is no longer the repository's only workflow. It still builds images and nothing else.
- No change to the daemon, the worker or the Makefile. One test is changed, `tests/test-web-jobs.c` (`/web-jobs/live/activity-indicator`): on a busy four-core runner it took an earlier message to the header for the one that follows a job's end.

## Impact

- Capabilities: `deployment`.
- Files: `.github/workflows/checks.yml` (new), `README.md`, `deploy/README.md`, `openspec/project.md`, `tests/test-web-jobs.c`.
- Behavior that differs: none in Axis. A push to `main` now starts a run of the checks; a green run for a commit is the sign that it can be deployed.
- Not covered: the hardware encoder tests (`make check-hwenc`), the scale tests and the Jellyfin check stay outside the gates, on GitHub as elsewhere. The arm64 build is tested only by the image workflow, in its build stage.
