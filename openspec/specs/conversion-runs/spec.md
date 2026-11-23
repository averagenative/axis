# conversion-runs Specification

## Purpose
Define how Axis converts a library in bulk: a ranked list of files by expected saving, a review before the start, and a run that handles one file at a time, from trials to replacing, and pauses safely on trouble.
## Requirements
### Requirement: A suggested list ranked by saving
For a chosen scope and profile revision the daemon SHALL make a list of the scope's video files that the estimator would convert with that revision, decided by the estimator's own loader and decision function (no other estimate), each with its size now, its estimated size after (low, middle, high) and its saving, ranked by unit with the largest estimated saving first, with the list's totals. The list SHALL be made by a job, a batch of files at a time, in one transaction, so that it appears whole and describes one moment; making it SHALL convert nothing.

#### Scenario: Ranked by saving
- **WHEN** a list is made of four movies the estimator would convert
- **THEN** they are listed by estimated saving, largest first, each with the estimator's own figures, and nothing is converted

#### Scenario: A file on a volume that is away
- **WHEN** the list is made while a volume is offline
- **THEN** its files that would be converted are in the list, and wait for the volume when the run reaches them

#### Scenario: A list job that ends without a list
- **WHEN** a run's list job fails or is cancelled
- **THEN** the run is marked as not listed, saying why, and nothing of it is kept

### Requirement: Files left out keep their reason
Every file of the scope that is not in the list SHALL be kept with exactly one reason: remembered with that revision as not worth it, converted with that revision already, another conversion of it in flight (checked before the estimate), or the estimator's reason. The files left out SHALL be counted by reason, and a person SHALL be able to look at the files of each reason.

#### Scenario: Files left out keep their reason
- **WHEN** the scope holds a hard-linked file, a file whose HDR is uncertain, an already efficient one, an unprobed one, one remembered as not worth it and one with a conversion in flight
- **THEN** each is counted under its reason and listed for that reason, and none is in the list

### Requirement: A series is one unit
For TV the unit of the list SHALL be a series: every episode file identification linked to one series, in whichever TV root, ranked together by their total estimated saving and listed in season and episode order (a file whose name holds no episode last); a TV folder that is no identified series SHALL be taken as one series; any other file, a movie among them, SHALL be a unit of its own. The run SHALL convert a unit's files one after another before it begins the next unit, and SHALL go on with the unit it works through before any other. Each unit SHALL show how many of its files are done.

#### Scenario: A series is one unit
- **WHEN** a TV list holds two identified series, a folder that is no identified series and a file at the root's top
- **THEN** the units are the two series, the folder and the file, ranked by their total saving, and each series' episodes are in season and episode order

#### Scenario: A series is finished before the next
- **WHEN** a run with one file at a time works through two series
- **THEN** it asks for every episode of the first before any of the second, and the first series shows "1 of 3 episodes done" while its second is converted

#### Scenario: A series started is finished first
- **WHEN** the run went on with a second series while the first one's files waited, and the first one's files can be asked for again
- **THEN** the run finishes the second series before it goes back to the first

### Requirement: Scopes
A run SHALL be made of every movies library, every TV library, chosen movies, TV or other libraries, or one series on disk, by an administrator, with a usable video profile revision; a scope that is none of these SHALL be refused.

#### Scenario: Scopes
- **WHEN** a run is made of the Movies library alone, and another of one series
- **THEN** the first lists the library's movies only and the second that series' episodes only

#### Scenario: A scope that is none
- **WHEN** a run is asked for a series that is not on disk, no library, or with no person
- **THEN** it is refused and nothing is stored

### Requirement: Review before the start
Before a run starts a person SHALL be able to look at its list, take units or files out of it, cap it by a number of files or an estimated saving, give it a time budget and hours to work in, and set how many files it asks for at once. A cap SHALL keep a series whole: the list SHALL end after the unit in which the cap is reached. A time budget SHALL count from the start; once it is used up the run SHALL ask for nothing more and SHALL finish when nothing of it is in flight. Outside its hours the run SHALL ask for nothing.

