# worker-protocol Specification

## Purpose
How encoding work is done outside the daemon: the worker program, its token, the API it speaks, how it proves it sees the right disk, and the leases that keep a lost or late worker from mattering.
## Requirements
### Requirement: Work runs in a separate program
The project SHALL provide a worker program that performs encoding work outside the daemon, on the same machine or another, communicating with the daemon only through the worker API and never with the database.

#### Scenario: Worker failure does not affect the daemon
- **WHEN** a worker or its encoder crashes during a job
- **THEN** the daemon keeps running and the job is handed back after its lease lapses

### Requirement: Worker tokens
A worker SHALL authenticate with a token created by an administrator, shown once and stored only as a hash. A token SHALL be valid only for the worker API, SHALL be revocable with immediate effect, and SHALL NOT appear in any page, log line or error.

#### Scenario: Valid token
- **WHEN** a worker presents its token to the worker API
- **THEN** the request is accepted

#### Scenario: Revoked token
- **WHEN** a revoked token is presented
- **THEN** the request is refused

#### Scenario: Token elsewhere
- **WHEN** a worker token is presented to a route outside the worker API
- **THEN** it grants nothing

#### Scenario: Session on the worker API
- **WHEN** a request to the worker API carries a logged-in session and no worker token
- **THEN** it is refused

#### Scenario: Shown once
- **WHEN** an administrator creates a worker
- **THEN** the token is shown on that page once and cannot be retrieved again

### Requirement: Workers are offered only what they can do
A worker SHALL report the encoder capabilities it measured on its own machine, and the daemon SHALL offer it only work those capabilities cover.

#### Scenario: Missing encoder
- **WHEN** a job needs an encoder the worker has not shown it can run
- **THEN** that job is not offered to that worker

### Requirement: Volume paths are proved
A worker SHALL be given work on a volume only after it has read that volume's marker at its configured path and reported the matching identifier, and SHALL repeat that proof before and during each job. Job descriptions SHALL name a volume and a path relative to it, never an absolute path, and the worker SHALL refuse a relative path that leaves the volume or passes through a symbolic link.

#### Scenario: Wrong disk
- **WHEN** the directory a worker is configured with holds another volume's marker, or none
- **THEN** the mapping is refused and no work on that volume is offered

#### Scenario: Disk replaced during a job
- **WHEN** the marker at the worker's path no longer matches during a job
- **THEN** the worker stops the job and reports the volume unavailable

#### Scenario: Escaping path
- **WHEN** a job description's relative path contains a parent reference or reaches a symbolic link
- **THEN** the worker refuses the job without opening anything outside the volume

### Requirement: Leases and fencing
A remote job SHALL be held under a lease that only heartbeats extend. When the lease lapses the job SHALL be handed back, and a result presented with the old lease SHALL be refused and SHALL change nothing. A worker that cannot renew its lease in time, or is told to stop, SHALL kill its encoder, discard its output and report nothing further for that job. A report or a heartbeat that is not answered, or is answered 429 or a server error, or with anything but the worker API's own answer, SHALL be sent again with a growing delay until the lease's deadline; only the worker API's own refusal SHALL end the report, or fence the job.

#### Scenario: Silent worker
- **WHEN** a worker stops heartbeating
- **THEN** after the lease the job is available again

#### Scenario: Late result
- **WHEN** a worker reports completion with a lease that has been replaced
- **THEN** the response is a conflict and the job's state is unchanged

#### Scenario: Worker fences itself
- **WHEN** a worker cannot reach the daemon before its lease deadline
- **THEN** it kills its encoder and leaves no output behind

#### Scenario: Cancelled
- **WHEN** an administrator cancels a job a worker is running
- **THEN** the worker's next heartbeat is answered with stop, and the worker stops

#### Scenario: A busy proxy is not a refusal
- **WHEN** a worker's report of a finished job is answered 503 and then 429 in plain text, as a proxy or the daemon's connection limits would
- **THEN** the worker logs each answer, sends the report again, and the job succeeds

