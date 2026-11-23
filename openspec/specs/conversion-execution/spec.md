# conversion-execution Specification

## Purpose
Running a conversion: a trial that leaves the library untouched or a replacement of the original, the gates a request must pass, the checks the result must pass before anything is placed, and how an interrupted conversion is recovered so that exactly one complete file is ever under a library name.
## Requirements
### Requirement: Conversion on explicit request
An administrator SHALL be able to request the conversion of one file with one profile revision as a trial or as a replacement, after a confirmation that states what will happen and the state of every gate. A replacement's confirmation SHALL state that the original will be deleted and cannot be recovered (or, with a retention set, for how long it is kept first) and SHALL need a box ticked saying so.

#### Scenario: Confirmation required
- **WHEN** a replacement is requested without the confirmation ticked
- **THEN** nothing is queued

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests a conversion
- **THEN** the response is 403 and nothing is queued

### Requirement: Gates
A conversion SHALL proceed only while storage is not read-only, the volume is online and writable, the file is a present, unpinned video, the revision's definition is valid, the file is eligible for the revision, a plan can be made, no other conversion of the file is in flight, the file is not remembered as not worth it or already converted with the revision, and free space stays above the floor. A replacement SHALL additionally require a proven revision that is its profile's current one, a player verdict of safe for the file's library root (an unknown verdict is not enough, and no setting relaxes this), a local filesystem unless a setting accepts a network one, and, for a new extension, that no file of the new name exists. The gates SHALL be evaluated at request, when a worker claims the work, and immediately before placement.

#### Scenario: Read-only storage
- **WHEN** storage is read-only
- **THEN** no conversion can be requested and nothing is written

#### Scenario: Locked profile
- **WHEN** a replacement is requested with a revision that has not been proven
- **THEN** it is refused, at request, at claim and before placement

#### Scenario: Unknown verdict
- **WHEN** a replacement is requested and the player check's verdict for the file's root is unknown or risky
- **THEN** it is refused

#### Scenario: Trial with a locked profile
- **WHEN** a trial is requested with a revision that has not been proven
- **THEN** it is accepted

#### Scenario: Gate fails later
- **WHEN** a file becomes pinned after its conversion was requested
- **THEN** the conversion stops before placement, its output is removed and the original is unchanged

#### Scenario: Not enough space
- **WHEN** the estimated output would take the volume below its free-space floor
- **THEN** the conversion is refused

#### Scenario: Two requests for one file
- **WHEN** a conversion of a file is requested while another of it is in flight
- **THEN** the second is refused

### Requirement: Workers write only in the work area
A worker SHALL create its output only in the volume's work area, under the name the daemon chose, never overwriting an existing file, SHALL remove its output when it stops without a result, and SHALL NOT rename, replace or remove any file in a library.

#### Scenario: Output location
- **WHEN** a worker encodes a file
- **THEN** the only file it creates is in the work area and no library file is changed by the worker

#### Scenario: Name already exists
- **WHEN** the output name the worker was given already exists
- **THEN** the worker fails the job without overwriting it

### Requirement: Verification by the daemon
Before any placement the daemon SHALL itself verify, and SHALL require all of: the output is the file it asked for and is a plain single-linked file on the volume; the source's size, times, inode and link count are unchanged and it is not pinned; the plan made again equals the one the worker was given and the output has exactly the planned streams with the planned flags, languages, attachments and chapters; its picture size, bit depth, colour properties and HDR format equal the plan's and, for HDR10, its mastering-display and content-light values equal the source's exactly, in the bitstream; it decodes completely without error; each stream's duration is within tolerance of the source's and the picture's packet count is equal; every track the plan copies has packet data bit for bit equal to the source's track (timestamps and container framing aside) and every copied attachment the source's data exactly; sampled pictures are similar to the source's above a floor; it is smaller by at least the profile's minimum saving; the gates still hold. The measurements of the full decode, the packet counts, the copied tracks' data and the sampled pictures MAY be made by a worker (Where the checks run); every check SHALL nevertheless be decided by the daemon, from the measurements against its own plan and thresholds, with the same conditions in every mode, and a measurement that could not be made SHALL fail its check. On any failure the output SHALL be removed when it is a plain file with one name in the work area (a link SHALL be left where it is), the original SHALL be left unchanged, and the failed check SHALL be recorded, with the checks after it recorded as not run.

