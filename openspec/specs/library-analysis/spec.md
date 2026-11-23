# library-analysis Specification

## Purpose
What the library is made of and where the space went: reports computed from the index, each stating what it could not see and linking every figure to the files behind it.
## Requirements
### Requirement: Space reports
The daemon SHALL report used space by volume, by root and by category, with each volume's capacity, free space and fill level.

#### Scenario: Totals agree with the index
- **WHEN** a report is computed for a library with known contents
- **THEN** each total equals the sum of the sizes of the files it covers

#### Scenario: Hardlinks counted once
- **WHEN** two indexed paths on one volume are the same file
- **THEN** its size is counted once in every space total, whichever roots the paths are in

#### Scenario: Only present files are counted
- **WHEN** a file was not found by the last complete scan of its root (suspected or missing)
- **THEN** it is in no total or breakdown, and the coverage lists it on its own line

### Requirement: Composition reports
The daemon SHALL report the library's composition by video codec, resolution class, HDR format, container and audio codec, by file count and by bytes, and the distribution of video bitrate within each resolution class.

#### Scenario: Breakdown
- **WHEN** the composition report is opened
- **THEN** each codec and resolution class is listed with its file count and total size

#### Scenario: Outliers
- **WHEN** a file's video bitrate for its resolution is above the configured threshold for its codec
- **THEN** it appears in the list of files that are large for their class

### Requirement: Largest items
The daemon SHALL list the largest files and the largest folders.

#### Scenario: Largest files
- **WHEN** the report is opened
- **THEN** files are listed in descending order of size with their volume and path

### Requirement: Likely duplicates
The daemon SHALL list groups of files that are likely the same content, and groups that are likely the same title, labelled as likely, showing the evidence, ranking the copies, and stating the space the lesser copies occupy. It SHALL NOT delete or change any file as a result.

#### Scenario: Same content
- **WHEN** two files on different paths that are not hardlinks of each other have identical size, duration and stream layout
- **THEN** they are listed as one likely-duplicate group

#### Scenario: Same title, different quality
- **WHEN** two files in the same category normalise to the same title key and differ in resolution
- **THEN** they are grouped, the higher-resolution copy is ranked first, and the size of the other is shown

#### Scenario: Hardlinks are not duplicates
- **WHEN** two paths are hardlinks of one file
- **THEN** they are not listed as duplicates

#### Scenario: Different titles stay apart
- **WHEN** two files have similar but different titles or different years
- **THEN** they are not grouped

### Requirement: Items split across volumes
The daemon SHALL list series and albums whose files lie on more than one volume, with the size on each.

#### Scenario: Split series
- **WHEN** episodes of one series are on two volumes
- **THEN** the series is listed with the bytes on each volume

### Requirement: Leftovers
The daemon SHALL list files that match its leftover rules (samples, extras, subtitles without a matching video, known junk, empty folders) with their total size.

#### Scenario: Orphaned subtitle
- **WHEN** a subtitle file has no video file with a matching name beside it
- **THEN** it is listed as a leftover

#### Scenario: Subtitle beside its video
- **WHEN** a subtitle file sits beside a video with a matching name
- **THEN** it is not listed

### Requirement: Stated coverage
Every report SHALL state what it could not see: files not probed, failed probes, roots never fully scanned and volumes offline when it was computed. Files on an offline volume SHALL be included from their last known state and marked.

#### Scenario: Partial data is labelled
- **WHEN** some files have not been probed
- **THEN** reports that depend on probe data show how many files are excluded

#### Scenario: Offline volume does not change totals
- **WHEN** a volume goes offline between two refreshes
- **THEN** library totals are unchanged and the volume's figures are marked as last known

#### Scenario: Uncertain and out-of-date probes are labelled
- **WHEN** some files have an HDR class the probe was not sure of, or changed after they were probed
- **THEN** the reports that depend on them say how many, with a link to those files

### Requirement: Snapshots and history
Reports SHALL be computed by a background job into snapshots, shown with the time they were taken, refreshed after scans and on an administrator's request, and retained so that growth over time can be shown.

#### Scenario: Snapshot time shown
- **WHEN** a report page is opened
- **THEN** it shows when its figures were computed

#### Scenario: Refresh does not block scanning
- **WHEN** a refresh runs while a scan is in progress
- **THEN** the scan is not blocked, and the refresh's figures describe the index as it was when the refresh began

#### Scenario: Refresh waits for scans and probes to settle
- **WHEN** a scan is running, a probe is queued, or the last of them ended only moments ago
- **THEN** no refresh is started on schedule; it is started once they have settled, at most once per configured interval

#### Scenario: Refresh now is for administrators
- **WHEN** a user who is not an administrator, or a request without the CSRF token, asks for a refresh
- **THEN** it is refused and nothing is queued

#### Scenario: Retention
- **WHEN** snapshots are older than the retention settings
- **THEN** the last snapshot of each day is kept for the longer period, every other for the shorter, and the newest is always kept

#### Scenario: Growth
- **WHEN** snapshots from several days exist
- **THEN** the overview shows used space over that period

### Requirement: Figures link to files
Every figure SHALL link to the list of files it counts.

#### Scenario: Following a figure
- **WHEN** a user follows the link on a figure
- **THEN** the file list opens with the matching filter and the same count

#### Scenario: Names are text
- **WHEN** a file, folder, root or tag name holds markup or punctuation
- **THEN** it is shown as escaped text, never in a link or an attribute, and no page carries an inline style or script

