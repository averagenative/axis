# downloaders Specification

## Purpose
How Axis drives the download clients already running: configuring them in one place, handing them releases exactly once, following each download to completion, and touching only what Axis itself added.
## Requirements
### Requirement: Download clients are configured in one place
The daemon SHALL let an administrator configure download clients, each with its kind, address, secret, category, priority and path mappings, and SHALL test a client on request, reporting whether it can be reached, authenticated and used.

#### Scenario: Test
- **WHEN** an administrator tests a client with a wrong secret
- **THEN** the test fails saying authentication was refused, without showing the secret

#### Scenario: Administrators only
- **WHEN** a non-administrator requests the download clients page
- **THEN** the response is 403

### Requirement: Axis touches only downloads it added
Every download the daemon adds SHALL carry its category and be recorded. The daemon SHALL NOT list, change or remove a download it did not add.

#### Scenario: Someone else's download
- **WHEN** the client holds a download added by another program
- **THEN** it never appears in Axis and is never changed or removed by it

#### Scenario: Removal of an unknown download
- **WHEN** removal is requested for an identifier Axis has no record of
- **THEN** it is refused

### Requirement: A release is sent once
The daemon SHALL record a download before sending it and SHALL NOT send it again when it cannot tell whether the first attempt arrived.

#### Scenario: Crash while sending
- **WHEN** the daemon stops after sending a release but before recording the client's identifier
- **THEN** on restart the download is matched to the client's copy or marked failed, and is not sent a second time

### Requirement: Downloads are tracked to completion
The daemon SHALL follow each download's state and progress, and SHALL record completion with the location of its files, failure with the client's reason, and disappearance from the client.

#### Scenario: Completed
- **WHEN** the client reports a download complete
- **THEN** it is recorded as completed with its location

#### Scenario: Failed
- **WHEN** the client reports a download failed
- **THEN** it is recorded as failed with the client's reason

#### Scenario: Client unreachable
- **WHEN** a client cannot be reached
- **THEN** its downloads keep their last known state, shown with its age, and none is marked failed

### Requirement: A download's location must be inside a known volume
A completed download's location SHALL be accepted only when the client's path maps to a place beneath a known volume. Otherwise the download SHALL wait and the problem SHALL be shown.

#### Scenario: Unmapped path
- **WHEN** a completed download's path matches no mapping
- **THEN** no location is recorded and the client shows the problem

#### Scenario: Outside every volume
- **WHEN** a mapped path resolves outside every known volume
- **THEN** it is refused

### Requirement: Client secrets stay with their client
A client's secret and any session it grants SHALL be sent only to that client's address and SHALL NOT appear in any page, log line or stored job.

#### Scenario: Redirect
- **WHEN** a client's address answers a request with a redirect elsewhere
- **THEN** nothing is sent to the other address

### Requirement: Downloads page
The daemon SHALL provide a page listing active downloads with live progress and recent completed and failed ones, each with its title, client, size and state.

#### Scenario: Live progress
- **WHEN** a download advances
- **THEN** the page shows the new progress without being reloaded

### Requirement: SABnzbd client
The daemon SHALL provide SABnzbd as a download client kind for Usenet releases: testing the connection, adding a release by uploading its file, listing the downloads it added from SABnzbd's queue and history, and removing one.

#### Scenario: Add
- **WHEN** a release file is sent to a SABnzbd client
- **THEN** it is uploaded with Axis's category and the identifier SABnzbd returns is recorded

#### Scenario: Failure inside a successful answer
- **WHEN** SABnzbd answers 200 with a failure status
- **THEN** the call is treated as refused, with SABnzbd's message

#### Scenario: Post-processing
- **WHEN** a job has finished downloading but SABnzbd is still unpacking it
- **THEN** it is not yet completed

#### Scenario: Completed
- **WHEN** SABnzbd reports a job completed with its final folder
- **THEN** the download is completed with that location, subject to the path mapping

### Requirement: The SABnzbd key is never in an address
Every request to SABnzbd SHALL carry the key in the request body, never in the requested address.

#### Scenario: Key placement
- **WHEN** any SABnzbd request is made
- **THEN** the requested address does not contain the key

