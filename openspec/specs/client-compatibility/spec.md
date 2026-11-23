# client-compatibility Specification

## Purpose
What the players in use can actually play: a read-only connection to the media server, evidence gathered per stream from sessions as they play, and a verdict for each conversion target in which unknown is never treated as safe.
## Requirements
### Requirement: Read-only media server connection
The daemon SHALL connect to a configured media server using a token held as a secret setting, SHALL only read from it, and SHALL send the token only to the configured host.

#### Scenario: Token stays secret
- **WHEN** the connection is configured and used
- **THEN** the token appears in no page and no log line

#### Scenario: Redirect to another host refused
- **WHEN** the media server answers with a redirect to a different host
- **THEN** the request fails and the token is not sent to that host

#### Scenario: Bad responses are survivable
- **WHEN** the media server returns an oversized, malformed or very slow response
- **THEN** the request fails with a recorded error and the daemon continues

#### Scenario: Connection test
- **WHEN** an administrator tests the connection
- **THEN** the page reports success with the server's name and version, or the reason for failure

### Requirement: Library mapping
The daemon SHALL map each media server library to Axis roots through an administrator-editable path map, SHALL list libraries it cannot map, and SHALL NOT guess a mapping silently.

#### Scenario: Mapped through the path map
- **WHEN** a library's folder corresponds to a root through the path map
- **THEN** the library is shown as mapped to that root

#### Scenario: Unmapped library
- **WHEN** a library's folder matches no root
- **THEN** it is listed as unmapped, with a suggestion if one is plausible, and is excluded from verdicts

### Requirement: Playback evidence
The daemon SHALL record play sessions observed while they play, each once, with the device, library, file and, for each stream, whether it was played directly, passed through or converted, and SHALL state how much evidence it holds. The media server states no reason for a conversion; the daemon SHALL infer one conservatively and SHALL record a conversion it cannot explain as cause unclear. Past plays imported from the server's history carry no playback decision and SHALL be used only to know which devices play which library.

#### Scenario: Evidence is per stream
- **WHEN** a session passes the video through and converts the audio
- **THEN** it is recorded as positive evidence for the video codec and negative evidence for the audio codec

#### Scenario: Unexplained conversion
- **WHEN** a stream is converted in a way that fits neither a codec the device did not take nor a bandwidth limit
- **THEN** it is recorded as cause unclear and counts as unknown

#### Scenario: Unknown decision value
- **WHEN** the server reports a decision value the daemon does not know
- **THEN** that stream's evidence is unknown

#### Scenario: History has no decisions
- **WHEN** past plays are imported from the server's history
- **THEN** they add no direct-play or conversion evidence

#### Scenario: Session recorded once
- **WHEN** a session is observed in several consecutive polls
- **THEN** one session record exists for it

#### Scenario: Coverage shown
- **WHEN** the Compatibility page is opened
- **THEN** it shows the period and number of plays the evidence covers

#### Scenario: Retention
- **WHEN** a session is older than the retention period
- **THEN** it is removed

### Requirement: Capabilities come from evidence
A device SHALL be considered able to play a combination of formats directly only if it has been observed doing so, or doing so with a combination defined as harder. A combination not observed SHALL be unknown.

#### Scenario: Observed direct play
- **WHEN** a device has directly played a 2160p HEVC 10-bit file
- **THEN** it is considered able to directly play 1080p HEVC 10-bit in the same container

#### Scenario: Not observed
- **WHEN** a device has never been observed with a codec
- **THEN** its ability to play that codec is unknown

#### Scenario: Bitrate-limited conversion
- **WHEN** a video stream was converted to the same codec at a lower resolution or bitrate, or a remote session's bandwidth was below the source's bitrate
- **THEN** the conversion is treated as a bandwidth or quality limit and does not count as evidence that the device cannot play the file's formats

#### Scenario: Passed through is not direct play
- **WHEN** a stream was passed through into a different container
- **THEN** it is evidence for the codec but not for the original container

### Requirement: Verdict per target and library
For each target and each mapped library, the daemon SHALL give a verdict of safe, risky or unknown based on the devices that have played that library within the look-back window, and SHALL show the devices and evidence behind it.

