# conversion-arguments Specification

## Purpose
Exactly what ffmpeg is told to do for a conversion: arguments built from data alone, every kept stream mapped, colour and HDR carried through, for encoders whose abilities were measured on the machine that will run them.
## Requirements
### Requirement: Arguments are built from data alone
The engine SHALL build the ffmpeg argument list for a conversion from the profile revision, the probed facts of the file, the chosen encoder and that machine's measured capabilities, without reading the file and without a shell. No text from a file name or a tag SHALL appear in the argument list. When a fact it needs is missing, it SHALL produce no arguments and give the reason.

#### Scenario: Hostile names and tags
- **WHEN** arguments are built for a file whose name and tags contain option-like or shell-like text
- **THEN** none of that text appears in the argument list

#### Scenario: Missing fact
- **WHEN** an HDR10 file's mastering-display values are unknown
- **THEN** no arguments are produced and the reason is given

### Requirement: ffmpeg is given descriptors, not paths
The input and the output of a conversion SHALL be passed to ffmpeg as open descriptors or, where the output cannot be, as a path inside a private scratch directory that is not derived from library data.

#### Scenario: No library path
- **WHEN** arguments are built for any file
- **THEN** the argument list contains no path under a library root

#### Scenario: Descriptors only
- **WHEN** arguments are built for any file
- **THEN** the input is descriptor 3 and the output descriptor 4, each with only the `fd` protocol allowed

### Requirement: Every kept stream is mapped explicitly
The arguments SHALL map each stream the profile keeps, individually and in a defined order, with its default and forced flags, and SHALL keep attachments, chapters and tags. The plan SHALL list each stream it drops and why.

#### Scenario: All kept streams present
- **WHEN** a file with one video stream, two audio streams, two text subtitle streams and a font attachment is converted with a profile that copies audio and subtitles
- **THEN** the output has those streams in the same order with the same default and forced flags, and its chapters and tags

#### Scenario: Dropped stream is explained
- **WHEN** a profile keeps only one audio language and a file has a track in another
- **THEN** the plan lists that track as dropped with the reason

#### Scenario: Added stereo track
- **WHEN** a profile adds a stereo track to a file that has only surround audio
- **THEN** the output has the original track and a stereo track, in the order the profile states

#### Scenario: A stream the target cannot carry
- **WHEN** a file has a stream the target container cannot hold as it is, such as a cover picture for Matroska or an attachment for MP4, and no rule of the profile drops it
- **THEN** no arguments are produced and the reason is given

### Requirement: Colour and HDR are carried through
The output's colour primaries, transfer, matrix and range SHALL equal the source's where the source states them, and SHALL be left unstated where it does not, with the plan saying so. A range left unstated is limited range (H.264 and H.265 infer a limited range where the bitstream does not state one), so an output that states limited range for a source that states no range, or none for a source stating limited range, carries the source's range through; full range SHALL come out stated. For an HDR10 source the output SHALL be 10-bit with the same mastering-display and content-light values. HLG SHALL stay HLG.

#### Scenario: SDR colour
- **WHEN** an SDR file with stated colour properties is converted
- **THEN** the output's four colour properties equal the source's

#### Scenario: Unstated colour
- **WHEN** a file that states none of its colour properties is converted
- **THEN** the output states no primaries, transfer function or matrix, its range is limited whether or not the encoder states it, and the plan notes each property left unstated and that an unstated range means limited range

#### Scenario: HDR10
- **WHEN** an HDR10 file is converted with an encoder shown to carry HDR10
- **THEN** the output is 10-bit, PQ, BT.2020, with the source's mastering-display and content-light values

#### Scenario: HLG
- **WHEN** an HLG file is converted
- **THEN** the output is HLG

### Requirement: Encoder families
The engine SHALL support software, NVENC and VA-API encoders for HEVC and H.264 behind one interface, each with its own mapping from the profile's quality level.

#### Scenario: Quality mapping differs by family
- **WHEN** arguments are built for one file and profile with a software encoder and with a hardware encoder
- **THEN** each uses its own family's quality option and level

### Requirement: Capabilities are measured
The engine SHALL determine what the installed ffmpeg can do on the machine that will encode by test encodes, per encoder: whether it works, whether it encodes 10-bit, and whether HDR10 values survive it. It SHALL store the result with the ffmpeg version and measure again when the version changes. It SHALL NOT build arguments for an encoder that was not shown able to produce the output.

#### Scenario: Encoder missing
- **WHEN** an encoder in the profile's preference is not usable on the machine
- **THEN** it is recorded as unusable with the reason, and the next encoder in the preference is used

#### Scenario: HDR not carried
- **WHEN** an encoder's test encode does not return the HDR10 values it was given
- **THEN** that encoder is never used for an HDR file

#### Scenario: New ffmpeg version
- **WHEN** the ffmpeg version changes
- **THEN** the capabilities are measured again before any arguments are built

### Requirement: Progress
The engine SHALL read ffmpeg's machine-readable progress and report the fraction done, speed and bytes written, treating every value as optional.

#### Scenario: Progress reported
- **WHEN** a conversion of a file with a known duration is half way through
- **THEN** the reported fraction is about one half

#### Scenario: Unknown duration
- **WHEN** the duration is not known
- **THEN** no fraction is reported and nothing fails