### Requirement: A client's address may carry a base path
A download client's address SHALL accept an optional path prefix, set by the administrator and never taken from a response, which is applied to every request to that client.

#### Scenario: Served under a path
- **WHEN** a client is configured with a path prefix
- **THEN** every request to it begins with that prefix

#### Scenario: Unsafe prefix
- **WHEN** a prefix contains a parent-directory segment or a query
- **THEN** it is refused

### Requirement: Deluge client
The daemon SHALL provide Deluge as a download client kind for torrents: testing the connection, adding a torrent file or a magnet link, listing the torrents it added, and removing one.

#### Scenario: Add
- **WHEN** a torrent is sent to a Deluge client
- **THEN** it is added, given Axis's label, and recorded by its info-hash

#### Scenario: Label cannot be set
- **WHEN** the torrent is added but its label cannot be set
- **THEN** the torrent is removed again and the add fails

#### Scenario: Seeding
- **WHEN** a torrent has finished and is seeding
- **THEN** the download is completed and stays completed

#### Scenario: Not ready
- **WHEN** the web interface is not connected to a daemon, or the Label plugin is not enabled
- **THEN** the test says which

### Requirement: A lost Deluge session never repeats an add
When Deluge no longer accepts the session, a call that changes nothing SHALL be retried once after logging in again, and an add SHALL NOT be retried.

#### Scenario: Session lost during a list
- **WHEN** a list is refused for lack of a session
- **THEN** the daemon logs in again and the list succeeds

#### Scenario: Session lost during an add
- **WHEN** an add is refused for lack of a session
- **THEN** it is reported as not sent and is not sent again by itself

### Requirement: qBittorrent client
The daemon SHALL provide qBittorrent (Web API v2, qBittorrent 4.1 and later, including 5.x) as a download client kind for torrents: testing the connection, adding a torrent file or a magnet link, listing the torrents it added, and removing one. It SHALL take both generations of the login (`200` with `Ok.` or `204` for success; `200` with `Fails.` or `401` for a wrong password), and send `Referer` and `Origin` headers equal to the client's own address with the login.

#### Scenario: Add
- **WHEN** a torrent is sent to a qBittorrent client
- **THEN** it is added in Axis's category, found again by its info-hash, and recorded by it

#### Scenario: Already there
- **WHEN** the torrent is in qBittorrent already, in another category
- **THEN** the add is refused and the torrent is not touched

#### Scenario: Added but not shown
- **WHEN** qBittorrent answers `Ok.` and the torrent does not show up in its list
- **THEN** the add is reported as uncertain, matched against the list later by its hash, and never sent again

#### Scenario: The category is made
- **WHEN** the client is tested and its category does not exist
- **THEN** the category is created, and the test says so

#### Scenario: Seeding
- **WHEN** a torrent has finished, whether it is seeding, stalled, queued for upload or stopped
- **THEN** the download is completed, at its `content_path`, and stays completed

#### Scenario: Not Axis's
- **WHEN** qBittorrent lists torrents of other categories, or of a sub-category of Axis's
- **THEN** they are not listed, changed or removed

### Requirement: qBittorrent states are mapped, never guessed
The kind SHALL map each documented `state` word of a torrent to one state of Axis, and a word it does not know SHALL be shown as unknown rather than guessed.

| qBittorrent state | Axis state |
|---|---|
| `downloading`, `forcedDL`, `stalledDL`, `metaDL`, `forcedMetaDL` | downloading |
| `queuedDL`, `allocating`, `checkingDL`, `checkingResumeData` | queued |
| `pausedDL`, `stoppedDL` | paused |
| `uploading`, `forcedUP`, `stalledUP`, `queuedUP`, `checkingUP`, `pausedUP`, `stoppedUP` | completed |
| `moving` | downloading (the files are not where they will be) |
| `error`, `missingFiles` | failed, with the reason |
| `unknown`, or any other word | unknown |

#### Scenario: Unknown word
- **WHEN** a torrent's state is a word the kind does not know
- **THEN** the torrent keeps the state last known for it (queued when never seen), a torrent that was completed does not stay completed, and the word is logged once

