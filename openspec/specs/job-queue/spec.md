# job-queue Specification

## Purpose
The durable job queue: how work is stored, claimed exclusively, retried, cancelled, scheduled and reported, and what happens to it across crashes and shutdowns.
## Requirements
### Requirement: Durable jobs
The job queue SHALL store every job in the database so that queued and running jobs survive a daemon restart.

#### Scenario: Queued job survives restart
- **WHEN** a job is enqueued and the daemon is stopped and started again before the job runs
- **THEN** the job runs after the restart

#### Scenario: Unknown kind is refused at enqueue
- **WHEN** a job is enqueued with a kind that has no registered handler
- **THEN** enqueue fails with an error naming the kind and no job is stored

### Requirement: Exclusive claiming
The job queue SHALL ensure that a job is claimed by at most one worker at a time.

#### Scenario: Concurrent claimers
- **WHEN** many workers claim concurrently from a queue holding fewer jobs than workers
- **THEN** each job is claimed exactly once and the remaining workers receive nothing

#### Scenario: Claim order
- **WHEN** a queue holds jobs of different priorities and ages
- **THEN** jobs are claimed in order of higher priority first, then earlier run time

#### Scenario: Future jobs wait
- **WHEN** a job is enqueued with a run time in the future
- **THEN** it is not claimed before that time

### Requirement: Leases and crash recovery
The job queue SHALL return a running job to the queue when its worker stops renewing its lease, and SHALL reject any later completion from that worker.

#### Scenario: Worker disappears
- **WHEN** a worker claims a job and then stops heartbeating until the lease expires
- **THEN** the job becomes claimable again and its attempt count has increased by one

#### Scenario: Stale worker cannot complete
- **WHEN** a job's lease expired and another worker claimed it, and the first worker then reports success
- **THEN** the report is rejected and the job remains running under the second worker

#### Scenario: Heartbeat extends the lease
- **WHEN** a worker heartbeats before its lease expires
- **THEN** the job is not returned to the queue while heartbeats continue

### Requirement: Retries with backoff
The job queue SHALL retry a failed job after an increasing delay until its maximum attempts are used, and SHALL then mark it failed and keep the last error.

#### Scenario: Failure is retried later
- **WHEN** a handler reports a failure and attempts remain
- **THEN** the job returns to queued with a run time in the future and the error text recorded

#### Scenario: Attempts exhausted
- **WHEN** a handler fails on the job's final attempt
- **THEN** the job is failed, is never claimed again, and keeps the last error text

#### Scenario: Permanent failure skips retries
- **WHEN** a handler reports a permanent failure on the first attempt
- **THEN** the job is failed immediately

### Requirement: De-duplication
The job queue SHALL NOT hold two unfinished jobs with the same de-duplication key.

#### Scenario: Duplicate enqueue returns the existing job
- **WHEN** a job is enqueued with a de-duplication key matching a queued or running job
- **THEN** no new job is created and the existing job's identifier is returned

#### Scenario: Key is free after completion
- **WHEN** a job with a de-duplication key has finished and another is enqueued with the same key
- **THEN** a new job is created

#### Scenario: Concurrent duplicate enqueues
- **WHEN** two callers enqueue the same de-duplication key at the same moment
- **THEN** exactly one job exists and both callers receive its identifier

### Requirement: Cancellation
The job queue SHALL allow a queued job to be cancelled immediately and a running job to be cancelled cooperatively.

#### Scenario: Cancel queued job
- **WHEN** a queued job is cancelled
- **THEN** it is cancelled and is never claimed

#### Scenario: Cancel running job
- **WHEN** a running job is cancelled and its handler checks for cancellation
- **THEN** the handler stops and the job ends as cancelled, not failed, and is not retried

#### Scenario: Cancel finished job
- **WHEN** a succeeded, failed or cancelled job is cancelled
- **THEN** nothing changes and the caller is told the job had already finished

### Requirement: Concurrency limits
The job queue SHALL run no more jobs from a queue at once, in-process, than that queue's configured concurrency.

#### Scenario: Limit is respected
- **WHEN** a queue with concurrency 2 holds 10 runnable jobs
- **THEN** at no time are more than 2 of its jobs running in-process

#### Scenario: Queues are independent
- **WHEN** one queue is saturated with long jobs
- **THEN** jobs in another queue still start

#### Scenario: Externally served queue
- **WHEN** a queue's in-process concurrency is 0
- **THEN** the daemon never runs its jobs itself and they remain claimable

### Requirement: Progress and events
The job queue SHALL record a running job's progress and SHALL publish an event for every state change and progress update.

#### Scenario: Progress is visible
- **WHEN** a handler reports progress 0.5 with a message
- **THEN** the job's stored progress and message reflect it within two seconds

