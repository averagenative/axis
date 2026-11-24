## ADDED Requirements

### Requirement: Kept files are converted only by choice
A conversion run SHALL leave out every file that Axis keeps, with the reason "kept", unless an administrator chose, for that run, to convert kept files. The choice SHALL be offered when a run is planned, off by default, with one sentence saying that a kept file is re-encoded in place after every check passes and stays kept, so that Axis still does not replace it with another release. A run with the choice SHALL decide a kept file exactly as a file that is not kept: the choice SHALL lift the reason "kept" and no other, so a kept file that has another reason to be left out SHALL be left out under that reason. The choice SHALL cover the files that are kept when the list is made (or when they are added to it), and each conversion the run asks for of a file it covers SHALL carry the choice; a file of the list that was not kept then and is kept when its turn comes SHALL be refused by the condition "kept" as without the choice, skipped with that reason, and nothing SHALL be asked for it. Listing, asking and converting SHALL NOT change whether a file is kept. The run's page SHALL say whether kept files are included and, when they are, who chose it and when.

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

#### Scenario: A file that is not kept is asked for without it
- **WHEN** a run with the choice asks for a file of its list that is not kept
- **THEN** the conversion records no choice, so a mark set on the file later refuses it when a worker takes it or just before placement

#### Scenario: Kept after the list was made
- **WHEN** a file of the list of a run with the choice was not kept when the list was made, and is kept when its turn comes
- **THEN** it is refused by the condition "kept", skipped with that reason, nothing is asked for it, and the run goes on with the next file

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
A run made without the choice SHALL offer, on the row of the files left out as kept, to add them to the run, naming how many. Doing so SHALL turn the choice on for that run and add those files to its list: each SHALL be decided as the first list decided the others and either listed or left out for its other reason, so that none stays left out as kept. It SHALL be allowed only while the run is ready to start, paused or finished, and has no file in flight; for any other state, for a run with a file in flight and for a run that has the choice already it SHALL be refused, and the page SHALL say what to do instead. The list SHALL be added to and not made again: no entry, conversion, pause or result the run had SHALL change. A file of a unit the list has SHALL join that unit after its files; the others SHALL make new units after the units there were, the largest estimated saving first; a unit a person took out SHALL stay out with its new files; a cap SHALL be worked out again. On a run that replaces originals, the action SHALL say that the originals of the N files are replaced by their converted files once every check passes, with the same sentence and the same box to tick as the switch to replacing originals; without the tick it SHALL be refused and nothing SHALL change, and with it each file added SHALL record who confirmed that its original is replaced and when, and its replacement SHALL be asked for as that person, not on the run's earlier confirmation. On a run of trials the action SHALL say that the files are added as trials, and SHALL need no tick. The run SHALL go back to the state it was in and SHALL NOT start by itself: a run that is ready waits to be started, a paused one to be resumed. A finished run to which files were added that it can convert SHALL wait paused, saying who added how many, for a person to resume; one that can convert none of them (beyond its caps, in a unit taken out, or its time budget used up) SHALL stay finished and say why. The control that resumes a run that replaces originals SHALL say how many originals resuming it will replace. If the files cannot be added, the run SHALL be as it was, the choice off, and SHALL say so.

#### Scenario: Added to a run that is ready
- **WHEN** an administrator adds the kept files to a run that is ready to start
- **THEN** the run is listed again as ready with those files after the ones it had, none is left out as kept, and the page says who chose it

#### Scenario: A paused run keeps what it did
- **WHEN** kept files are added to a paused run that has files done
- **THEN** the files done, their conversions and the run's pause are as they were, and the kept files wait after them

#### Scenario: A finished run
- **WHEN** kept files are added to a finished run
- **THEN** the run is paused, saying who added how many, and converts them once a person resumes it

#### Scenario: A run that replaces originals, without the tick
- **WHEN** the kept files are added to a run that replaces originals by a form without the confirmation ticked
- **THEN** it is refused, and the run, its list and its choice are as they were

#### Scenario: A run that replaces originals, with the tick
- **WHEN** an administrator adds the kept files to a run that replaces originals, ticking that their originals are replaced
- **THEN** each file added records who confirmed it and when, the run waits paused without starting by itself, its resume control says how many originals it will replace, and once resumed each file added is asked for as a replacement with the choice, as the person who confirmed it

#### Scenario: A run of trials
- **WHEN** the page offers to add the kept files to a paused or finished run of trials
- **THEN** it says that they are added as trials, and asks for no tick

#### Scenario: A finished run that can convert none of them
- **WHEN** kept files are added to a finished run and every one of them is beyond its caps
- **THEN** the run stays finished and says why

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