#### Scenario: Renamed words
- **WHEN** a qBittorrent 5.x reports `stoppedDL` or `stoppedUP`
- **THEN** they are read as `pausedDL` and `pausedUP` are

### Requirement: A refused qBittorrent login is not retried in a loop
When qBittorrent refuses the login the kind SHALL NOT log in again for a while, and the reason SHALL be said plainly: a wrong password in ten minutes' wait, and a `403` (qBittorrent bans an address after several failed logins) in a fifteen minutes' wait, in which no request is made. Saving other credentials, or testing the client, SHALL end the wait.

#### Scenario: Wrong password
- **WHEN** the login answers `Fails.` or `401`
- **THEN** the client is marked as refusing the password, and tracking makes no second login attempt in the wait

#### Scenario: Banned
- **WHEN** the login answers `403`
- **THEN** the message says that qBittorrent has probably banned Axis's address and that Axis is waiting, and no request is made in the wait

### Requirement: A lost qBittorrent session never repeats an add
When qBittorrent answers `403` to an API call for lack of a session, a call that changes nothing SHALL be retried once after logging in again, and an add SHALL NOT be retried.

#### Scenario: Session lost during a list
- **WHEN** a list is answered `403`
- **THEN** the daemon logs in again once and the list succeeds

#### Scenario: Session lost during an add
- **WHEN** an add is refused for lack of a session
- **THEN** it is reported as not sent and is not sent again by itself

### Requirement: A download client may have a user name
A download client SHALL have an optional user name, stored with the client (it is not a secret), for the kinds whose login takes one. A kind that needs none SHALL be unaffected, and the pages SHALL say which kinds use it.

#### Scenario: Saved and shown
- **WHEN** a qBittorrent client is saved with a user name
- **THEN** its page shows the user name, never its password

### Requirement: SABnzbd's host-name refusal is not a refused key
The daemon SHALL tell SABnzbd's refusal of a host name (a 403 whose body says "Hostname verification failed", read within a small size cap) from a refused API key, SHALL report it as its own failure saying that SABnzbd refused the host name Axis used for it, naming that host, and that the name is added to SABnzbd's `host_whitelist` in its Special settings or an address SABnzbd accepts is used, and SHALL NOT count it as a refused key or show the client as "Access refused". Any other 403 SHALL still be a refused key. The body SHALL NOT be put in a message.

#### Scenario: Host name not in the whitelist
- **WHEN** SABnzbd answers 403 "Access denied - Hostname verification failed" to a test of the client
- **THEN** the client is shown as failing, with a message naming the host Axis used and `host_whitelist`, and not as a refused key

#### Scenario: Another 403
- **WHEN** SABnzbd answers 403 with any other body
- **THEN** the key is shown as refused, as before

### Requirement: What SABnzbd deletes when a job is removed
The daemon SHALL treat a SABnzbd history delete with its files as deleting the files of a failed job only: a completed job's entry is removed and its folder stays where it finished. Tests SHALL use a SABnzbd double that behaves so.

#### Scenario: Completed job removed with its files
- **WHEN** a completed SABnzbd job is removed with its files
- **THEN** its history entry is gone and its folder is still there

#### Scenario: Failed job removed with its files
- **WHEN** a failed SABnzbd job is removed with its files
- **THEN** its files are deleted

#### Scenario: A job of one file
- **WHEN** SABnzbd reports a completed job of one file by that file inside the job's folder
- **THEN** the download completes at that file, mapped onto its volume, and removing the job with its files keeps both the file and its folder