### Requirement: Nothing a worker says is trusted
The daemon SHALL validate every field of every worker request and SHALL re-check any fact a worker reports before acting on it, with one exception: the measurements of a check job (a decode's outcome, packet counts, track hashes, picture similarity), which it cannot re-check without making them again. Those it SHALL use only where `convert.checks_where` sends checks to workers, only when they fit the plan it sent and bind to the files on its own disk, and only as figures it judges itself; a worker SHALL never report a verdict.

#### Scenario: Malformed request
- **WHEN** a worker sends a request that is malformed, oversized or of the wrong content type
- **THEN** it is refused and nothing changes

#### Scenario: Implausible result
- **WHEN** a worker reports a measurement outside the plausible range
- **THEN** it is not recorded

#### Scenario: Measurements that do not fit the plan
- **WHEN** a worker reports a check job's measurements with another number of copied tracks or points than the plan it was given, or another output name or size
- **THEN** the report is refused, nothing is recorded, and the job is tried again when its lease runs out

### Requirement: Work that changes no file
Workers SHALL perform capability measurement, sample measurement and the measurements of a converted file's checks without creating, changing or removing any file on a volume; only a conversion's encode creates a file, its output in the work area.

#### Scenario: Sample measurement through a worker
- **WHEN** a sample measurement runs on a worker
- **THEN** calibration samples are recorded and no file on the volume is created, changed or removed

#### Scenario: Checks measured on a worker
- **WHEN** a worker measures a converted file's checks
- **THEN** no file on the volume is created, changed or removed, and a read-only mount is enough

### Requirement: Transport protection
The worker SHALL verify the daemon's certificate by default and SHALL refuse to send its token over unencrypted HTTP to an address that is not loopback unless explicitly configured to.

#### Scenario: Plain HTTP refused
- **WHEN** a worker is configured with an unencrypted non-loopback address and no override
- **THEN** it refuses to start and says why

### Requirement: Workers page
The daemon SHALL provide a page, for administrators, listing workers with their state, versions, capabilities, accepted volumes and current work, and allowing a worker to be created, revoked or asked to measure again. Its description of what workers do SHALL be true for the effective `convert.checks_where`: who encodes, and who checks converted files. It SHALL say when conversion work is held after a restart and offer to resume it.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page or an action
- **THEN** the response is 403

#### Scenario: Where checks run, said
- **WHEN** `convert.checks_where` is `worker_only`
- **THEN** the page says that workers encode and check converted files, and that this server does no heavy work

#### Scenario: Held after a restart
- **WHEN** conversion work is held after a restart
- **THEN** the page says so and offers to resume it

### Requirement: Clean stop
When asked to stop, a worker SHALL stop its encoder, give its job back for retry, and exit.

#### Scenario: Stop during a job
- **WHEN** a worker receives a termination signal while encoding
- **THEN** the encoder is gone, the job is available again, and the worker exits successfully

### Requirement: Conversion work
A worker SHALL perform conversion work by proving the volume, checking the argument vector it was given (input and output as its two descriptors, nothing else), opening the source read-only beneath the volume without following links, confirming the source's recorded size, modification time, inode and link count (or, on a volume its configuration lists as seen through a mount that does not keep file identities, its size, its modification time to the second and its link count as the mount reports it), confirming that the digest of samples of the source's content it reads equals the one the daemon sent, writing its output exclusively in the work area under the name it was given, and reporting only the output's name and size. It SHALL remove its output and stop when told to, when its lease lapses, when the volume's marker stops matching, or when its encoder fails or writes more than the source's size. It SHALL recognise its own output for removal through a descriptor of it held open, never by an inode number read after the file was closed. The daemon SHALL accept a result only for the job's current lease and only with the name it chose for that attempt.

#### Scenario: Source changed before encoding
- **WHEN** the source does not match its recorded stamp when the worker opens it
- **THEN** the worker fails the job without encoding

#### Scenario: Stopped during encoding
- **WHEN** a worker is told to stop during a conversion
- **THEN** its encoder is gone and its output removed

#### Scenario: Lease lost
- **WHEN** a worker's lease runs out while it converts, or after its encode finished and before the daemon took the result
- **THEN** its encoder is gone, its output removed, it says nothing more about the job, and anything sent for the job under that lease is refused and changes nothing

#### Scenario: Read-only mount
- **WHEN** a worker's mount of the volume is read-only
- **THEN** it fails the job without creating anything

#### Scenario: Another name reported
- **WHEN** a worker reports an output name other than the one it was given
- **THEN** the conversion is rejected

#### Scenario: A mount without file identities, not listed
- **WHEN** the worker sees the source with another inode number or another time below a second than the daemon recorded, its size and time to the second equal, and the volume is not listed in `volumes_without_file_identity`
- **THEN** it fails the job without encoding, saying what differs and naming the setting

#### Scenario: A mount without file identities, listed
- **WHEN** the volume is listed, and the source has the recorded size, time to the second and one link, and its samples' digest equals the daemon's
- **THEN** the worker encodes it

#### Scenario: Other content of the same size and time
- **WHEN** the samples of the source the worker reads differ from the daemon's
- **THEN** it fails the job without encoding, saying that the content is not what the daemon read

### Requirement: A job whose thread does not end
When a job's thread has not returned a bounded time after the worker told its encoder to stop (a thread blocked in the kernel on a volume that stopped answering), the worker SHALL give the job up: it SHALL say nothing more about the job than a failure for another try (nothing if the job is fenced), SHALL keep the job's state until the thread really returns, and SHALL act on nothing the thread does after the give-up, except that it SHALL then remove a conversion's output the thread left in the work area, or leave it to the daemon's tidy job when the volume does not allow it. It SHALL offer the volumes that job reads no more work until a check proves them again, and SHALL keep running, taking other work and reconnecting to the daemon. It SHALL NOT keep more threads stuck than the number of jobs it runs at once: one more, and it SHALL give its other work back and exit with a failure status, saying which jobs' threads do not end.

#### Scenario: The daemon and the mount go away during an encode
- **WHEN** the daemon stops answering during an encode, the lease lapses, the encoder is stopped, the job's thread stays blocked, the disk goes away and the worker gives the job up
- **THEN** when the thread returns the worker keeps running, reports nothing about the job, and offers the volume again once the disk is back and proved

#### Scenario: A result found after the give-up
- **WHEN** a conversion's encode finished but its thread stayed blocked until after the job was given up
- **THEN** the result is not reported, and the output is removed from the work area when the thread returns

#### Scenario: Checks and sample measurements
- **WHEN** the thread of a converted file's checks or of a sample measurement stays blocked past its lease
- **THEN** the job is given up the same way and its measurements are not reported

#### Scenario: Stuck for good
- **WHEN** more job threads are stuck than the worker runs jobs at once
- **THEN** the worker gives its other work back, says which jobs' threads do not end, and exits with status 1

### Requirement: A volume without a marker is not offered to workers
Because a worker proves a disk by its marker, the daemon SHALL offer no work on a volume that has no marker yet, whatever a worker reports, and the Workers and Conversions pages SHALL name such volumes and say why no worker can use them until their marker is written.

#### Scenario: Not offered
- **WHEN** a job needs a volume added from a read-only disk that has no marker yet
- **THEN** no worker is given that job, and once the volume's marker is written it is offered as any other

#### Scenario: The pages say why
- **WHEN** a volume has no marker yet
- **THEN** the Workers and Conversions pages name it and say that a worker recognises a disk only by its marker

### Requirement: A volume that hangs
A worker SHALL NOT wait on a volume that does not answer, neither to register nor to check its other volumes. It SHALL prove and check each volume independently and with a time limit. Its first registration SHALL wait for the first proofs at most a short, fixed time and then go with the volumes proved so far, saying which volumes are not proven and why. A volume whose check exceeds the limit SHALL be offered no work and the jobs that read it SHALL be stopped. It SHALL register a volume again when the volume answers and is proved. It SHALL NOT start a new check or proof for a volume while the previous one has not returned.

#### Scenario: A mount hangs when the worker starts
- **WHEN** a worker starts while one of its volume paths is a mount that never answers
- **THEN** the worker registers with its other volumes within a few seconds, says that it registers without the hung volume and why, and takes work on the others; when the mount answers and the volume is proved, the worker registers again with it

#### Scenario: One volume hangs, another changes
- **WHEN** a volume's check does not return and another disk goes away and comes back
- **THEN** the worker drops the disk that went away at its next check, proves it and offers it again when it comes back, and drops the hung volume after the limit, all without waiting for the hung call

#### Scenario: A volume that hangs for good
- **WHEN** a volume's check never returns
- **THEN** no more than one thread of the worker is blocked on that volume, however many checks come due

### Requirement: Mounts that do not keep file identities
A worker SHALL check every volume strictly (inode and time to the nanosecond) unless the person running it lists the volume's path in `volumes_without_file_identity` (`AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`), which SHALL name only paths that are among its volume paths; nothing SHALL be inferred from the filesystem. The worker SHALL report at registration, per volume, whether its mount keeps file identities, and the daemon SHALL keep it with the volume's path. The sample measurement SHALL compare the size and the time to the second of a file on a listed volume, and the time to the nanosecond otherwise.

#### Scenario: Default
- **WHEN** a worker is configured with volumes and no `volumes_without_file_identity`
- **THEN** it reports every volume as keeping file identities and checks every source as strictly as before

#### Scenario: A path that is not a volume path
- **WHEN** `volumes_without_file_identity` names a path that is not one of the worker's volume paths
- **THEN** the worker refuses to start and names the path

### Requirement: Protocol versions are not mixed
Every message SHALL carry the protocol version, and each side SHALL accept only its own. The daemon SHALL refuse a request of another version before reading it, with an answer an older worker can read (in that worker's version when the daemon can write it, with an error word every version knows) naming both versions and what to do, and the Workers page SHALL say so; the worker SHALL read an older daemon's refusal and log that the two speak different versions. Neither side SHALL run or give work across versions. The capability measurement a worker keeps SHALL keep its own format number, independent of the protocol's.

#### Scenario: An older worker
- **WHEN** a worker of protocol 1 registers with a daemon of protocol 2
- **THEN** the request is refused with a protocol 1 answer saying "this daemon speaks worker protocol 2 and this axis-worker 1", nothing it said is recorded, and it is given no work

#### Scenario: An older daemon
- **WHEN** a worker of protocol 2 is pointed at a daemon of protocol 1
- **THEN** it logs that the daemon speaks protocol 1 and it speaks 2, takes no work and keeps trying

### Requirement: A crashed encoder pauses its worker
When a worker's encoder dies of a signal it was not sent by the worker (not the runner's own file size or CPU limit), the worker SHALL report the job failed as for any encoder failure and SHALL take no more work until an administrator resumes it on the Workers page or the worker process is restarted. It SHALL say so in each claim until a claim's answer says the daemon no longer holds it paused; the daemon SHALL give a paused worker no work, and a resume SHALL not be undone by the worker repeating the crash it was resumed from. An encoder that exits with a status SHALL NOT pause the worker. Other workers SHALL NOT be affected.

#### Scenario: Crash
- **WHEN** a worker's encoder dies of SIGSEGV during a conversion
- **THEN** the conversion fails, the worker takes no more work, and the Workers page shows it paused with the job and the signal

#### Scenario: Ordinary failure
- **WHEN** a worker's encoder exits with status 1 because it cannot read the input
- **THEN** the conversion fails and the worker takes the next job

#### Scenario: Another worker
- **WHEN** one worker is paused and work is queued
- **THEN** another worker that can do the work takes it

#### Scenario: Restarted
- **WHEN** a paused worker is stopped and started again
- **THEN** it registers and takes work

### Requirement: Check work
A worker of protocol 3 SHALL offer to run `convert.verify` jobs. For one, it SHALL prove the volume, open the original read-only beneath the volume without following links and confirm its recorded stamp and the digest of its samples as for a conversion (with the same rules for mounts that do not keep file identities), open the output read-only in the volume's work area as a plain file of the recorded size, and make the measurements the job's plan names with the shared measurement code: a full decode of the output with its copied tracks' hashes, the picture's packets in both files, the original's copied tracks' hashes, and the similarity of the pictures at each point; and the SHA-256 of the whole output, computed while it reads the output, and the digests of samples of the output and of the original. It SHALL prove the volume again afterwards and report the figures and how each tool run ended, never a verdict. A tool run that was cancelled, could not start or crashed SHALL NOT be reported as a measurement.

#### Scenario: Measured and reported
- **WHEN** a worker runs a check job on a good output
- **THEN** it reports every measurement of the plan, the output's digests and the original's, and the daemon accepts the output

#### Scenario: The original changed
- **WHEN** the original's samples the worker reads differ from the digest in the job
- **THEN** it reports no measurement and fails the job saying the source changed

### Requirement: A crashed check pauses its worker
When a tool of a check job dies of a signal it was not sent by the worker (not the runner's own file size or CPU limit), the worker SHALL fail the job for another try and SHALL take no more work until an administrator resumes it on the Workers page or the worker process is restarted, as for an encoder that crashed.

#### Scenario: Decoder crash while checking
- **WHEN** the ffmpeg of a check job dies of SIGSEGV
- **THEN** the job is tried again, and the worker is shown paused with the job and the signal

### Requirement: The check goes to another worker first
When another suitable worker than the one that encoded is connected as a check job is queued, the job SHALL be offered only to other workers for two minutes, then to any worker that can take it, the one that encoded included.

#### Scenario: Two workers
- **WHEN** a conversion encoded on worker A is ready to be checked and worker B is connected and has proved the volume
- **THEN** A's claim within two minutes does not get the check job and B's does

#### Scenario: One worker
- **WHEN** the worker that encoded is the only suitable worker
- **THEN** it gets the check job at once

### Requirement: Protocol 3
The check job's kind, description and result SHALL make the worker protocol version 3. A daemon of protocol 3 SHALL refuse a request of protocol 2 or 1 before reading it, in that version, with the word `malformed` and a sentence naming both versions and what to do, and the Workers page SHALL say so; a worker of protocol 3 SHALL read a refusal of a daemon of protocol 2 and log that the two speak different versions. Neither side SHALL run or give work across versions.

#### Scenario: A worker of protocol 2
- **WHEN** a worker of protocol 2 registers with a daemon of protocol 3
- **THEN** it is refused with a protocol 2 answer saying "this daemon speaks worker protocol 3 and this axis-worker 2", and it is given no work

#### Scenario: A daemon of protocol 2
- **WHEN** a worker of protocol 3 is pointed at a daemon of protocol 2
- **THEN** it logs that the daemon speaks protocol 2 and it speaks 3, takes no work and keeps trying

### Requirement: A job given up while the worker stops
A worker that gives a job up because its thread does not end SHALL report it as a failure, never as a job given back by a clean stop, also while the worker is stopping. When the job's thread had already failed before it stood still (a volume lost, an output name taken), the report SHALL carry that failure's own words, and a failure for good SHALL stay one; otherwise the job SHALL be reported failed for another try.

#### Scenario: Stopped while a thread does not end
- **WHEN** a worker is told to stop while a job's thread stays blocked, and gives the job up
- **THEN** it reports the job failed for another try and not given back, and exits with status 0

#### Scenario: Given up after failing for good
- **WHEN** a job's thread fails for good (its output name is taken) and then stays blocked, and the worker gives the job up
- **THEN** the worker reports that failure, in its own words, not for another try

### Requirement: A second crash on the same job
A resume SHALL be used up by the first claim that reports the crash the worker was resumed from; a later report of a crash on the same job SHALL pause the worker again until a person resumes it, also when that first claim brought the job back.

#### Scenario: The same job crashes again after a resume
- **WHEN** a worker resumed after its tool crashed on a job reports a crash on that same job again
- **THEN** the worker is paused again and given no work

