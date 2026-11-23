## ADDED Requirements

### Requirement: Counting the jobs that wait or run does not read the jobs that ended
A statement that the daemon sends on a timer or at a change of a job's state, and that counts or lists the jobs that are queued or running, SHALL be answered from an index that holds only those jobs. Its cost SHALL NOT grow with the number of finished jobs kept. This covers the count in the page header, the counts of the Jobs page, the probe queue's top-up, the check whether an Analysis refresh is due, the metrics of jobs by queue and state, and the kinds read at start-up.

#### Scenario: Two million finished jobs
- **WHEN** the job table holds 2,000,000 finished jobs and 300 that wait or run, and the header's count is read
- **THEN** the statement reads the 300 through the index of live jobs and no other row of the table

#### Scenario: No probe among the jobs kept
- **WHEN** no probe job is among the finished jobs kept, and the daemon asks whether an Analysis refresh is due
- **THEN** the statement finds that out without walking the finished jobs

### Requirement: The job table is vacuumed and analysed early
The job table SHALL be vacuumed and analysed by autovacuum once a fiftieth of its rows changed, not the server's default fifth, so that its partial indexes and its statistics stay current while most of its rows are finished jobs that never change.

#### Scenario: A table of millions
- **WHEN** the job table holds 2,000,000 rows
- **THEN** autovacuum takes it up after about 40,000 changed rows, where the default waited for 400,000