### Requirement: SABnzbd says where it keeps Axis's category
The daemon SHALL ask SABnzbd for the folder of Axis's category from its configuration, without asking for its other settings: a relative category folder beneath SABnzbd's folder for finished downloads, an absolute one as it is, a trailing `*` not part of it. A category with no folder of its own (an empty folder, or SABnzbd's folder for every finished download), a folder with a "." or ".." part, a relative folder under a folder for finished downloads that is not a full path, and a category SABnzbd does not have SHALL each be a refusal with its reason. A client that cannot be asked SHALL be an error distinct from a refusal. A torrent client SHALL be asked only as the requirement on where a torrent client keeps Axis's label or category says.

#### Scenario: Relative folder
- **WHEN** Axis's category has the folder `axis` and finished downloads go to `/downloads/complete`
- **THEN** the category's folder is `/downloads/complete/axis`

#### Scenario: No folder of its own
- **WHEN** Axis's category has no folder setting
- **THEN** it is refused, saying the category has no folder of its own

### Requirement: A removal of a download the client no longer has is told apart
A kind whose client's answer says it does not have a download SHALL report that as the download gone, distinct from a refusal: Deluge's error naming `InvalidTorrentError` and saying `torrent_id <the torrent's hash> not in session`; for qBittorrent, whose delete answers alike either way, the torrent not listed by its hash when asked before the delete, and then no delete sent. A kind that cannot tell (SABnzbd's history delete) SHALL report what it can, as before. Any other error, and the same words about another torrent, SHALL stay a refusal. A download removed from the Downloads page that its client no longer has SHALL be recorded as gone, and the page SHALL say that nothing was removed or deleted at the client. Tests SHALL use a Deluge double that answers a removal of a torrent it does not have as Deluge 2.2.0 did.

#### Scenario: Deluge does not have the torrent
- **WHEN** Deluge answers a removal with InvalidTorrentError, "torrent_id <its hash> not in session"
- **THEN** the download is reported gone, not refused

#### Scenario: Another torrent's words
- **WHEN** Deluge's answer names another torrent's hash, or is any other error
- **THEN** it is a refusal

#### Scenario: qBittorrent does not list it
- **WHEN** qBittorrent does not list the torrent asked to be removed
- **THEN** it is reported gone and no delete is sent

#### Scenario: From the Downloads page
- **WHEN** an administrator removes a download its client no longer has
- **THEN** it is recorded as gone and the page says nothing was removed or deleted there

### Requirement: A torrent client says where it keeps Axis's label or category
The daemon SHALL be able to ask a torrent client, read only, for the folder it keeps Axis's label or category in: for Deluge, the Label plugin's "Move completed to" folder of the label, when it is switched on and set; for qBittorrent, the category's own save path. That folder SHALL be refused, with the reason, when it is not a full path, when the client's own folder for every download (Deluge's download and move-completed folders, asked for alone; qBittorrent's default save path) is it or lies inside it, or when another label's or category's folder is it or lies inside it; a category with no save path, a label with the option off, a client without the Label plugin and a label or category the client does not have SHALL each be a refusal. A client that cannot be asked SHALL be an error distinct from a refusal. It SHALL be asked only for a torrent the client no longer has after its import.

#### Scenario: The label moves finished torrents to its own folder
- **WHEN** Deluge's label for Axis has "Move completed to" `/downloads/axis` switched on, and Deluge downloads into `/downloads/incoming`
- **THEN** the label's folder is `/downloads/axis`

#### Scenario: Shared
- **WHEN** another label, or the client's own folder for every download, is that folder or lies inside it
- **THEN** it is refused, saying which

#### Scenario: No folder of its own
- **WHEN** the label's option is off, or the category has no save path
- **THEN** it is refused, saying it has no folder of its own

### Requirement: What is at a completed download's place is recorded
When the daemon records a download as completed at its place on a volume, it SHALL record the inode of what is there (the file or the folder the client reported), so that what the client keeps of it after a removal can be told from something else at that place later. A removal with its data of a download whose client may keep its files (a Usenet job; a torrent its client no longer had) SHALL be recorded for its leftover to be looked at; a removal without its data, and a torrent its client removed with its files, SHALL NOT.

#### Scenario: Completed
- **WHEN** a download completes at a folder on a known volume
- **THEN** its location and the inode of that folder are recorded

#### Scenario: Removed with its data
- **WHEN** a completed Usenet download is removed from its client with its data
- **THEN** it is recorded as removed, with its leftover to be looked at

### Requirement: A switched-off client's downloads are still followed
A download client switched off SHALL be sent no new download, and the daemon SHALL still follow the downloads it has in progress to their completion, failure or disappearance, and import them; a switched-off client with none in progress SHALL NOT be polled. The client's pages SHALL say so.

#### Scenario: Switched off during a download
- **WHEN** a client is switched off while one of its downloads is in progress
- **THEN** no new release is sent to it, the download's progress is still followed and it completes and is imported, and once nothing is in progress the client is not polled

### Requirement: A torrent's client is made to let go of it before an import moves its files
The downloads layer SHALL be able to have a torrent's client let go of a completed torrent before an import moves its files: it SHALL ask the client to remove the torrent without its data and then ask again, up to three times, until the client's own answer is that it does not have the torrent (Deluge's "not in session" for that torrent, qBittorrent's list without it). It SHALL NOT change the download's record, and SHALL refuse a download that is not a completed torrent or has no identifier at its client. A client that cannot be reached SHALL be that error, and one that still has the torrent after being asked three times SHALL be a refusal; neither is taken to have let go.

#### Scenario: Let go of
- **WHEN** a completed torrent's client removes it without its data and then says it no longer has it
- **THEN** the torrent is let go of, its files were not deleted, and the record is unchanged

#### Scenario: Gone already
- **WHEN** the client says at once that it does not have the torrent
- **THEN** it is let go of, said to have been gone already

#### Scenario: Never says so
- **WHEN** the client answers a removal of a torrent it lacks with a refusal
- **THEN** the torrent is not taken to have been let go of

#### Scenario: Not a completed torrent
- **WHEN** the download is a Usenet job, or a torrent that has not completed
- **THEN** it is refused and nothing is asked of the client

### Requirement: What a client has now can be asked, with where each download's files are
The downloads layer SHALL be able to ask a client what it lists in Axis's category now, in every state, without recording anything: each item's identifier, name and, where the kind can tell it, its place (where its files are now, in progress too: qBittorrent's content path, Deluge's save path and name; none while the client moves them), checked as the layer checks every item. The place SHALL be used only for what accounts for an entry of the client's folder for Axis, never for what is imported. The answer SHALL say how much of it Axis could not use: items left out by the check, items whose place could not be read, and a list the kind says is not whole: SABnzbd's at the most one call asks for, qBittorrent's and Deluge's cut at the most torrents read, or holding an entry of Axis's category that cannot be read (not an object, no valid hash, no name), which the kind leaves out.

#### Scenario: A torrent in progress
- **WHEN** qBittorrent lists a torrent still downloading in Axis's category
- **THEN** its place is its content path, though it has not completed

#### Scenario: Moving
- **WHEN** the client is moving a torrent's files
- **THEN** it has no place

#### Scenario: Not all read
- **WHEN** an item's place holds a control character
- **THEN** the answer says one thing could not be read

#### Scenario: An entry without a hash
- **WHEN** qBittorrent or Deluge lists, in Axis's category, an entry whose hash is not one, or that has no name
- **THEN** the entry is left out and the answer says something could not be read

#### Scenario: More torrents than are read
- **WHEN** qBittorrent lists more torrents in Axis's category than the most it reads
- **THEN** the answer says the list is not whole

### Requirement: What the Leftovers page found is kept with the client
Each download client SHALL keep how many entries the page "Leftovers in the download folders" last found in its folder for Axis that no download and no import accounts for, so that the Downloads page and the notices of what needs attention can link to that page while it is not empty.

#### Scenario: The link
- **WHEN** the client's folder for Axis holds two such entries and the count was taken
- **THEN** the Downloads page says so to an administrator and links to "Leftovers in the download folders", and a notice of what needs attention links there with "Open Leftovers in the download folders"

### Requirement: A removal acts only on a torrent still Axis's
A torrent kind SHALL remove a torrent (with its data or without it, after an import, before a move, or on a person's request) only while the client has it in Axis's category or label. One the client has in another category or label (a person moved it there) SHALL be left alone, nothing asked of the client about it, and the removal SHALL fail as not Axis's, distinct from a refusal and from gone. A removal from the Downloads page, or by the films' and series' follow-up, of such a torrent SHALL record it gone, with nothing of its files for Axis to look at. The layer SHALL be able to ask a torrent client, read only, whether it has a torrent by its hash in any category, and in which. An answer about the torrent that holds an entry Axis cannot read (the torrent's own without its name, another without a hash), or more entries than are looked at without the torrent among them, SHALL NOT be taken for the client not having it: the removal SHALL fail as an answer not read, nothing removed, and a release before a move SHALL NOT be taken for done.

#### Scenario: Moved to another category, qBittorrent
- **WHEN** a person moves a torrent Axis added to another category of qBittorrent, and Axis would release it before a move, or remove it with its files after an import
- **THEN** nothing is removed or deleted, no delete is sent, the torrent is in the person's category still, and the removal says it is not Axis's

#### Scenario: Another label, Deluge
- **WHEN** a person gives a torrent Axis added another label in Deluge, and Axis would remove it
- **THEN** no removal is sent, the torrent keeps its label, and a removal from the Downloads page records it gone

#### Scenario: Still Axis's
- **WHEN** the torrent is in Axis's category again
- **THEN** it is released as before

#### Scenario: An answer that cannot be read
- **WHEN** qBittorrent, asked for the torrent by its hash before its release, answers with its entry without a name, or with another entry without a hash
- **THEN** nothing is removed, the release fails as an answer not read, and the torrent is still at its client

### Requirement: SABnzbd's folder for Axis is not another category's
SABnzbd's folder for Axis's category SHALL be refused, with the reason, when another category's folder is that folder or lies inside it, or cannot be worked out. A category with no folder of its own, and one whose folder is SABnzbd's folder for every finished download, SHALL NOT make it refused.

#### Scenario: The same folder as another category
- **WHEN** Axis's category and another category both have the folder `movies` beneath `/downloads/complete`
- **THEN** the folder for Axis is refused, naming the other category

#### Scenario: Beside it
- **WHEN** the other category has the folder `tv`, or none of its own
- **THEN** the folder for Axis is `/downloads/complete/movies`

### Requirement: A path from a client is taken exactly or refused
A path a client reports (a download's place, its folder for Axis, an item's place) SHALL NOT be trimmed or otherwise changed into another path: one that begins or ends with white space SHALL be refused, the download waiting and saying why. Several slashes in a row, and a slash at the end, SHALL still mean the one path they mean. A download's name SHALL be kept exactly as the client's answer gave it (the text read from the answer, before any cleaning, by every kind) for what is compared with names on disk and with a release's name; the name shown MAY be cleaned.

#### Scenario: A folder ending in a space
- **WHEN** a client reports a completed download at a folder whose name ends in a space, beside a folder of the same name without it
- **THEN** no place is recorded, the download waits saying the path begins or ends with white space, and the other folder is never taken

#### Scenario: Slashes
- **WHEN** a client reports the same folder with two slashes in a row and one at its end
- **THEN** it is recorded at that folder

#### Scenario: A name kept exactly
- **WHEN** a client names a download with two spaces in a row and a space at its end
- **THEN** its name is shown with one space and none at its end, and kept with both for comparing

#### Scenario: Through each kind's own answer
- **WHEN** SABnzbd, Deluge or qBittorrent lists a download so named
- **THEN** the name kept for comparing is the one in its answer, with both spaces

### Requirement: Whether anything of a client's is at a path is asked of it whole
What a client has, asked for whether anything of the client's is at a path (the page "Leftovers in the download folders", a download's folder after its import), SHALL be everything it has in every category or label, not Axis's only: SABnzbd's queue and history without a category, each read a page at a time (at most 20 calls of 500 jobs each, beyond which the list is not whole); Deluge's torrents without a label filter; qBittorrent's torrents without a category. Nothing of such a listing SHALL be remembered for tracking.

#### Scenario: Another category's
- **WHEN** a client has a download in another category or label than Axis's, and more of SABnzbd's finished jobs than one page holds
- **THEN** every one of them is listed, with its category or label, and the list is whole

### Requirement: A list that is not whole makes no download gone or failed
While a client's list is not whole (the kind cut it at the most one call reads, or it holds an entry of Axis's category whose identifier Axis cannot read), tracking SHALL leave every recorded download the list does not show as it was, counting no miss, and SHALL NOT fail a download being sent that it does not show; the client's page SHALL say the list is not whole, and once it has not been whole for an hour the Downloads page SHALL say so too. An entry whose identifier (an info-hash, SABnzbd's job identifier) is read and not the rest of it (its name, its category or label) SHALL NOT make the list not whole: the download it is SHALL be kept as it was, and the other downloads judged as ever; a download being sent that the list does not show SHALL be kept while such an entry may be it; the client's page SHALL say which entry and why. A SABnzbd slot whose identifier cannot be read SHALL make SABnzbd's list not whole.