#### Scenario: Truncated output
- **WHEN** the output is incomplete
- **THEN** it is rejected, removed, and the original is unchanged

#### Scenario: Missing stream
- **WHEN** the output lacks a stream the plan expected
- **THEN** it is rejected and the original is unchanged

#### Scenario: HDR values lost
- **WHEN** the output of an HDR10 source lacks or alters the source's static metadata
- **THEN** it is rejected and the original is unchanged

#### Scenario: Wrong picture
- **WHEN** the output's sampled pictures do not match the source's (another movie of the same length, wrong colour)
- **THEN** it is rejected and the original is unchanged

#### Scenario: Copied track replaced
- **WHEN** a track the plan copies is in the output with other content of the same codec, channels and length (silence, another track, a subtitle with changed text, another attachment of the same name)
- **THEN** it is rejected by the copied-tracks check and the original is unchanged

#### Scenario: Copied tracks from Matroska into MP4
- **WHEN** a Matroska source's AAC and AC-3 tracks are copied into an MP4 output
- **THEN** they are found equal to the source's, and a track the plan made anew is said to be compared by structure, length and a clean decode only

#### Scenario: Source changed
- **WHEN** the source's size, times, inode or link count differ from those recorded at request
- **THEN** the conversion is rejected and the original is unchanged

#### Scenario: Not smaller enough
- **WHEN** the output is not smaller than the source by the profile's minimum saving
- **THEN** it is rejected, the original is kept, and the file is not offered again with that revision

#### Scenario: Worker reports a different file
- **WHEN** a worker reports an output name other than the one it was given, or the file of that name is not a plain single-linked file in the work area
- **THEN** the conversion is rejected and nothing outside the work area is touched

#### Scenario: A worker's measurements judged by the daemon
- **WHEN** a worker reports a decode that wrote an error, or a sampled picture below the floor
- **THEN** the daemon rejects the output by that check, with the same sentence it would have written had it measured itself

### Requirement: Trials do not touch the library
A trial's verified output SHALL be kept in the work area for a retention time (or until an administrator removes it) and SHALL NOT change any library file.

#### Scenario: Trial
- **WHEN** a trial completes
- **THEN** the library's files are exactly as before and the output is in the work area

### Requirement: Replacement is atomic or recoverable
A replacement SHALL be performed only by the daemon, by one job at a time per conversion, holding a write guard and the shared form of the scan's volume lock, and proving the volume again before each change. At every instant the library SHALL hold either the complete original or the complete verified output under the file's name, and never a partial file. The original SHALL be retired into the volume's recycle folder and removed from it only after the replacement is in place, verified and recorded, and only after its retention (`convert.keep_original`, default zero). An interruption at any point SHALL leave a state from which the daemon completes or abandons the replacement from its own records without losing both files; when the records and the disk do not agree, nothing SHALL be removed.

#### Scenario: Same extension
- **WHEN** the output has the source's extension
- **THEN** it takes the source's name in one atomic step, and the original is then only in the recycle folder

#### Scenario: Different extension
- **WHEN** the output has a different extension
- **THEN** the new file is in place and durable before the source is retired, and if the new name already exists the replacement is refused

#### Scenario: Original removed after recording
- **WHEN** a replacement completes with a retention of zero
- **THEN** the original is removed from the recycle folder only after the conversion is recorded as done

#### Scenario: Interrupted placement
- **WHEN** the daemon stops at any point during placement and starts again
- **THEN** exactly one complete file is under the library name once recovery finishes, and the records agree with the disk

#### Scenario: Volume goes away
- **WHEN** the volume becomes unavailable during a conversion
- **THEN** nothing is concluded, and the conversion is completed or abandoned when the volume is provably back

### Requirement: Nothing else changes
A conversion SHALL change no file other than its own output, the file being replaced, the recycle folder entry of that file, and files in the work area. Files sharing the source's base name SHALL be left as they are.