#### Scenario: Events follow the lifecycle
- **WHEN** a job is enqueued, runs and succeeds
- **THEN** subscribers receive queued, running and succeeded events for it, in that order

### Requirement: Recurring schedules
The job queue SHALL enqueue a scheduled job each time its interval elapses, without ever running two instances of the same schedule at once. A schedule whose next run lies more than its interval ahead (a tick moved it there by a database clock that was later set back) SHALL be brought back to one interval from now before it is next looked at, with one warning naming it; a schedule whose next run is within its interval SHALL be left as it is.

#### Scenario: Schedule fires
- **WHEN** a schedule with a 1 second interval is enabled
- **THEN** its job runs repeatedly, about once per interval

#### Scenario: No overlap
- **WHEN** a scheduled job takes longer than its interval
- **THEN** a second instance is not queued until the first has finished

#### Scenario: No burst after downtime
- **WHEN** the daemon was stopped for many intervals and starts again
- **THEN** the schedule runs once, not once per missed interval

#### Scenario: Disabled schedule
- **WHEN** a schedule is disabled
- **THEN** it enqueues nothing

#### Scenario: After the clock went back
- **WHEN** a schedule of every second has its next run a day and a second ahead and its last run a day ahead, and an hourly one has its next run thirty minutes ahead
- **THEN** the first runs within a few seconds and goes on running every second, one warning says it was brought back, and the hourly one keeps its next run

### Requirement: Shutdown hand-back
On shutdown the job queue SHALL stop claiming, give running jobs a grace period to finish, and return any job still running to the queue without consuming an attempt.

#### Scenario: Job finishes within grace
- **WHEN** shutdown begins while a job is running and the job completes within the grace period
- **THEN** the job is succeeded and the daemon exits with status 0

#### Scenario: Job exceeds grace
- **WHEN** shutdown begins while a job is running and it does not finish within the grace period
- **THEN** the job is queued again with the same attempt count as before it started, and the daemon exits