#### Scenario: Beyond the 500th job
- **WHEN** SABnzbd's queue holds 500 jobs of Axis's category before one of Axis's downloads, and answers the 500 asked for
- **THEN** that download stays queued however many times the client is looked at, a download being sent is not failed, and the client's page says its list is not whole; once the list is whole without them, they are missing and gone as before

#### Scenario: A slot that cannot be read
- **WHEN** SABnzbd lists a slot of Axis's category whose identifier is not one SABnzbd gives
- **THEN** the list is not whole, and a download it does not show is not taken for gone

#### Scenario: One entry read by its hash only
- **WHEN** qBittorrent lists one torrent of Axis's category with its hash and no name, and no longer lists another of Axis's downloads
- **THEN** the first is kept as it was, the second is missed and gone as ever, and the client's page names the first's hash and why

#### Scenario: Not whole for long
- **WHEN** a client's list has not been whole for two hours
- **THEN** the Downloads page says so, naming the client, and no longer once it is whole again

### Requirement: A client's page says when its folder for Axis is shared
Testing a client SHALL say, when another client (switched off in Axis too) keeps Axis's downloads in the same folder as Axis sees it on a volume, or in one that holds it or lies inside it, that the folder is not its own, and, when another client whose path mappings reach it keeps Axis's downloads in no folder Axis can work out, that it may not be its own; and that Axis deletes nothing there until each client has a folder of its own. When no other client is configured, nothing more SHALL be asked. What the test or the page "Leftovers in the download folders" last found SHALL be said with the client's problems on its page from the next look at the client on, until it is no longer so, or the client itself has no folder of its own for Axis (that page then says so).