#### Scenario: Sidecar files
- **WHEN** a movie with separate subtitle files is replaced and its extension changes
- **THEN** the subtitle files are unchanged and still match the movie's base name

### Requirement: Leftovers are accounted for
Files in a work area that no record accounts for SHALL be listed for an administrator and removed only after a grace time; a name that is not one of Axis's own forms SHALL be listed and never removed.

#### Scenario: Orphan
- **WHEN** a temporary file is left by a worker that died
- **THEN** it is listed, and removed after the grace time and not before

#### Scenario: Attempt given back
- **WHEN** the lease of a conversion's attempt runs out and its job goes back to the queue
- **THEN** the conversion waits for a worker again and says why, its original is untouched, and what the attempt left in the work area is listed

### Requirement: Record of every conversion
Every conversion SHALL be recorded against its file with the profile revision, the requester, the encoder, worker and arguments, where the checks were measured and by which worker, sizes before and after, each check's result, each placement step, and its outcome. A completed replacement SHALL update the file's index record (keeping its identity, so that the links of movies and TV episodes to it follow it), SHALL ask for the new file to be probed, SHALL record that the file was converted and the ladder quality of the file it was made from, and SHALL be recorded so that the file is not converted with the same revision again.

#### Scenario: History
- **WHEN** a file has been replaced
- **THEN** its conversion's page shows the checks with their figures and the sizes

#### Scenario: Not repeated
- **WHEN** a file was replaced with a revision
- **THEN** a conversion of it with that revision is refused

#### Scenario: The movie keeps its file
- **WHEN** a movie's file is replaced, with the same or a different extension
- **THEN** the movie is still in the library with that file, and an upgrade decision treats the file as its source's quality

#### Scenario: Who converted and who checked
- **WHEN** a conversion was encoded on one worker and checked on another
- **THEN** its page names both, and says so when it was the same worker

### Requirement: Proving a revision by hand
An administrator SHALL be able to mark a video profile revision proven when at least `convert.prove_trials` (default 3) trials of it on that many different files have passed every check and no trial of it was rejected by an output check, after a confirmation that names those trials. The mark SHALL record who made it, when, and which trials. Nothing SHALL prove a revision automatically.

#### Scenario: Too few trials
- **WHEN** a revision has fewer passed trials than the setting
- **THEN** it cannot be marked proven

#### Scenario: Proven by hand
- **WHEN** an administrator confirms proving a revision with enough passed trials
- **THEN** the revision is unlocked, and its proof names the administrator, the time and the trials

### Requirement: What a conversion made is recorded
A conversion SHALL record its source's HDR class as the probe classified it at the request (SDR only when the probe was certain of it; any class the probe was uncertain of recorded as not classified), the class of the picture its plan makes when a worker takes the work, and, when a conversion run asked for it, that run.

#### Scenario: An untagged SDR source replaced
- **WHEN** a source the probe classified as SDR with certainty, that states no colour, is replaced by a 10-bit profile
- **THEN** its conversion records the source as SDR and the picture made as SDR, while the new file's own probe does not classify it

#### Scenario: A run's conversion
- **WHEN** a conversion run asks for a file
- **THEN** the conversion records the run, and its page names it

### Requirement: A run's request is the single request
A request made by a conversion run SHALL be the single conversion's request, evaluating the same gates in the same function at request, at claim and before placement, recorded as asked for by the person who started the run (trials) or confirmed its replacing (replacements); a refusal SHALL say which gate refused.

#### Scenario: The same gates
- **WHEN** a run asks for a replacement while the player check's verdict for the file's library is unknown
- **THEN** the request is refused by the verdict gate exactly as a single replacement's would be, and nothing is stored

### Requirement: The source on the daemon's disk when a worker takes the work
When a worker takes a conversion, the daemon SHALL open the file on its own disk and require the whole stamp recorded at the request (size, modification and change times, inode, one link), refusing the conversion when the file differs, and SHALL read samples of the file's content (its first and last blocks and blocks spread evenly between them, or the whole of a small file) and give the worker their SHA-256 digest in the description of the work, for every worker. A volume or file that cannot be read at that moment SHALL make the work wait and be tried again. The daemon's own checks before placement SHALL NOT depend on what the worker checked.

