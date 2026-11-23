# job-ui Specification

## Purpose
The Jobs page: what is running, queued and finished, shown live, with the actions an administrator can take on a job.
## Requirements
### Requirement: Jobs page
The daemon SHALL provide a page listing jobs with their kind, state, progress, attempts and times, filterable by state and queue, and paged.

#### Scenario: Listing
- **WHEN** a logged-in user opens the Jobs page
- **THEN** running and queued jobs and the most recent finished jobs are listed, newest first

#### Scenario: Filter
- **WHEN** the user filters by state failed
- **THEN** only failed jobs are listed

#### Scenario: Failure detail
- **WHEN** the user opens a failed job
- **THEN** its last error text and attempt count are shown

### Requirement: Live progress
The Jobs page SHALL update a job's row when its state or progress changes, without a page reload.

#### Scenario: Progress appears
- **WHEN** a running job reports progress while the page is open
- **THEN** the row shows the new progress within two seconds

#### Scenario: Page is correct without a stream
- **WHEN** the page is loaded with event streams unavailable
- **THEN** it shows the current state of jobs as of load time

### Requirement: Job actions
An administrator SHALL be able to cancel a queued or running job and retry a failed or cancelled job from the page; other users SHALL NOT. A retry SHALL be the same piece of work: the job itself queued again (job-queue, "A person's retry"), never a copy, and the browser SHALL be sent to that job, whose page SHALL list how it ended before each retry, naming who retried it to administrators only. Only one job's page SHALL read its earlier runs; the list and the live updates SHALL NOT. While another job of its de-duplication key is queued or running, Retry SHALL change nothing and say which job is doing the work. For a kind whose owner asks for its work again (an import, a conversion's encode or checks, a conversion run's list, a measurement of the savings, on the server or on a worker, a worker's encoder test), Retry SHALL change nothing and say where the work is asked for instead. A refusal SHALL be shown on the page.

#### Scenario: Cancel
- **WHEN** an administrator cancels a running job
- **THEN** the job is asked to cancel and the row reflects it

#### Scenario: Retry
- **WHEN** an administrator retries a failed job
- **THEN** the same job is queued again with its payload, the browser goes to it, it runs with its attempts counted from the start, and its page lists the earlier run under "Earlier runs of this job" with its error and who retried it, while a user who is not an administrator sees the earlier run without who retried it, and the rows the live updates read hold no earlier runs

#### Scenario: A retry is the same work
- **WHEN** an administrator retries a failed job with a de-duplication key and a priority of 10, and while it runs the same work is asked for by that key, with two workers on its queue
- **THEN** the job keeps its key and its priority, the request by key is answered with it, and the work runs once

#### Scenario: Retry while the work is being done
- **WHEN** an administrator retries a failed job while another job of its key is running
- **THEN** the response is 409 naming the running job, and the failed job is as it was

#### Scenario: A kind its owner asks for again
- **WHEN** an administrator retries a failed job of a kind whose owner asks for its work again
- **THEN** the response is 409 with the sentence that says where, and nothing changes

#### Scenario: A cancelled job retried
- **WHEN** an administrator retries a cancelled job that had a cancel request
- **THEN** it runs past a lease renewal and to its end, not cancelled again

#### Scenario: Non-admin cannot act
- **WHEN** a non-administrator sends a cancel or retry request
- **THEN** the response is 403 and nothing changes

### Requirement: Activity indicator
Every page's header SHALL show how many things Axis is doing now (jobs of work running and downloads in progress at their clients, never background upkeep), what they are by kind ("2 searches, 1 download": searches, downloads, imports, scans, conversions, other jobs), with how many wait for their turn in its tooltip, and SHALL update it live. It SHALL lead to the Activity page, for anyone logged in. On a phone it SHALL keep to its count.

#### Scenario: Count changes
- **WHEN** a job starts or finishes while any page is open
- **THEN** the header count changes without a reload

#### Scenario: By kind
- **WHEN** two searches run and one download is in progress
- **THEN** the header says 3 running: 2 searches, 1 download, and links to the Activity page

### Requirement: Activity page
The daemon SHALL provide a page, Activity, that lists in a person's words the work under way (since when, how far), the downloads at their clients (how far, how much), the work waiting for its turn (how many ahead) and what ended in the last hour (done, failed and why, cancelled): each job named by its module ("Searching for Harbour Lights (2024), season 1", "Scanning the disk Disk one"), else by its kind's label, with a link to what it is for. Anyone logged in SHALL see it; who asked, the job's number, what a job said (its error, its progress message), the download client's name and a link to an administrator's page SHALL be shown to administrators only: a viewer gets the plain state. Background upkeep SHALL NOT be listed. The page SHALL follow the hub (the last of a burst of changes winning; while anything is in hand also every 30 seconds) and be right as loaded without it.

#### Scenario: A viewer
- **WHEN** someone who is not an administrator opens Activity while a search an administrator asked for waits, a search failed with its indexer's words, a download runs and Identify looks at a folder
- **THEN** it lists "Searching for ..." under the work waiting for its turn, with no name of who asked and no job number, the failure without its words, the download without its client's name, and Identify's line without a link