#### Scenario: Shared
- **WHEN** a client is tested whose folder for Axis is also another client's
- **THEN** the test's note names the other client and says Axis deletes nothing there, and after the next look at either client its problems say so

#### Scenario: No longer shared
- **WHEN** the other client is given a folder of its own and "Leftovers in the download folders" is opened
- **THEN** after the next look at the client its problems no longer say so

### Requirement: A built-in client is driven like any other
The downloads layer SHALL allow a kind of client that is a part of Axis itself (the YouTube kind, which runs yt-dlp): it has no address, no secret and no path mapping, takes a protocol of its own (`youtube`) whose release is an address, and has one client that Axis makes on first use. A download of it SHALL be recorded before it is handed over, followed by tracking and removed through the kind, as any download is. Tracking SHALL follow the built-in client only while it has a download in progress. A path it reports for a finished download SHALL be Axis's own (a volume and a path on it) and SHALL be checked through the volume guard like a mapped client path. Its finished download SHALL be recorded as imported at once, since its file is already in the library, and the manual import SHALL NOT offer it. Its client SHALL NOT be among the clients an administrator configures: it is not listed, counted, changed or deleted with them, and no other client SHALL be saved under its name. No indexer result SHALL be taken for a release of this protocol.

#### Scenario: Sent and followed
- **WHEN** a video's address is added with protocol `youtube`
- **THEN** a download of the built-in client is recorded, its kind queues the fetch, tracking carries its state, progress and location into the record, and the finished download is recorded as imported

#### Scenario: Not a configured client
- **WHEN** the built-in client exists and no other client is set up
- **THEN** the Download clients page lists no client, and the count of configured clients is 0

#### Scenario: An address for another protocol
- **WHEN** a Usenet or torrent release is added with an address, or a `youtube` release with a file or a magnet link
- **THEN** it is refused, and nothing is recorded

