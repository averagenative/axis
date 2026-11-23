# space-estimate Specification

## Purpose
What converting would save, and what may be converted at all: the single eligibility decision that estimates, plans and conversions share, size estimates with honest ranges, calibration from real results, and the Savings pages.
## Requirements
### Requirement: Eligibility with a stated reason
For a file and a target, the engine SHALL decide whether the file is eligible for conversion and, when it is not, SHALL give exactly one reason: the first that applies in this fixed order: not applicable (the target is for another kind of file or library), not present, pinned, not probed, probe failed (or timed out), probe stale (changed since probed, a stamp that is not the index's, or an older probe version), HDR uncertain (uncertain or unclassified), Dolby Vision, HDR10+, HDR source for an SDR-only target, interlaced, field order unknown, stream layout or container the target cannot carry, codec without figures (or lossless not known), lossy source, facts missing, facts inconsistent, already in a more efficient codec, already efficient, album not eligible as a whole, would not shrink enough, volume away. The same decision SHALL be used by estimates, plans and conversions. A fact that is missing or unknown and matters SHALL make the file not eligible with a reason, never eligible.

#### Scenario: Pinned file
- **WHEN** a pinned file is evaluated
- **THEN** it is not eligible, with reason pinned

#### Scenario: Dolby Vision
- **WHEN** a file whose main video stream is Dolby Vision is evaluated against a video target
- **THEN** it is not eligible, with reason Dolby Vision

#### Scenario: Uncertain HDR
- **WHEN** a file flagged as HDR-uncertain, or whose picture has no HDR class, is evaluated against a video target
- **THEN** it is not eligible, with reason HDR uncertain

#### Scenario: Already efficient
- **WHEN** a file is already in the target codec at or below the efficiency threshold
- **THEN** it is not eligible, with reason already efficient

#### Scenario: Lossy audio source
- **WHEN** an audio file with a lossy codec is evaluated against a target that does not explicitly allow lossy sources
- **THEN** it is not eligible, with reason lossy source

#### Scenario: Several reasons apply
- **WHEN** more than one reason applies to a file
- **THEN** the reason reported is the first in the fixed order

#### Scenario: Unknown facts fail closed
- **WHEN** a fact the decision or the estimate needs (the field order, the codec, whether audio is lossless, the duration, the picture size, the frame rate, a bitrate) is unknown
- **THEN** the file is not eligible, with the reason that names what is unknown

#### Scenario: Probe describes older content
- **WHEN** a file changed after it was probed, or its probe record's size, times or inode differ from the index
- **THEN** it is not eligible, with reason probe stale

#### Scenario: Volume away
- **WHEN** a file that would otherwise be eligible is on a volume that is away
- **THEN** it is not eligible now, with reason volume away, and the reports count it as converting from the index as it last was, marked

### Requirement: Size estimate with a range
For an eligible file and a target, the engine SHALL estimate the size after conversion as a low, middle and high figure, accounting for the video stream, each audio and subtitle stream as copied, converted or dropped, and container overhead. The figures SHALL be whole numbers, never negative, with low at most middle and middle at most high.

#### Scenario: Never larger than the source
- **WHEN** a file is estimated
- **THEN** the high figure does not exceed the file's current size

#### Scenario: Would not shrink enough
- **WHEN** a file's estimated saving is below the target's minimum saving
- **THEN** the file is not eligible, with reason would not shrink enough

#### Scenario: No saving is never eligible
- **WHEN** a file, or an album taken as a whole, is not estimated strictly smaller than it is now
- **THEN** it is not eligible, whatever the target's minimum saving, including a minimum of zero

#### Scenario: Absurd sizes do not break totals
- **WHEN** files with sizes near the largest representable value are added up
- **THEN** the total stays at the largest value instead of overflowing

#### Scenario: Copied streams keep their size
- **WHEN** a target copies audio and a file has a 4 GB lossless audio track
- **THEN** the estimate includes that track at its current size

#### Scenario: Dropped tracks
- **WHEN** a target drops audio tracks in other languages
- **THEN** the estimate is smaller by the size of the dropped tracks, and a track of unknown language, or the last one, is never dropped

#### Scenario: Ordering
- **WHEN** one file is estimated at two quality levels of the same target
- **THEN** the higher-quality estimate is not smaller than the lower-quality one

#### Scenario: Larger picture
- **WHEN** two files that differ only in picture size, at the same bits per pixel, are estimated at the same quality level
- **THEN** the larger picture's estimate is not smaller

#### Scenario: Any facts
- **WHEN** a file with any facts at all (missing, out of range, not finite) is evaluated
- **THEN** the decision is one of the reasons and every figure is finite and never negative

### Requirement: Albums are estimated as units
For audio targets, files grouped as one album (the folder two levels below a music or audiobooks root, or the folder of a shallower file) SHALL be eligible or ineligible together, and the minimum saving SHALL apply to the album.

#### Scenario: Mixed album
- **WHEN** one track of an album is ineligible
- **THEN** every track of the album is reported ineligible, with a reason naming the album and the track

#### Scenario: Album not given whole
- **WHEN** fewer files are evaluated together than the index holds in their album
- **THEN** none of them is eligible

### Requirement: Built-in targets
The engine SHALL provide built-in targets for video (Space saver, Balanced, Maximum compatibility) and audio (Keep lossless, Space saver, Maximum compatibility, Audiobooks), each a full combination of codec, profile and bit depth, encoder and quality, audio and subtitle handling and container.

#### Scenario: Compatibility target skips HEVC and HDR
- **WHEN** an HEVC file or an HDR file is evaluated against Maximum compatibility
- **THEN** it is not eligible by default

### Requirement: Calibration from real results
The engine SHALL record real conversion results and measured samples and SHALL use them to correct its estimates, and SHALL state for each target whether its estimates are an "Informed guess", "Partly based on measurements" or "Based on measurements".

#### Scenario: Results move estimates
- **WHEN** enough real results for a kind of file show smaller outputs than estimated
- **THEN** later estimates for that kind of file are smaller

#### Scenario: Uncalibrated is stated
- **WHEN** a target has no real results
- **THEN** its figures are shown as an "Informed guess"

#### Scenario: Calibration thresholds
- **WHEN** a kind of file has no result, fewer than twenty, or twenty or more
- **THEN** its estimates are an "Informed guess", "Partly based on measurements" (blending the prior with the results), or "Based on measurements" (from the results alone)

### Requirement: Sample measurement
An administrator SHALL be able to request a measurement that encodes short excerpts of a few files to a scratch location and records the results as calibration samples. It SHALL read media only through a read guard, SHALL write nothing to any media volume, SHALL leave nothing behind, SHALL run one at a time at the lowest priority, and SHALL work while storage is read-only.

#### Scenario: Measurement records samples
- **WHEN** a measurement completes
- **THEN** samples are recorded and the scratch location is empty

#### Scenario: Media untouched
- **WHEN** a measurement runs
- **THEN** no file under any media root is created, changed or removed

#### Scenario: Cancel
- **WHEN** a measurement is cancelled
- **THEN** encoding stops and the scratch location is emptied

#### Scenario: Killed daemon
- **WHEN** the daemon was killed during a measurement
- **THEN** what it left in the scratch location is removed when the module starts, without following a link out of it

#### Scenario: Unsafe scratch location
- **WHEN** the scratch location is a link, is open to other users, or lies on or under a volume
- **THEN** the measurement does not run and nothing is created there

### Requirement: Savings reports
The daemon SHALL show, for each target, the estimated size after, the saving as an amount and a percentage with its range, for the whole library and per volume, root and category; the projected free space per volume; the number and size of files skipped, by reason; and the files with the largest estimated saving. Reports SHALL state their coverage as analysis reports do, SHALL count a hard-linked file's size once, and SHALL keep files on a volume that is away in every total, marked.

#### Scenario: Totals are sums
- **WHEN** a target's report is computed
- **THEN** its middle total equals the sum of the middle estimates of the eligible files it covers

#### Scenario: Projected free space
- **WHEN** a volume has eligible files
- **THEN** the report shows its free space now and the projected free space after, with a range

#### Scenario: Skips are explained
- **WHEN** files are skipped
- **THEN** the report lists each reason with its file count and total size, linking to those files

#### Scenario: Figures match lists
- **WHEN** a figure that counts files is followed to its file list
- **THEN** the list holds that many files, decided again by the same rules

#### Scenario: Volume goes away
- **WHEN** a volume goes away between two reports
- **THEN** the totals do not change and the volume and its files are marked as last known

#### Scenario: Comparing targets
- **WHEN** the overview is opened
- **THEN** each built-in target is shown side by side with its estimated saving and calibration state

#### Scenario: Controls are an administrator's
- **WHEN** someone who is not an administrator, or a request without the CSRF token, asks for a refresh, a measurement or its cancellation
- **THEN** the request is refused

### Requirement: Estimates on the file page
A file's page SHALL show, for each target of its kind, whether it is eligible, the reason if not, and the estimated size after.

#### Scenario: If converted
- **WHEN** a user opens a probed video file
- **THEN** the page lists each video target with eligibility and the estimated size after

### Requirement: Estimates per profile revision
Estimates, snapshots, calibration state and file lists SHALL be available for every profile, built-in or not, and SHALL name the profile revision they were computed for.

#### Scenario: Custom profile
- **WHEN** an administrator opens the Savings page of a profile they created
- **THEN** it shows that profile's estimates, and its figures link to lists with matching counts

#### Scenario: Revision named
- **WHEN** a Savings page is shown
- **THEN** it states the profile revision its figures were computed for

#### Scenario: Samples shared where the output is the same
- **WHEN** two revisions differ only in parts that cannot change the output
- **THEN** calibration samples recorded for one apply to the other

### Requirement: Real conversions calibrate estimates
Every verified conversion SHALL be recorded as a calibration sample, of origin "converted", with its source facts, profile revision, encoder family and the measured bitrate of its output picture.

#### Scenario: Sample recorded
- **WHEN** a conversion's output passes verification
- **THEN** a calibration sample exists for it

