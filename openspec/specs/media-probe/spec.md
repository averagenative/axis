# media-probe Specification

## Purpose
What Axis knows about the inside of a media file: how ffprobe is run safely, the stream model it fills, and classification that says "uncertain" rather than guess.
## Requirements
### Requirement: Probed facts
The engine SHALL record, for each indexed video or audio file, its container, duration and bitrate, and for each stream its type, codec and the properties relevant to that type.

#### Scenario: Video file
- **WHEN** a file with one video stream, two audio streams and two subtitle streams is probed
- **THEN** the record has the container and duration, the video stream's codec, resolution, bit depth and frame rate, each audio stream's codec, channel layout and language, and each subtitle stream's language and whether it is text or image based

#### Scenario: Audio file
- **WHEN** a music file is probed
- **THEN** the record has its codec, sample rate, bit depth where applicable, channels, whether it is lossless, and whether tags and embedded art are present

#### Scenario: Missing bitrate is estimated
- **WHEN** a stream reports no bitrate of its own
- **THEN** a bitrate is derived and recorded as an estimate

### Requirement: HDR classification
The engine SHALL classify each video stream as SDR, HLG, HDR10, HDR10+ or Dolby Vision, and SHALL flag the file as uncertain when the signals conflict.

#### Scenario: HDR10
- **WHEN** a stream uses the PQ transfer function with mastering display metadata
- **THEN** it is classified HDR10 with 10-bit depth recorded

#### Scenario: Dolby Vision
- **WHEN** a stream carries a Dolby Vision configuration record
- **THEN** it is classified Dolby Vision with its profile and whether it has an HDR10-compatible base layer

#### Scenario: Conflicting signals
- **WHEN** a stream's colour signalling is inconsistent or incomplete
- **THEN** the file is flagged as HDR-uncertain rather than classified by guess

#### Scenario: Dolby Vision that is not in the expected shape
- **WHEN** a stream carries something that looks like Dolby Vision (a configuration record with missing or unusual fields, or frame metadata without a record) or like dynamic HDR metadata of a kind the engine does not know
- **THEN** the stream is flagged uncertain, and is never classified as SDR

#### Scenario: Colour signalling that proves nothing
- **WHEN** a video stream of more than 8 bits has no colour signalling, or PQ is signalled without mastering display metadata, or a frame that was needed to tell could not be read
- **THEN** the stream is flagged uncertain

### Requirement: Robust to bad input
Probing SHALL never crash or stall the daemon, whatever the file contains or the probe tool outputs.

#### Scenario: Corrupt file
- **WHEN** a file is truncated or not a media file at all
- **THEN** the probe is recorded as failed with an error message and other files continue to be probed

#### Scenario: Probe tool hangs
- **WHEN** the probe tool does not finish within the timeout
- **THEN** it is killed, the probe is recorded as timed out, and no child process is left running

#### Scenario: Unexpected tool output
- **WHEN** the probe tool emits malformed, enormous or unexpectedly shaped output
- **THEN** the probe is recorded as failed and the daemon's memory use stays bounded

#### Scenario: Hostile file name
- **WHEN** a file's name begins with a dash or looks like a URL or option
- **THEN** it is probed as a local file and never interpreted as an option or a network address

### Requirement: No network access from probing
Probing SHALL only read the local file it was given.

#### Scenario: File referencing a remote resource
- **WHEN** a playlist-style file referencing a network URL is probed
- **THEN** no network request is made

#### Scenario: File referencing another local file
- **WHEN** a playlist, script or manifest that names another local file is probed
- **THEN** that file is not opened and none of its contents appears in the record

### Requirement: Probing follows the index
The engine SHALL probe new and changed video and audio files, SHALL NOT probe a file more than once per change, and SHALL discard a result if the file changed while being probed.

#### Scenario: New file is probed once
- **WHEN** the scanner indexes a new video file
- **THEN** it is probed, and scanning again without changes does not probe it again

#### Scenario: Changed file is re-probed
- **WHEN** a probed file's size or modification time changes
- **THEN** it is probed again and its record replaced

#### Scenario: File still being written
- **WHEN** a file's size changes during its probe
- **THEN** the result is discarded and the file is probed on a later scan

#### Scenario: Failed probe is not retried in a loop
- **WHEN** a file's probe fails and the file does not change
- **THEN** it is not probed again automatically

#### Scenario: Probe again on request
- **WHEN** an administrator asks for a file, or for the failed files of a root, to be probed again
- **THEN** they are probed again, and a user who is not an administrator cannot ask

#### Scenario: Older records are probed again
- **WHEN** the version of the probe's mapping is raised
- **THEN** files recorded with an older version are probed again once each, after new and changed files

#### Scenario: A scan comes first
- **WHEN** a volume is being scanned
- **THEN** no probe of that volume starts, and a probe of a volume never makes its scan fail

### Requirement: Volume safety
Probing SHALL use the volume guard and SHALL treat an unavailable volume as a reason to retry later, not as a probe failure.

#### Scenario: Volume goes offline
- **WHEN** a probe job runs for a file whose volume is offline
- **THEN** the file's probe status is unchanged and the job is retried later

#### Scenario: Volume goes offline during the probe
- **WHEN** a volume stops being available while its file is being probed
- **THEN** nothing is recorded for the file, not even a failure, and the job is retried later

### Requirement: HDR10 static metadata values
For a stream with HDR10 mastering-display or content-light data, the probe SHALL store the values themselves, range-checked, and SHALL treat values that are missing or out of range as uncertain HDR.

#### Scenario: Values stored
- **WHEN** an HDR10 file with mastering-display and content-light data is probed
- **THEN** the chromaticity coordinates, white point, luminance range, and the maximum content and frame-average light levels are stored

#### Scenario: Out of range
- **WHEN** the reported values are outside what the format allows
- **THEN** the stream is marked HDR uncertain

#### Scenario: Existing files
- **WHEN** this version first runs on a library probed by an earlier one
- **THEN** HDR files are probed again so that their values are stored

