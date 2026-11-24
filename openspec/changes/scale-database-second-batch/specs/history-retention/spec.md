## ADDED Requirements

### Requirement: Each kind of history has a keeping time
Axis SHALL have four settings, on a **History settings** card of the Settings page, that say how long history is kept: the events of movies and series (`history.keep_events`), the records of grabs (`history.keep_grabs`), finished downloads (`history.keep_downloads`) and finished imports (`history.keep_imports`). Each SHALL default to 365 days, SHALL accept 30 days to 3,650 days, and SHALL accept 0, which means kept forever. Age SHALL be counted from when the row ended (an event's time, a grab's closing, a download's or an import's finish), never while it is open.

#### Scenario: The defaults
- **WHEN** Axis is installed or upgraded and nothing is set
- **THEN** each of the four keeping times is 365 days

#### Scenario: Kept forever
- **WHEN** an administrator sets a keeping time to 0
- **THEN** nothing of that kind is deleted, whatever its age

#### Scenario: Too short
- **WHEN** an administrator enters 10 days
- **THEN** the page refuses it and says that it must be 0 or at least 30 days

### Requirement: Old history is deleted in bounded batches
An upkeep job, run every hour, SHALL delete the history that is older than its keeping time. Each statement SHALL look at a bounded number of rows (1,000; 250 imports, each of which takes its files with it), found through an index on when they ended, and SHALL be its own transaction. A run SHALL send at most 50 statements for each table; what is left SHALL wait for the next run. No statement SHALL read a history table as a whole. A row that is kept SHALL NOT stand in front of rows that may go: the next statement goes on after it.

#### Scenario: A backlog after an upgrade
- **WHEN** half of 3,000,000 events are older than the keeping time
- **THEN** a run deletes about 50,000 of them in statements of a few milliseconds each, and later runs the rest

#### Scenario: Kept rows do not block
- **WHEN** the oldest rows of a table are all rows that are kept for good
- **THEN** the run goes past them and deletes the rows behind them that may go

### Requirement: Nothing that is still needed is deleted
The deletion SHALL never delete a row that another row names, by a foreign key or by a plain column; a reference that the database would set to NULL is not leave to delete. It SHALL never delete a row that is open or pending, nor one that explains the present state of a title that exists. In particular it SHALL keep, whatever their age:

- of the events: the 30 latest of each movie and each series (and of each title that was removed), the events that say a title was added or removed, and every event of something a person did;
- of the imports: one that is not ended (waiting, running, blocked or partly imported); one that a file in the library came by; one that retired a file to the recycle folder; one with work left at its download client or in its download's folder, or whose torrent is left seeding; one with a file still under a temporary name; the latest of each movie and series; one with a job queued or running;
- of the grabs: one that is open; one whose download is still to be removed from its client; one that a blocklist entry, an event or an import names; one whose download is still at its client; a series' grab whose import left episodes without a file (the search reads it so as not to grab that release again); the 5 latest of each movie and series;
- of the downloads: one that is still at its client; one that a grab or an import names; one whose folder is still to be looked at.

A grab SHALL go only after the events and imports that named it, and a download only after the grabs and imports that named it. The files a recycled import retired (`import_recycle`) SHALL NOT be deleted by this job.

#### Scenario: A file in the library keeps its record
- **WHEN** a movie's file came by an import three years ago
- **THEN** that import, its grab and its download are kept

#### Scenario: A title's page keeps its history
- **WHEN** a series has events from three years ago and none since
- **THEN** its 30 latest events are still on its page

#### Scenario: What a person did stays
- **WHEN** an administrator blocked a release two years ago
- **THEN** the event that says so is kept, and so is the grab the blocklist entry names

#### Scenario: A row that went is gone with what was its own
- **WHEN** an old import that nothing names is deleted
- **THEN** its files, its actions and its assignments go with it, and nothing else
