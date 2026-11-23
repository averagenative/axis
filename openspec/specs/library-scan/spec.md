# library-scan Specification

## Purpose
The file index: how roots are scanned, what is recorded about each file, and the rules that keep the index from ever concluding a file is gone when it merely could not be seen.
## Requirements
### Requirement: File index
The scanner SHALL record every regular file under a library root with its path relative to the root, size, modification time, inode, link count and kind.

#### Scenario: New files are indexed
- **WHEN** a root containing files in nested directories is scanned for the first time
- **THEN** each regular file has a record with correct relative path, size and kind

#### Scenario: Unchanged files are not rewritten
- **WHEN** a root is scanned again with no changes on disk
- **THEN** no file record's content changes and no file is marked changed

#### Scenario: Modified file is flagged
- **WHEN** a file's size or modification time changes between scans
- **THEN** its record is updated and it is marked changed

### Requirement: Scans stay inside their root
The scanner SHALL NOT follow symbolic links and SHALL NOT descend into a directory on a different filesystem than the root.

#### Scenario: Symbolic link to elsewhere
- **WHEN** a root contains a symbolic link pointing outside the root
- **THEN** the link is recorded as a link and nothing at its target is indexed

#### Scenario: Other filesystem inside a root
- **WHEN** a directory inside a root is a mount point of another filesystem
- **THEN** it is skipped and reported in the scan result

#### Scenario: Link loop
- **WHEN** a root contains symbolic links forming a loop
- **THEN** the scan finishes

### Requirement: Any file name is handled
The scanner SHALL index files whose names contain spaces, newlines, quotes, non-UTF-8 bytes or other unusual characters, storing the name so that the exact original bytes can be recovered.

#### Scenario: Non-UTF-8 name
- **WHEN** a file's name contains bytes that are not valid UTF-8
- **THEN** it is indexed, flagged as having an invalid name, and its exact original name can be reconstructed

#### Scenario: Hostile-looking name
- **WHEN** a file name contains quotes, semicolons, markup or newlines
- **THEN** it is stored verbatim and displayed escaped

### Requirement: Offline volumes are never read as empty
The scanner SHALL NOT mark any file missing, or remove any record, because of a scan during which the volume was unavailable at any point.

#### Scenario: Volume offline before the scan
- **WHEN** a scan is requested for a root whose volume is offline
- **THEN** the scan does not run, reports the volume as unavailable, and no record changes

#### Scenario: Volume disappears mid-scan
- **WHEN** a volume goes offline while one of its roots is being scanned
- **THEN** the scan stops, is recorded as incomplete, and no file is marked missing

#### Scenario: Unreadable directory
- **WHEN** a directory cannot be read during an otherwise complete scan
- **THEN** files previously recorded under that directory are not marked missing and the error is reported

### Requirement: Missing files need confirmation
The scanner SHALL mark a file missing only after it was absent in two completed scans at least an hour apart, and SHALL delete its record only 30 days later.

#### Scenario: First absence
- **WHEN** a recorded file is absent in one completed scan
- **THEN** it is marked as suspected missing and its record is otherwise intact

#### Scenario: Second absence
- **WHEN** it is still absent in a later completed scan at least an hour after the first
- **THEN** it is marked missing

#### Scenario: File reappears
- **WHEN** a suspected-missing or missing file is seen again
- **THEN** it returns to normal with its previous record

#### Scenario: Cancelled scan confirms nothing
- **WHEN** a scan is cancelled or interrupted by shutdown
- **THEN** no file's missing status changes because of it

### Requirement: Moves keep identity
The scanner SHALL recognise a file moved or renamed within a volume and keep its record rather than treating it as a deletion and a new file.

#### Scenario: Renamed file
- **WHEN** a file is renamed within a root between scans
- **THEN** the same record now carries the new path

### Requirement: Pinned files
The scanner SHALL mark as pinned every file that has more than one hard link or lies under a downloads root.

#### Scenario: Hardlinked file
- **WHEN** a file has two hard links
- **THEN** its record is marked pinned

#### Scenario: Link removed
- **WHEN** the second link is deleted and the root is scanned again
- **THEN** the file is no longer marked pinned

### Requirement: Classification and ignored names
The scanner SHALL classify each file by kind from its extension, and SHALL skip built-in and user-defined ignored names without indexing them.

#### Scenario: Kinds
- **WHEN** a root contains video, audio, subtitle, image and unknown files
- **THEN** each is recorded with the matching kind, case-insensitively by extension

#### Scenario: Partial download is not indexed
- **WHEN** a root contains a file with a partial-download suffix
- **THEN** it is not indexed and is counted as ignored in the scan result

### Requirement: Restartable, bounded scans
A scan SHALL be safe to interrupt at any moment, SHALL use memory independent of the number of files, and SHALL run at most one at a time per volume.

#### Scenario: Killed mid-scan
- **WHEN** the daemon is killed during a scan and restarted
- **THEN** the index is consistent and the next scan completes normally

#### Scenario: One scan per volume
- **WHEN** scans are requested for two roots on the same volume
- **THEN** they run one after the other, while a root on another volume may scan at the same time

#### Scenario: Large tree
- **WHEN** a root with several hundred thousand files is scanned
- **THEN** the scan completes with memory use that does not grow with the file count

### Requirement: The work area is not indexed
The scanner SHALL NOT index or report as leftovers anything in a volume's work area.

#### Scenario: Temporary file present
- **WHEN** a scan runs while a temporary output exists in the work area
- **THEN** no index record is created for it