#### Scenario: Caps keep a series whole; a time budget
- **WHEN** a TV run whose first series has three episodes is capped at two files
- **THEN** its list ends after that series, and the run finishes when the three are done; and a run whose time budget is used up asks for nothing and finishes once its file in flight ends

#### Scenario: Hours
- **WHEN** a run's hours do not include now
- **THEN** it waits, saying between which hours it works, and asks once they do

### Requirement: One file at a time, through the single request
A running run SHALL ask for each file of its list, in its order, through the single conversion's request with exactly its gates, and each file SHALL become an ordinary conversion with its own record, checks and page, naming the run and the person who started it (trials) or confirmed replacing (replacements). A run SHALL ask for one file at a time unless a person sets more (at most eight), and never for more at once than the connected workers can take. A run SHALL add no other way of asking for or replacing a file.

#### Scenario: One file at a time, through the single request
- **WHEN** a run is started
- **THEN** it asks for its first file, an ordinary conversion of the run waiting for a worker, and for nothing more until that conversion ends; then the next, in the list's order; and it finishes when its list is done

#### Scenario: Never more than the workers take
- **WHEN** a run may ask for two files at once and the one connected worker takes one job at a time
- **THEN** it has one file in flight, and two once the worker takes more

### Requirement: Trials, then replacing, with the same list
A run with a locked revision SHALL make trials only, SHALL ask for no more trials than proving the revision by hand needs, and SHALL pause once that many trials of the revision passed every check, saying that a person must play them and prove the revision. Once the revision is proven and its profile's current one, a person SHALL be able to switch the run to replacing originals, ticking the single replacement's own sentence, without a new list; the files it converted as trials SHALL be asked for again as replacements. Starting a run as replacing SHALL need what a single replacement needs. A trial rejected by a check of the converted file SHALL pause a run of trials.

#### Scenario: Trials, then replacing, with the same list
- **WHEN** a run with a locked revision has made as many passed trials as proving needs
- **THEN** it pauses saying so, cannot replace while the revision is locked or without the box ticked, and once a person proved the revision and switched it, it asks for the files of its trials again as replacements by that person

#### Scenario: A trial rejected by a check of the converted file
- **WHEN** a trial of a run is rejected by check 4
- **THEN** the run pauses, saying that the revision cannot be proven from its trials

### Requirement: Failures and refusals in a row pause the run
A running run SHALL pause itself, saying why and naming the last file, when as many files in a row as `convert.run_failures` (default 3) were rejected by a check of the converted file or failed, and when twenty files in a row were refused when it asked for them. A file done SHALL start both counts again, and so SHALL a person's resume. A file skipped for its own matter (not worth it, changed, a gate before placement) SHALL NOT count as a failure.

#### Scenario: Failures in a row pause the run
- **WHEN** three files in a row fail or are rejected by checks of the converted files, after one not worth it and one done
- **THEN** the run pauses after the third, saying "3 files in a row failed or were rejected", the one not worth it skipped with its reason

#### Scenario: Refusals in a row pause the run
- **WHEN** every file of a list is refused when asked for
- **THEN** the run pauses after twenty, the rest of the list still waiting

### Requirement: The run pauses itself for a person
A running run SHALL pause itself, saying why, when the encoder of a worker crashed on one of its files (the worker paused itself), when the player check stops saying safe for a library or the revision cannot replace any more, and, with trials, when the revision was proven meanwhile; the file it was asking for SHALL stay in the list.

#### Scenario: An encoder crash pauses the run
- **WHEN** a worker paused itself after its encoder crashed on a file of the run
- **THEN** the run pauses, naming the worker, the file and what the worker said, and asks for nothing more

#### Scenario: The player check stops saying safe
- **WHEN** a run replacing originals asks for a file of a library whose verdict is no longer safe
- **THEN** the request is refused by that gate, the run pauses with the gate's words, and the file waits in the list

