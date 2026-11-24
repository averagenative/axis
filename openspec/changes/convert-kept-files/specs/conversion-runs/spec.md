## ADDED Requirements

### Requirement: Kept files are converted only by choice
A conversion run SHALL leave out every file that Axis keeps, with the reason "kept", unless an administrator chose, for that run, to convert kept files. The choice SHALL be offered when a run is planned, off by default, with one sentence saying that a kept file is re-encoded in place after every check passes and stays kept, so that Axis still does not replace it with another release. A run with the choice SHALL decide a kept file exactly as a file that is not kept: the choice SHALL lift the reason "kept" and no other, so a kept file that has another reason to be left out SHALL be left out under that reason. Every conversion the run asks for SHALL carry the choice. Listing, asking and converting SHALL NOT change whether a file is kept. The run's page SHALL say whether kept files are included and, when they are, who chose it and when.

#### Scenario: Off by default
- **WHEN** a run is made without the choice over a library with kept files
- **THEN** every kept file is left out with the reason "kept" and counted under it, as before

#### Scenario: Chosen when the run is planned
- **WHEN** a run is made with the choice
- **THEN** the kept files the profile would convert are in the list, ranked with the others, and no file is left out as kept

#### Scenario: A kept file with another reason
- **WHEN** a run with the choice looks at a kept file that is efficient already, of uncertain HDR, pinned, remembered or in flight
- **THEN** the file is left out under that reason

#### Scenario: The requests carry the choice
- **WHEN** a run with the choice asks for a kept file as a replacement
- **THEN** the conversion is asked for through the single request with the choice recorded, and the file is still kept after the conversion ended

#### Scenario: The same selection with the other choice
- **WHEN** a run of the same selection and profile version that has not started exists, and a run is asked for with the other choice about kept files
- **THEN** a new run is made

### Requirement: The choice stays with the run
The choice to convert kept files SHALL be stored with the run and SHALL NOT be changed by starting, pausing, resuming, setting limits or switching the run from trials to replacing originals. Only an administrator SHALL be able to make it, and the audit log SHALL name who did.

#### Scenario: Trials, then replacing
- **WHEN** a run of trials made with the choice is switched to replacing originals
- **THEN** it has the choice still, with who made it, and asks again for the kept files it tried, as replacements with the choice

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator, or a request without the CSRF token, makes a run with the choice or adds kept files to a run
- **THEN** the response is 403 and nothing changes

### Requirement: Kept files added to a run
A run made without the choice SHALL offer, on the row of the files left out as kept, to add them to the run, naming how many. Doing so SHALL turn the choice on for that run and add those files to its list: each SHALL be decided as the first list decided the others and either listed or left out for its other reason, so that none stays left out as kept. It SHALL be allowed only while the run is ready to start, paused or finished, and has no file in flight; for any other state, for a run with a file in flight and for a run that has the choice already it SHALL be refused, and the page SHALL say what to do instead. The list SHALL be added to and not made again: no entry, conversion, pause or result the run had SHALL change. A file of a unit the list has SHALL join that unit after its files; the others SHALL make new units after the units there were, the largest estimated saving first; a unit a person took out SHALL stay out with its new files; a cap SHALL be worked out again. The run SHALL go back to the state it was in, except that a finished run to which files were added SHALL wait paused, saying who added how many, for a person to resume. If the files cannot be added, the run SHALL be as it was, the choice off, and SHALL say so.

#### Scenario: Added to a run that is ready
- **WHEN** an administrator adds the kept files to a run that is ready to start
- **THEN** the run is listed again as ready with those files after the ones it had, none is left out as kept, and the page says who chose it

#### Scenario: A paused run keeps what it did
- **WHEN** kept files are added to a paused run that has files done
- **THEN** the files done, their conversions and the run's pause are as they were, and the kept files wait after them

#### Scenario: A finished run
- **WHEN** kept files are added to a finished run
- **THEN** the run is paused, saying who added how many, and converts them once a person resumes it

#### Scenario: An episode joins its series
- **WHEN** an episode left out as kept is added and its series is a unit of the list
- **THEN** it is a file of that unit, after the episodes that were listed, and the unit's totals count it

#### Scenario: Refused
- **WHEN** the action is asked for a run that is running, being listed or cancelled, that has a file in flight, or that has the choice already
- **THEN** it is refused and the run is unchanged

#### Scenario: The job ends without adding them
- **WHEN** the job that adds the files fails or is lost
- **THEN** the run is in the state it was in with the list it had and the choice off, and its page says that the files were not added

### Requirement: A run's progress as bars
A run's page SHALL show, with what it already says in words: a progress bar of the files settled (done, skipped or failed) out of the files within its caps, or out of its whole list when it has no cap; a bar of the bytes saved so far against the saving estimated for those files, once a file is done; and, for each file in flight, the bar of its conversion with the stage, the percent, the speed and the time left as a single conversion's page shows them. Each bar SHALL have its words beside it and as its accessible name. The bars SHALL update with the block of the page that follows the run. The dashboard's piece of a run SHALL carry the bar of its files.

#### Scenario: Files of the list
- **WHEN** one file of a run of two is settled
- **THEN** the bar's value is 1 of 2 and the words say so

#### Scenario: Within the caps
- **WHEN** the run has a cap of one file and that file is settled
- **THEN** the bar is 1 of 1, and the words say that it is of the files within the caps

#### Scenario: The file in flight
- **WHEN** a file of the run is being encoded and its worker has reported half the source read
- **THEN** the run's page shows that file's bar at 50 of 100 with the stage, the speed and the time left