#### Scenario: Changed without a scan
- **WHEN** the file was rewritten in place after the request, its size and modification time put back, and no scan has run since
- **THEN** the conversion is refused when a worker takes it, saying the file on disk is not the one the conversion was requested for

#### Scenario: The digest given
- **WHEN** a worker takes a conversion
- **THEN** its description carries the digest of the source's samples as the daemon read them from its own disk

### Requirement: Where the checks run
The setting `convert.checks_where` SHALL say where the measurements of a converted file's checks are made: `server` (on this server; the default), `worker` (on a worker that is connected, speaks the daemon's protocol, is neither revoked nor paused and has proved the file's volume, else on this server) or `worker_only` (only on a worker: with no such worker the conversion SHALL wait and its page SHALL say why, and the daemon SHALL NOT make those measurements on this server; the light checks of the identity, the original, the streams and the picture still run here). Before handing the measurements to a worker the daemon SHALL run and record the checks of the output's identity, the original, the streams and the picture; when the measurements come back it SHALL run those again, require the output's stamp to be the one recorded at hand-out, require the samples of the output and of the original the worker read to be the ones it reads on its own disk, and, while `convert.confirm_whole_file` is on (the default), require the SHA-256 of the whole output to be the one it computes itself. Measurements that do not bind to the files SHALL NOT be used: nothing SHALL be placed, the output SHALL be removed, the original SHALL be unchanged, and the conversion SHALL fail saying why. A check job that workers cannot finish in its attempts SHALL be measured on this server in `worker` mode and SHALL fail the conversion in `worker_only` mode. The conversion's page and the Workers page SHALL show the effective choice in plain words.

#### Scenario: On the server
- **WHEN** `convert.checks_where` is `server` and a converted file is ready
- **THEN** the daemon measures and decides every check itself, as before

#### Scenario: On a worker
- **WHEN** it is `worker`, a suitable worker is connected, and it reports measurements of a good output
- **THEN** every check passes, the page says it was checked on that worker, and nothing heavy ran on the server

#### Scenario: No worker connected
- **WHEN** it is `worker` and no suitable worker is connected
- **THEN** the daemon measures on this server