#### Scenario: Safe
- **WHEN** every device that played the library in the window is able to directly play the target's combination
- **THEN** the verdict is safe

#### Scenario: Risky
- **WHEN** a device that played the library in the window has been observed converting the target's combination
- **THEN** the verdict is risky and names that device and the reason

#### Scenario: Unknown is never safe
- **WHEN** a device that played the library in the window has not been observed with the target's combination
- **THEN** the verdict is unknown and names that device

#### Scenario: No evidence
- **WHEN** no plays of a library have been recorded
- **THEN** the verdict for every target is unknown

#### Scenario: Pinned device
- **WHEN** a device marked always-consider last played a library before the look-back window
- **THEN** it still counts toward that library's verdicts

### Requirement: What fails today
The daemon SHALL report which files and format combinations are currently converted by the media server when played, by device and reason.

#### Scenario: Current failures
- **WHEN** a device has repeatedly converted files with a given audio codec
- **THEN** the report lists that device, the codec, the reason and the number of plays

### Requirement: Recommendation
For each mapped library, the daemon SHALL recommend the target with the largest estimated saving whose verdict is safe, or state that none is safe and which devices prevent the best one. Verdicts and the recommendation SHALL be shown beside each target on the Savings pages.

#### Scenario: Largest safe saving
- **WHEN** two targets are safe for a library
- **THEN** the one with the larger estimated saving is recommended

#### Scenario: Nothing safe
- **WHEN** no target is safe for a library
- **THEN** no target is recommended, and the page names the devices that block the target with the largest saving

### Requirement: Play history is protected
Compatibility pages SHALL be available to administrators only, and stored sessions SHALL hold no media server user name unless an administrator enables it.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests a Compatibility page
- **THEN** the response is 403

#### Scenario: Names off by default
- **WHEN** sessions are recorded with default settings
- **THEN** no media server user name is stored

### Requirement: Jellyfin as a media server
The daemon SHALL support Jellyfin as a kind of media server for client compatibility, with the same read-only rules as any other: only GET requests, and the secret sent only to the configured origin.

#### Scenario: Connection test
- **WHEN** an administrator tests a Jellyfin connection
- **THEN** the page reports the server's name and version, or the reason for failure

#### Scenario: Secret stays secret
- **WHEN** a Jellyfin connection is configured and used, including when requests fail
- **THEN** the key appears in no page, log line or stored job, and is sent to no other origin

#### Scenario: Read-only
- **WHEN** the daemon collects from Jellyfin
- **THEN** it sends no request other than GET

### Requirement: More than one server at once
The daemon SHALL allow connections to different kinds of media server at the same time, SHALL show which server each device, library and piece of evidence came from, and SHALL keep working with the others when one is unreachable.

#### Scenario: Verdict across servers
- **WHEN** a library root is served by two servers and devices of both have played it
- **THEN** its verdict considers the devices of both

#### Scenario: One server down
- **WHEN** one configured server does not answer
- **THEN** collection from the other continues and its evidence is unaffected

### Requirement: Server-stated reasons
Where a media server states why a stream is being converted, the daemon SHALL use that reason as the evidence, mapped through a fixed table, SHALL store what the server stated, and SHALL treat a reason it does not know as cause unclear. Inference SHALL be used only when no reason is stated.

#### Scenario: Stated codec reason
- **WHEN** the server states that the device does not support a stream's codec
- **THEN** the evidence is that the device did not take that codec

#### Scenario: Stated bandwidth reason
- **WHEN** the server states that a conversion is because of a bitrate or bandwidth limit
- **THEN** it does not count as evidence against the file's formats

#### Scenario: Unknown reason
- **WHEN** the server states a reason the table does not contain
- **THEN** the conversion is recorded as cause unclear and counts as unknown

### Requirement: Evidence names the property that failed
When a server states that a conversion was caused by a specific property (container, profile, bit depth, level or subtitle format) and not the codec, the daemon SHALL record the evidence against that property.

#### Scenario: Container-only failure
- **WHEN** a device is reported not to take a container while the codec is passed through
- **THEN** the device is not counted as unable to play that codec in another container

