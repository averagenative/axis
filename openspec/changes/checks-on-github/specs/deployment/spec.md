## ADDED Requirements

### Requirement: The gates run on every push to main
The repository SHALL hold a workflow that runs the four gates (`make check`, `make fuzz`, `make check-container` and `make check-examples`) on every push to the main branch and when started by hand, each gate in a job of its own. The jobs SHALL NOT wait for each other, a failing job SHALL NOT stop the others, and a run for one commit SHALL NOT cancel or replace a run for another. The workflow SHALL NOT run on pull requests. It SHALL use standard runners only, SHALL ask for no permission except to read the source, SHALL use no secret and SHALL publish nothing. It SHALL NOT run a hardware encoder. A job that fails SHALL show the failing check's output in its log and SHALL keep its whole log for a limited time. A test or a part of a check that did not run SHALL NOT pass unnoticed: the run SHALL name the tests that skipped themselves, and a skipped part of `make check-examples` SHALL fail its job.

#### Scenario: A push to main
- **WHEN** a commit is pushed to the main branch
- **THEN** four jobs start, one for each gate, and the run is green only when all four pass

#### Scenario: One gate fails
- **WHEN** one of the four jobs fails
- **THEN** the other three run to their end, the run is marked failed, and the failed job's log is kept

#### Scenario: Two pushes in a row
- **WHEN** a second commit is pushed while the first commit's run has not finished
- **THEN** both runs finish, and each commit has its own result

#### Scenario: A pull request from a fork
- **WHEN** a pull request is opened from a fork
- **THEN** the workflow does not run, and none of the pull request's code is run by the repository's workflows

#### Scenario: A part of the examples cannot run
- **WHEN** `make check-examples` skips a part on the runner
- **THEN** the job fails and names the skipped part

## MODIFIED Requirements

### Requirement: Published images
The project SHALL publish versioned images for amd64 and arm64, built and signed by its own pipeline on version tags, with the source revision in the image labels; that pipeline SHALL build and publish images and SHALL do nothing else. A release tag (`vX.Y.Z`) SHALL be published under the exact version, the minor line and `latest`. A pre-release tag (a version tag with a hyphen suffix, such as `v0.1.0-rc1`) SHALL be published under the exact version with its suffix and the moving tag `rc` only, and SHALL NOT move the minor line or `latest`. A version tag SHALL NOT publish a `sha-<commit>` tag; a run started by hand at a commit without a version tag SHALL publish `sha-<commit>` only. The programs in an image SHALL report the image's version, with a pre-release's suffix, and the build SHALL fail when they report another.

#### Scenario: A version tag
- **WHEN** a release tag such as `v0.1.2` is pushed
- **THEN** an image for both architectures is pushed under `0.1.2`, `0.1` and `latest`, and signed

#### Scenario: A release candidate
- **WHEN** the tag `v0.1.0-rc1` is pushed
- **THEN** an image for both architectures is pushed under `0.1.0-rc1` and `rc` only, and signed, and `axisd --version` in it reports `0.1.0-rc1`

#### Scenario: A tag the source was not moved to
- **WHEN** an image is built as version `0.2.0` from a source whose version is `0.1.0`
- **THEN** the build fails and nothing is pushed
