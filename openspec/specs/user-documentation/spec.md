# user-documentation Specification

## Purpose
Define the user guide under `docs/`: pages organised by what a person wants to do, each saying what has been verified against real services, with made-up examples only, and a runbook for the database.
## Requirements
### Requirement: The guide is organised by use case
The project SHALL keep a user guide under `docs/`, linked from the root `README.md` and indexed by `docs/README.md`. Each page SHALL be one worked use case, a person's goal and not a feature or a screen, with what is needed first, numbered steps, what the screen shows and what can go wrong. Labels, menu paths, settings and messages SHALL be quoted as the interface renders them.

#### Scenario: A page is a use case
- **WHEN** a person opens a page of the guide
- **THEN** it starts from a goal, lists what they need first, gives numbered steps from start to finish, says what the screen shows, and says what can go wrong and what to do

#### Scenario: The index leads to every page
- **WHEN** a person opens `docs/README.md`
- **THEN** every page of the guide is listed by what the person wants to do, and each use case that has no page yet is listed as not written

#### Scenario: Deployment detail is linked
- **WHEN** a page needs container, orchestration or proxy detail
- **THEN** it links to `deploy/` and does not repeat it

### Requirement: Each page states what is verified against real services
Each page of the guide SHALL end with a "Verified" note that says what has run against real services and hardware and what has only run in tests or against test doubles. A page SHALL NOT claim more than has been run, and SHALL agree with the known limits that `openspec/project.md` records.

#### Scenario: A feature proven only in tests
- **WHEN** a page describes a feature that is built but has not run against the real service or hardware
- **THEN** the page's "Verified" note says so in words

#### Scenario: What was run changes
- **WHEN** a feature is run against a real service for the first time, or a change alters what a page describes
- **THEN** the page and its "Verified" note are updated in the same change

### Requirement: Examples are made up
The guide SHALL use made-up titles, host names and paths, with hosts under `example.com`, and SHALL NOT describe any particular person's server or library. Screenshots SHALL be taken from a demo instance, and the images each page needs SHALL be listed in `docs/SCREENSHOTS.md`.

#### Scenario: No real names
- **WHEN** a page needs an example title, host or path
- **THEN** it uses a made-up one

### Requirement: The database runbook
The user guide SHALL have a page for backing up, restoring and repairing the database. It SHALL describe only what Axis does today, SHALL mark each step that has not been run, and SHALL say what a person can see when the database is the problem and what to do about each.

#### Scenario: Following the restore
- **WHEN** a person follows the runbook's restore
- **THEN** every step they run was run by the review or a check, or is marked untested on the page

#### Scenario: The database is the problem
- **WHEN** Axis cannot reach its database, or the database's disk is full
- **THEN** the runbook names what the person sees (the probes' answers and the log lines) and the step to take