### Requirement: Devices without history
For a server that keeps no per-device play history, a device SHALL be considered for a library only once it has been observed playing it, and the page SHALL say so.

#### Scenario: Not yet seen
- **WHEN** a device of such a server has not been observed playing a library
- **THEN** it does not count toward that library's verdict, and the page states that history is unavailable for that server

### Requirement: Media servers are told which folder changed
After a file is imported, replaced or removed, the daemon SHALL tell each configured media server the folder that changed, as that server sees it, and SHALL NOT ask for a scan of a whole library.

#### Scenario: Import
- **WHEN** a film is imported into a root that maps to a library on a server
- **THEN** that server is asked to scan the film's folder only

#### Scenario: No matching library
- **WHEN** the root maps to no library on a server
- **THEN** nothing is sent to that server and the page says the root is not mapped

### Requirement: The only write to a media server is the rescan request
A media-server connection SHALL be able to send exactly one kind of request that is not a read: the request that a path be rescanned.

#### Scenario: Any other write
- **WHEN** code attempts any other non-read request on a media-server connection
- **THEN** it is refused before anything is sent

### Requirement: A media server being down never affects an import
Notifications SHALL be sent after the import is complete, retried a limited number of times, and dropped with a visible note when they cannot be delivered.

#### Scenario: Server down
- **WHEN** a media server cannot be reached
- **THEN** the import succeeds and the undelivered notification is shown

### Requirement: Notifications are batched
Changes to one folder within a short period SHALL be sent as one notification.

#### Scenario: Film with subtitles
- **WHEN** a film and its subtitles are imported together
- **THEN** each server receives one notification for the folder

### Requirement: A rescan request no mapping reached is sent once one does
When a path mapping is added or removed, the daemon SHALL send again each rescan request that was not sent because no mapping reached its folder, whose folder last changed within the last seven days, that is the latest request for its folder, and whose folder the path map now reaches. Older requests SHALL stay as they are. At most one request per server and folder SHALL wait at a time.

#### Scenario: Mapping added after the import
- **WHEN** a film was imported into a folder no mapping reached, and a mapping that reaches it is added a day later
- **THEN** the server is asked to scan that folder

#### Scenario: Older than a week
- **WHEN** the request's folder last changed more than seven days before the mapping was added
- **THEN** it is not sent

#### Scenario: Told since
- **WHEN** a later request for the same folder was sent
- **THEN** the earlier one is not sent again

### Requirement: A request that was not sent can be asked again
The Rescan requests page SHALL offer administrators an "Ask again" action, a POST that needs the session's token, on each request that was not sent: one no mapping reached, one whose tries ran out, one skipped. It SHALL wait again as if its folder had just changed. A request that was sent or is waiting SHALL be refused, and when a request for the same folder is waiting already nothing SHALL change.

#### Scenario: Ask again
- **WHEN** an administrator asks a dropped request again
- **THEN** it waits, its tries counted from nothing, and is sent

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator, or a request without the token, asks again
- **THEN** it is refused and nothing changes

### Requirement: A file Axis made is what it made
When the daemon works out whether a library may hold HDR for the verdicts, a video file whose probe is uncertain of its HDR class SHALL count as SDR, and not as possibly HDR, when its content is still a replacement Axis made, recorded as made from a source the probe classified as SDR with certainty, with a plan that kept SDR, that passed every check: its inode, size and modification time are those of the output Axis settled. Every other file whose probe is uncertain SHALL keep counting as possibly HDR.

#### Scenario: A file Axis made from an SDR source is SDR
- **WHEN** a library of SDR files whose players are seen to direct-play HEVC Main 10 SDR has a file replaced by Space saver, whose output probes as HDR uncertain, with the record of what Axis made
- **THEN** the verdict for Space saver stays safe, and stays safe after a second such replacement

#### Scenario: A file Axis did not make keeps the caution
- **WHEN** a file probes as HDR uncertain without such a record, or with a record of a source the probe was not certain was SDR, or has been replaced since by something else
- **THEN** the library counts as possibly HDR, and the verdict is unknown without evidence of HDR playback