#### Scenario: Only on a worker, none there
- **WHEN** it is `worker_only` and no suitable worker is connected
- **THEN** the conversion waits, its page says that no connected worker has proved the volume and that this server does no heavy work, and none of the heavy measurements (the full decode, the packet counts, the copied tracks' data, the sampled pictures) runs on the server

#### Scenario: Evidence of another file
- **WHEN** the digest of the whole output, or of its samples, or of the original's samples, in a worker's measurements differs from the daemon's, or the output's stamp changed since it was handed out
- **THEN** nothing is placed, the output is removed, the original is unchanged, and the conversion fails saying the measurements are not of this file

#### Scenario: Attempts used up
- **WHEN** a check job ends without measurements three times and the mode is `worker_only`
- **THEN** the conversion fails, its output is removed and the original is unchanged

### Requirement: Conversion work waits after a restart
While `convert.wait_after_start` is on (the default), conversion work SHALL be held from the moment the daemon starts until an administrator resumes it: no encode, no check measurement and no sample measurement SHALL be handed to a worker or run on this server, and their jobs SHALL keep their state. The Conversions page, the page of a conversion that waits on it, and the Workers page SHALL say that conversion work is paused since the daemon started, and why, and SHALL offer one button that resumes it. Placing a converted file and recovering a placement in flight, and every other queue (downloads, imports, scans), SHALL NOT be held.

#### Scenario: Restart with a conversion being checked
- **WHEN** the daemon starts with a conversion being checked on this server and the setting on
- **THEN** no check tool runs, the conversion's page says conversion work is paused since the start, and after an administrator resumes it the checks run

#### Scenario: Encodes and measurements wait
- **WHEN** the work is held and a worker asks for work while an encode, a check job and a sample measurement are queued
- **THEN** it is given none of them, and after the resume it is given work again

#### Scenario: Other work goes on
- **WHEN** the work is held
- **THEN** downloads, imports, scans and the recovery of a placement in flight run as usual

### Requirement: A load limit on the server's checks
Every ffmpeg the checks run on this server SHALL be given a bounded thread count, `convert.check_threads` (default 2), for each input's decoder and for its filters, and SHALL run at the lowest CPU priority and the idle I/O class. The daemon's own reading of a whole converted file SHALL run at the lowest CPU priority and the idle I/O class too.

#### Scenario: Threads bounded
- **WHEN** the server measures a converted file's checks with `convert.check_threads` 2
- **THEN** every ffmpeg command of the checks asks for two threads for each input and for its filters

### Requirement: Checks that passed are not judged again
When every check of a converted file has passed, the daemon SHALL record that file (its stamp and a digest of samples of its content, and the whole file's digest when a worker's measurements were bound by it) before anything changes it. A finish job tried again after that (a scan held the volume, a database error, a daemon that stopped) SHALL place that same file after checking again only that it is the file asked for, that the original is unchanged, that it is worth it and that the gates hold, without measuring or binding anything again, whether the checks were measured on this server or on a worker, and also while conversion work is held after a restart. The file SHALL be taken for the same one only when its inode, size and samples are those recorded and its times are those recorded when its checks passed, those Axis recorded after settling it, or (for measurements a worker made, after a stop between settling and that record) the original's modification time with a later change time and, when recorded, the same whole-file digest. Any other file SHALL be checked from the start.

#### Scenario: A scan holds the volume after the checks passed
- **WHEN** the checks of a replacement measured on a worker passed, the output was settled, and the placement could not start because a scan held the volume
- **THEN** the next finish job places that file, its checks 5 to 8 as the worker's measurements found them, and does not reject it

#### Scenario: A stop at every step with the checks on a worker
- **WHEN** a replacement whose checks a worker measured stops before or after any step of settling and placing, with the same name or another container
- **THEN** the next run finishes it and the new file is under the library name

#### Scenario: Nothing heavy again on the server
- **WHEN** every check passed on this server and the placement could not start
- **THEN** the next finish job places the file without decoding it or comparing its pictures again

#### Scenario: The file changed after the checks passed
- **WHEN** the converted file is touched after its checks passed and before it was placed
- **THEN** it is checked from the start, and measurements a worker made are not used for it

#### Scenario: The file changed while conversion work is held
- **WHEN** conversion work is held after a restart and the finish job of a conversion whose checks passed finds that its file is not that file any more
- **THEN** nothing is measured or handed out: the conversion waits, saying that conversion work is paused, and is checked from the start once conversion work is resumed

### Requirement: A plan judged by what it was made from
When a worker takes the encode, the daemon SHALL record what the plan it gives was made from besides the file and the revision: the worker's measured encoders, the software preset and the threads. Check 3 SHALL make the plan again from those, so that a setting or a worker's measurement changed between the encode and the checks makes no difference. When the plan still cannot be made as it was given (Axis itself changed meanwhile), the converted file SHALL NOT be judged: the conversion SHALL end saying so, with no check failed, its output removed and the original unchanged; it SHALL count neither as a passed trial nor as a trial rejected by a check of the converted file, and asking for it again SHALL be allowed.

#### Scenario: A setting changed during a trial
- **WHEN** an administrator changes the software preset while a trial waits for its checks
- **THEN** the trial is judged against the plan it was made with, and passes

#### Scenario: The worker's encoders measured again during a trial
- **WHEN** the worker's measured encoders change while a trial waits for its checks
- **THEN** the trial is judged against the plan it was made with, and passes

#### Scenario: Axis changed between the encode and the checks
- **WHEN** the plan made again from the conversion's records differs from the one the worker was given
- **THEN** the conversion ends "Not judged", saying it is not a rejection by a check; check 3 is recorded as not made; the output is removed and the original unchanged; the revision can still be proven by hand from its trials; and the file can be asked for again

#### Scenario: Handed out before the inputs were recorded
- **WHEN** a conversion handed out before the plan's inputs were recorded waits for its checks and a setting changes meanwhile
- **THEN** the plan is made from the settings and measured encoders of now, and a difference from the plan given ends the conversion "Not judged" as well, never as a rejection by check 3

### Requirement: A rejected trial set aside
An administrator SHALL be able to set aside a trial rejected by a check of the converted file (checks 3 to 8), giving a reason, after which it SHALL count neither for nor against proving its revision; the action SHALL record who did it, when and why, SHALL be logged as an audit line, SHALL need the CSRF token, and SHALL be refused without a reason, for any other conversion and a second time. The database SHALL refuse a set-aside on anything but a trial rejected by checks 3 to 8, one whose failed check is not recorded included. The trial's page SHALL keep showing that it was set aside, by whom, when and why, and the revision's proving page SHALL say how many of its trials were set aside.

#### Scenario: A trial rejected by a check found wrong since
- **WHEN** an administrator sets aside a trial rejected by check 7, saying why
- **THEN** the revision can be proven from its passed trials, the trial's page says it was set aside, by whom and why, and that it counts neither for nor against proving

#### Scenario: Refused
- **WHEN** a trial is set aside without a reason, by a viewer, a second time, or a trial that passed is set aside
- **THEN** it is refused and nothing is recorded

### Requirement: Rejections and passes by a rule found wrong since
Each conversion whose checks of the converted file were made SHALL record the version of those checks' rules it was judged by; one judged before versions were recorded SHALL count as earlier than any. Raising the version SHALL change nothing that was judged. A mark of the build that a check was wrongly strict before a version SHALL make the trials that check rejected, judged before that version, count neither for nor against proving their revision, their pages saying so with the mark's reason; what the later checks would have said of such a file is not known, and it SHALL NOT count as a pass. A mark that a check was too lenient before a version SHALL make the trials that passed every check, judged before that version, count no more towards proving their revision, their pages saying so with the reason, and a finish job tried again for a conversion whose checks all passed before that version SHALL check its file from the start, measuring it again, instead of placing it on that pass. A proof already recorded SHALL NOT be withdrawn by a later mark.

#### Scenario: A check found wrongly strict
- **WHEN** the build marks check 5 wrongly strict before version 2, and a trial was rejected by check 5 judged by version 1
- **THEN** it counts neither for nor against proving its revision, a run paused for it can be resumed, and its page says why; a rejection by check 5 judged by version 2, or by another check, still counts

#### Scenario: A check found too lenient
- **WHEN** the build marks a check too lenient before version 2, and a trial passed every check judged by version 1
- **THEN** it is no trial towards proving its revision and its page says why; a pass judged by version 2 still counts

#### Scenario: A pass by a lenient rule tried again
- **WHEN** the finish job of a replacement whose checks all passed by a rule since marked too lenient is tried again
- **THEN** the file is measured and checked again from the start before it is placed

### Requirement: A name of its own for every claim
The daemon SHALL give the output of every claim of an encode a name of its own in the work area, so that a job given back (which uses no attempt) or a file an earlier claim left never makes a later claim's encode fail. An encode claimed before claims were counted SHALL start counting at the attempt it recorded, so that its next claim takes a name none of its earlier claims can have left.

#### Scenario: A job given back while its output stays
- **WHEN** a worker gives an encode back and a file of that claim's output name stays in the work area
- **THEN** the next claim's output has another name, its encode succeeds and its checks pass

#### Scenario: Claimed before the upgrade
- **WHEN** an encode whose second attempt was named by its attempt is in flight when Axis is upgraded
- **THEN** its claims start at two, and its next claim is named by the third

### Requirement: The original is unchanged when it is retired
After the new file of a replacement into another container is published under its new name, the original SHALL be retired only while it is still the file that was checked (its size, both times, inode and one link). An original changed since SHALL keep its name beside the new file, nothing SHALL be removed, and the conversion SHALL end saying that both are kept and why.

#### Scenario: The original changed while the daemon was down
- **WHEN** a replacement into another container stopped after publishing the new file, and the original was changed in place at the same size before it resumed
- **THEN** the original is kept under its name with its changes, the new file under the new name, and the conversion says that the original changed and both are kept