### Requirement: Jobs without a handler are preserved
The job queue SHALL NOT claim, sweep or delete a stored job whose kind has no handler in the running daemon. At start it SHALL fail each queued job of such a kind for good, keeping it, with an error saying that this build has no handler for its kind, so that the de-duplication key it held (a schedule's whose kind changed, say) is free; a build that has the kind can retry it. Its running jobs and its schedules SHALL be left as they are, and a warning SHALL name the kind.

#### Scenario: Handler missing after restart
- **WHEN** the daemon starts and the queue holds two queued jobs and a running one of a kind that is not registered, and a schedule of it
- **THEN** the queued ones are failed with an error naming the kind, the running one and the schedule are left as they are, and warnings name the kind

#### Scenario: A schedule whose kind changed
- **WHEN** a queued job of a kind no longer registered holds the key of a schedule that now enqueues another kind
- **THEN** the schedule fires again

### Requirement: Retention
The job queue SHALL delete succeeded and cancelled jobs after 7 days and failed jobs after 30 days.

#### Scenario: Old finished jobs are pruned
- **WHEN** the retention job runs and a succeeded job finished more than 7 days ago
- **THEN** that job is deleted and newer or unfinished jobs are kept

### Requirement: Disabled modules' jobs are held
The queue SHALL NOT claim a job whose kind belongs to a disabled module and SHALL NOT fire a disabled module's schedules. Held jobs SHALL keep their state and SHALL become claimable when the module is enabled.

#### Scenario: Held, then released
- **WHEN** a job of a disabled module's kind is queued, and the module is later enabled
- **THEN** the job is not started while the module is disabled, and starts after it is enabled without being re-queued

#### Scenario: Other kinds unaffected
- **WHEN** one module is disabled
- **THEN** jobs of other modules' kinds are claimed as usual

### Requirement: Remote job kinds
A job kind marked remote SHALL NOT be claimed by the daemon's own worker threads and SHALL be claimable only through the worker API, with the claiming worker recorded. Retries, cancellation and expiry SHALL apply to it as to any job.

#### Scenario: Not run in-process
- **WHEN** a remote job is queued and no worker is connected
- **THEN** it stays queued

#### Scenario: Claimed by a worker
- **WHEN** a worker claims a remote job
- **THEN** the job is running and names that worker

### Requirement: Held kinds
A kind SHALL be able to follow a hold, a flag shared by reference: while the hold is on, the queue SHALL NOT claim jobs of that kind in-process and the worker API SHALL NOT hand them to a worker. Held jobs SHALL keep their state and SHALL become claimable when the hold is released, without being queued again. Kinds without the hold SHALL be unaffected.

#### Scenario: Held, then released
- **WHEN** a job of a held kind is queued and the hold is later released
- **THEN** it is not started while held, and starts after the release

#### Scenario: Other kinds unaffected
- **WHEN** a hold is on
- **THEN** jobs of kinds without it are claimed as usual

### Requirement: A remote job may prefer another worker
A remote job's payload MAY name a worker it is not to be claimed by (`requires.prefer_not`) until a time (`requires.prefer_not_until`, seconds since the epoch); before that time every other worker that can do the job SHALL be able to claim it and that worker SHALL NOT, and after it any worker that can do the job SHALL.

#### Scenario: Before the time
- **WHEN** the named worker claims before the time
- **THEN** it is not given the job

#### Scenario: After the time
- **WHEN** the named worker claims after the time
- **THEN** it is given the job

### Requirement: A restarted daemon takes back its own jobs
The names of a job queue's own workers SHALL carry an instance made at each start of the queue, so that no two runs share a worker name whatever host name and process id they run with. A daemon SHALL take back the in-process jobs that a previous run left running, however busy its own workers are, no later than one sweep interval after their leases have run out.

#### Scenario: The container restarts
- **WHEN** the daemon restarts in the same container (same host name, process 1), a job of its previous run is still `running` with a lease that runs out after the start, and the new worker of that job's queue is busy with other work
- **THEN** the job is returned to the queue within one sweep interval of its lease running out

### Requirement: Jobs an earlier run left are taken back at start
A daemon that holds the database lock SHALL take back, 50 seconds after its job queue starts, every running job of its kinds whose worker is an in-process worker of another daemon run with an instance in its name, whatever the job's lease, as a job whose lease ran out is taken back; when the queue was suspended before that, the take-over SHALL wait 50 seconds from the resume. The take-over SHALL be fenced by the lock, and a take-over refused by the fence SHALL be tried again. Jobs of remote workers, and of daemons whose worker names carry no instance, SHALL keep their leases. A job queue that is not told that it holds the database alone SHALL take nothing back before the leases run out.

#### Scenario: Started after a crash
- **WHEN** a daemon that takes the lock starts, and an earlier run left a job running with an hour of lease
- **THEN** the job is queued again once the take-over delay has passed and runs, while a remote worker's job and a job of a daemon without an instance in its worker names keep running

#### Scenario: Suspended before the take-over
- **WHEN** the daemon's queue is suspended before the take-over delay has passed, and resumed after it
- **THEN** the jobs of the earlier run are taken back a whole delay after the resume, not at the resume

#### Scenario: Not the daemon
- **WHEN** a job queue that was not told it holds the database alone starts, and another run left a job running with an hour of lease
- **THEN** the job stays running until its lease runs out

### Requirement: Claims and renewals need the lock
When a daemon gives its job queue its lock, every statement by which the daemon takes work or takes it back SHALL check, in the same statement, that the daemon's lock session (by its process id and start) holds the lock: the queue's claims, lease renewals, sweeps and take-overs, and the worker API's claims and lease expiries. Otherwise a claim SHALL take nothing, a sweep, take-over or expiry SHALL take nothing back, and a renewal SHALL be refused with an error of its own, distinct from a lost lease, upon which the handler of that job SHALL be told to stop as for a shutdown, so that its job is handed back. The worker API's renewals and reports SHALL NOT be fenced.

#### Scenario: The lock's session ends
- **WHEN** the lock's session ends while one job runs and another is queued
- **THEN** the running job's next renewal is refused, its handler stops and the job is handed back with its attempt not counted, the queued job is not claimed, and once the lock is held again on a new session both jobs run

#### Scenario: A process id that repeats
- **WHEN** the fence names the lock session's process id with another start
- **THEN** nothing is claimed

#### Scenario: Taken back while the lock is gone
- **WHEN** the queue's lock session has ended and an earlier run's job and a job whose lease ran out are running
- **THEN** neither is taken back until the lock is held again

#### Scenario: Remote workers
- **WHEN** the worker API's fence does not hold
- **THEN** a remote worker is given no job and no remote lease is taken back

### Requirement: Suspended while the lock is lost
A job queue SHALL be able to suspend: it SHALL then claim, sweep, take back, schedule and renew nothing, and SHALL tell every handler running in it to stop as for a shutdown, so that a job whose handler stops is handed back without using an attempt. Resumed, every queue SHALL look for work at once.

#### Scenario: Suspended and resumed
- **WHEN** the queue is suspended while a job runs, another job is enqueued, and the queue is then resumed
- **THEN** the running job is handed back with its attempt not counted, nothing is claimed while the queue is suspended, and both jobs run after the resume

#### Scenario: Suspended after the lock is gone
- **WHEN** the lock's session has ended, the queue is then suspended, and a handler that reports progress takes seconds to stop
- **THEN** its job is handed back with its attempt not counted, not left running until its lease runs out

### Requirement: The build's schedules and a person's
A module's default schedule SHALL be the build's. At every start of the queue, each schedule the build declares SHALL be created, or, while the stored schedule is still the build's, given the kind, payload and interval the build declares; a shorter interval SHALL bring its next run closer, a longer one SHALL leave it. Every enabled schedule of the build's that the build does not declare SHALL be disabled, not deleted, marked as disabled by the build, with a log line naming it, and enabled again by a build that declares it. A schedule disabled in any other way (switched off by hand in the database before schedules were told apart) SHALL stay disabled, and the log SHALL say so. A schedule a person made or changed SHALL be the person's: no build SHALL change or disable it. Each change SHALL be logged with what it was before.

#### Scenario: A newer build changes a schedule
- **WHEN** a build that declared a schedule hourly without a payload ran, and a build that declares it every minute with a payload starts
- **THEN** the stored schedule runs every minute with the new payload, and the log says what it was

#### Scenario: A newer build drops a schedule
- **WHEN** a build that declared a schedule of every second left it stored, and a build that has its kind but declares no such schedule starts
- **THEN** it does not fire, it is kept disabled, and the log says no module of this build declares it

#### Scenario: Declared again
- **WHEN** a later build declares that schedule again
- **THEN** it is enabled and fires

#### Scenario: A person's schedule
- **WHEN** a person made a schedule of the name the build declares with another interval, and another schedule that no module declares, and the build starts
- **THEN** both keep their intervals and stay enabled, and nothing is logged about them

#### Scenario: Switched off by hand
- **WHEN** a stored schedule of the build's was switched off in the database, not by a start, and the build that declares it starts
- **THEN** it stays disabled and does not fire, and the log says it stays disabled

### Requirement: Jobs an earlier build queued on another queue
At start, the queue SHALL move every queued or running job of one of its kinds that is stored on another queue than the one its kind is claimed from (a build that claims the kind from another queue left it) to its kind's queue, keeping its state, lease, attempts and place in the claim order, and log how many of which kind moved from where. Finished jobs SHALL keep the queue they ran on. Jobs of kinds without a handler SHALL be left as they are.

#### Scenario: Searches of an earlier build
- **WHEN** an earlier build left a queued job and a running job whose lease ran out, of a kind now claimed from another queue, and a finished one
- **THEN** both unfinished jobs run on the kind's queue, the queued one with its priority, the finished one keeps its queue, and one message says what moved

### Requirement: Finishing an acquisition never waits behind searches
Searches for films and episodes SHALL run on a queue of their own (`searches`, one worker unless `jobs.concurrency` says otherwise), so that following the download clients and importing finished downloads (the `downloads` queue) never wait for a search, however many are queued or however long one takes. No other kind SHALL be on the searches' queue.

#### Scenario: The queues of acquisition
- **WHEN** the modules' kinds are registered
- **THEN** the films' and series' searches are on the searches' queue, tracking and imports on the downloads queue, and nothing else on the searches' queue

### Requirement: A person's retry
A failed or cancelled job SHALL be retried as itself: the same job, with its identifier, payload, de-duplication key, priority and attempt limit, queued again on the queue its kind is claimed from, claimable at once, with its attempts counted from none and no cancel request, progress or error left. How its run before ended (its state, attempts, the first 500 characters of its error, worker and times, and who retried it and when) SHALL be kept with it, the latest 20 runs, none longer than a failed job is kept. Nothing SHALL change while another job of its key is queued or running, and the retry SHALL say which job that is, also when that job became queued after the retry looked; when that job ended before it could be named, the retry SHALL be tried once more, and after that say that another job did the work just now. A job of a kind without a handler, or one that is queued, running or succeeded, SHALL NOT be retried; a job deleted while it was being retried SHALL be answered as no such job.

#### Scenario: Retried as itself
- **WHEN** a failed job with a key, a priority of 7, a cancel request left over and progress, stored on a queue its kind is no longer claimed from, is retried
- **THEN** it is queued on its kind's queue with its key and priority, no cancel request, progress or error, is claimed as itself on its first attempt, an enqueue of its key is answered with it, and its earlier run is kept

#### Scenario: The key taken meanwhile
- **WHEN** another job of the key is inserted, not yet committed, while the retry runs, and commits
- **THEN** the retry changes nothing and names that job

#### Scenario: Earlier runs kept
- **WHEN** a job is retried 21 times after its first failure
- **THEN** its latest 20 earlier runs are kept, oldest first

#### Scenario: Earlier runs kept briefly
- **WHEN** a job is retried after an error of 2000 characters, and the retention job runs while a job has an earlier run that ended 31 days ago and one of 8 days
- **THEN** the retried job keeps 500 characters of the error, and the earlier run of 31 days is dropped, the other kept

#### Scenario: Deleted meanwhile
- **WHEN** a failed job is deleted after the retry looked at it and before it changed it
- **THEN** the retry answers that there is no such job

