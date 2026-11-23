# library-browser Specification

## Purpose
Looking at the library: the pages that show what is on each volume, walk its folders, list and filter its files, and explain each file's state, at a cost that does not grow with the size of the library.
## Requirements
### Requirement: Volumes and roots overview
The daemon SHALL provide a page listing every volume with its state, capacity, used and free space, and under it each root with its file count, total size and the time of its last completed scan.

#### Scenario: Overview
- **WHEN** a logged-in user opens the Library page
- **THEN** each volume and root is listed with its state, totals and last completed scan time

#### Scenario: Offline volume
- **WHEN** a volume is offline
- **THEN** it is listed as offline with its last known totals, and its actions are unavailable

### Requirement: Folder view
The daemon SHALL let a user walk a root's folders, showing for each folder its direct children with file counts and total sizes as of the last completed scan.

#### Scenario: Walking down
- **WHEN** a user opens a folder
- **THEN** its sub-folders and files are listed with sizes, and a breadcrumb leads back to the root

#### Scenario: Totals reflect completed scans only
- **WHEN** a scan of a root is interrupted
- **THEN** folder totals for that root are unchanged from the last completed scan

#### Scenario: Folders with nothing in them
- **WHEN** a folder holds no present file, directly or in any folder below it
- **THEN** it is not listed, and the totals of the folders above it do not count it

### Requirement: Filterable file list
The daemon SHALL provide a list of files across all roots that can be filtered by kind, container, video codec, resolution class, HDR format, audio codec, pinned state, probe state, size range and name, sorted by a supported column, and paged.

#### Scenario: Filter
- **WHEN** a user filters by video codec HEVC and resolution class 2160p
- **THEN** only files whose main video stream matches both are listed

#### Scenario: Unknown filter refused
- **WHEN** a request carries a filter name or value the daemon does not define
- **THEN** the response is 400

#### Scenario: Name text too short to search
- **WHEN** a request filters by a name of fewer than three characters
- **THEN** the response is 400

#### Scenario: Stable paging
- **WHEN** files are added to the index while a user pages through a list
- **THEN** no file already shown is repeated and none between the pages is skipped

#### Scenario: Nothing from a name in a link
- **WHEN** a page lists files and folders
- **THEN** every link and cursor in it is made of numbers and fixed text, and names appear only as text

### Requirement: File detail
The daemon SHALL provide a page for each file showing its location, size, times, link count, whether and why it is pinned, its scan state, and each probed stream with its properties, or the probe error.

#### Scenario: Streams
- **WHEN** a user opens a probed video file
- **THEN** the page lists its video, audio and subtitle streams with codec, dimensions or channels, language and HDR format

#### Scenario: Pinned reason
- **WHEN** a user opens a pinned file
- **THEN** the page states why it is pinned

#### Scenario: Probe failure
- **WHEN** a user opens a file whose probe failed
- **THEN** the page shows the recorded error

### Requirement: File names are shown safely and exactly
File and folder names SHALL be escaped in every page, and a name that is not valid UTF-8 SHALL be listed, openable, and shown in an exact escaped form on its detail page.

#### Scenario: Markup in a name
- **WHEN** a file name contains HTML markup
- **THEN** it is displayed as text

#### Scenario: Name that is not UTF-8
- **WHEN** a file name is not valid UTF-8
- **THEN** the file appears in lists with a marker, its detail page opens, and the page shows the exact bytes in escaped form

### Requirement: Library actions
An administrator SHALL be able to request a scan of a root and a re-probe of a file or of the files under a folder; other users SHALL NOT. These actions SHALL only create jobs.

#### Scenario: Scan now
- **WHEN** an administrator requests a scan of a root twice in quick succession
- **THEN** one scan job is queued

#### Scenario: Re-probe folder is capped
- **WHEN** an administrator requests a re-probe of a folder holding more files than the cap
- **THEN** probes are queued up to the cap and the page says the request was capped

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator sends an action request
- **THEN** the response is 403 and no job is created

#### Scenario: Offline volume refused
- **WHEN** an administrator sends an action request for a root, folder or file on a volume that is not online
- **THEN** the response is 409 and no job is created

### Requirement: Live library state
The Library pages SHALL show scan progress and volume state changes without a reload.

#### Scenario: Scan progress
- **WHEN** a scan of a root is running while the overview is open
- **THEN** the root shows the scan's progress as it advances

#### Scenario: List changed notice
- **WHEN** a scan of a root completes while a list of its files is open
- **THEN** the page shows a notice offering to reload the list