### Requirement: The run waits by itself
A running run SHALL ask for nothing and SHALL say why while conversion work is held after the daemon started, while storage is read-only, while no suitable worker is connected (heard from in the last minute, not paused, not revoked, of the daemon's protocol, with a volume proved), and outside its hours; it SHALL go on by itself when the condition clears. It SHALL never have this server convert or check in a worker's place. The files of a run in flight SHALL be recorded as they end while it waits.

#### Scenario: No worker, no request
- **WHEN** no worker is connected, or the one connected is paused
- **THEN** the run asks for nothing and says that it waits for a worker, and asks once one is connected

#### Scenario: Read-only storage holds the run, by itself
- **WHEN** storage is read-only
- **THEN** the run waits and says so, and asks once it is not

#### Scenario: A restart holds the run
- **WHEN** the daemon starts again with a file of a running run in flight and conversion work held
- **THEN** the run asks for nothing and says why, records the file in flight when it ends, and goes on once a person resumes conversion work

### Requirement: An offline volume freezes its files
A volume that is not online, is below its free-space floor, or that no connected worker has proved SHALL freeze its files in the run: they SHALL stay in the list, not skipped and not failed, the run SHALL go on with other volumes' files and SHALL say which volumes its files wait for and why, and it SHALL ask for them again when the volume can be used.

#### Scenario: An offline volume freezes its files
- **WHEN** the disk of a run's files goes away
- **THEN** the run asks for nothing on it, marks none of its files skipped or failed, says the volume is offline, and asks again once it is back

#### Scenario: A volume below its floor freezes its files
- **WHEN** the free-space gate refuses a file
- **THEN** the file stays in the list, the volume's files wait, and the run says the volume is too full

### Requirement: A file's own matter is skipped
A file that, when the run reaches it, is refused for its own matter (not eligible any more, remembered, changed since the scan, gone from the index, another conversion in flight, a name taken, a file that cannot be opened) SHALL be skipped with the gate's or the request's words, and the run SHALL go on with the next. A file whose encode keeps failing, such as one that cannot be read, SHALL be marked failed and not asked for again.

#### Scenario: A file's own matter is skipped
- **WHEN** of a run's list one file is no longer eligible, one is remembered as not worth it, one was taken out and one is gone from the index
- **THEN** the run skips each with its reason and asks for the next file

### Requirement: Stopping is safe
A person SHALL be able to pause a running run (nothing more is asked for; what is in flight finishes and is recorded), resume it, and cancel it; cancelling SHALL cancel its conversions that wait for a worker or are being converted as any cancelled conversion is, and SHALL leave those being checked or put in place to finish. One run SHALL ask for files at a time: starting or resuming another while one runs SHALL be refused.

#### Scenario: Stopping is safe
- **WHEN** a person pauses a run with two files in flight, resumes it, and cancels it with one file waiting for a worker and one being checked
- **THEN** the paused run asks for nothing while the first ends; the cancelled run's waiting conversion is cancelled and its file skipped, and the one being checked finishes and is recorded

#### Scenario: One run at a time
- **WHEN** a second run is started while one runs
- **THEN** it is refused, naming the running one

### Requirement: Real savings beside the estimates
A run SHALL show, for its files done, their size before, the estimate after and the real size after; and for its revision, over every finished conversion with an estimate, the same. Nothing of it SHALL be fed back into the estimator.

#### Scenario: Real savings beside the estimates
- **WHEN** two files of a run are done
- **THEN** its summary shows their size before, their estimate and their real size, the revision's ratio over its finished conversions, and no calibration sample is added by the run

### Requirement: Conversion run pages
Administrators SHALL have, under Library: the list of runs; planning a run (its scope and profile); a run's page (what it does, where it is and why it is paused or waiting, its progress, what was saved against the estimate, its files in flight linked to their conversions, the volumes whose files wait, its list a page of units at a time, what is not in it by reason, the last files settled and why, the start, pause, resume, cancel, replacing and limits); a unit's page with its files; and the files left out for a reason. Where the run is SHALL refresh by itself while it is listed or runs. A conversion's page SHALL name the run that asked for it. The Dashboard SHALL show the running run, else the newest paused one, and a paused run as something to look at. Every page SHALL be an administrator's, every action SHALL need the CSRF token and SHALL be decided again on the server, and no JavaScript beyond the vendored htmx SHALL be used.

#### Scenario: Planning a run; the review; starting; where it is
- **WHEN** an administrator plans a run, reviews its list and starts it with trials
- **THEN** the review shows the list ranked, what is left out and why, and no replacing while the revision is locked; the running run shows its file in flight linked to an ordinary conversion page that names the run; a viewer is refused and a form without its token is refused

#### Scenario: Pause, resume, cancel; trials to replacing; the dashboard
- **WHEN** a run is paused
- **THEN** the Dashboard shows it and lists it as something to look at, and once its revision is proven its page offers replacing originals with the single replacement's sentence and box, refusing it without the box

#### Scenario: A series with its episodes
- **WHEN** a run of one series is reviewed
- **THEN** its page shows "0 of 3 episodes done" and the series' page lists S01E01, S01E02 and S01E03 in order

### Requirement: The list works for a large library
Making the list SHALL take a number of statements that grows with batches of files, not with files, and every page of a run (its summary with a page of units, a unit with its files, the files left out for a reason, the list of runs) SHALL take a bounded number of statements wherever it is in the list.

#### Scenario: The list works for a large library
- **WHEN** a run is made of 50,000 files in 5,000 movies and 1,500 series
- **THEN** the list is made in about a hundred statements, a run's page at the top of the list and at its end costs the same statements, a series' page the same for the first and the last series, and paging reaches every unit once

### Requirement: A file in flight that no worker can take
A running run SHALL NOT hold a place for a conversion of it that waits for a worker on a volume that is not online or that no connected worker has proved: it SHALL give that conversion back (nothing was made of it), the file SHALL wait in the list again, not skipped and not failed, the run SHALL go on with other volumes' files and SHALL name the volume among those whose files wait, saying why, and it SHALL ask for the file again when the volume can be used.

#### Scenario: The only worker stops proving a disk
- **WHEN** a run with one file at a time has asked for a file of disk one, and its only worker, still connected, stops proving disk one
- **THEN** that conversion is given back, the run asks for a file of disk two, names disk one as waiting for a worker that has proved it, and asks for the first file again once one has

### Requirement: A file not judged is asked for again
A conversion of a run that ended not judged (its plan could not be made again as the worker was given it) SHALL NOT count as a failure: the file SHALL wait in the list again and be asked for again, counted as a refusal in a row.

#### Scenario: Not judged
- **WHEN** a run's conversion ends not judged
- **THEN** the run asks for the same file again, and no failure in a row is counted

### Requirement: A run paused for a bad trial goes on once nothing counts
A run of trials paused because a trial was rejected by a check of the converted file SHALL be resumable once no trial of its revision counts against proving it (each such trial set aside, or judged by a check's rule changed since); while one still counts, resuming SHALL be refused, saying how many and what can be done.

#### Scenario: Set aside, then resumed
- **WHEN** a run of trials paused after a trial was rejected by check 4, its resume is refused, an administrator sets that trial aside, and the run is resumed
- **THEN** the run goes on and asks for its next file

### Requirement: A request linked to the conversion asked for
A request of a run that committed before the step recorded it SHALL be linked at the next step only to a conversion of that file's entry requested since the entry was marked asked, the newest of them.

#### Scenario: Stopped between a request and its record, after trials
- **WHEN** a run moved from trials to replacing, asked for a file again, and the step stopped after the replacement's request committed and before it was recorded
- **THEN** the next step links the entry to the replacement in flight, not to the file's trial

