# import Specification

## Purpose
How a finished download becomes a library file: deciding what may be imported, placing it without ever exposing a partial file or leaving a title without one, naming it for the media servers, and recording what was done.
## Requirements
### Requirement: A completed download is imported for the title it was grabbed for
The daemon SHALL import a completed download into the library for the title it was grabbed for, finding that title's file among the download's files and ignoring samples and unrelated files.

#### Scenario: Film imported
- **WHEN** a film's download completes and holds one film file
- **THEN** the file is placed in the film's folder in its destination root and the film is in the library

#### Scenario: A film titled Sample
- **WHEN** the film's own title contains the word sample
- **THEN** its file is not taken for a sample

### Requirement: Every import decision is explained, and a blocked import can be resolved
Each file considered SHALL be accepted or rejected with every reason stated. An import that cannot proceed SHALL be marked blocked with its reasons and the actions that resolve it.

#### Scenario: Several films
- **WHEN** a download holds several files that could each be the film
- **THEN** the import is blocked, saying so, and an administrator can choose the file

#### Scenario: Wrong film
- **WHEN** the file's name reads as a different film
- **THEN** the import is blocked with that reason

### Requirement: A partial file is never visible
While a file is being placed it SHALL exist only under a temporary name that media servers ignore, and a failure at any step SHALL leave the library as it was.

#### Scenario: Copy fails part way
- **WHEN** a copy fails before it is verified
- **THEN** no file under the final name exists and the temporary file is removed

### Requirement: An upgrade never leaves a title without a file
When an import replaces a file, the new file SHALL be in place and verified before the old file is retired, and the old file SHALL be moved to a recycle folder and kept for the retention time.

#### Scenario: Upgrade
- **WHEN** a better file is imported for a film already in the library
- **THEN** the new file is in place before the old one is moved to the recycle folder

#### Scenario: Upgrade fails
- **WHEN** placing the new file fails
- **THEN** the old file is still in the library

### Requirement: Imported files are named so both media servers recognise them
An imported film SHALL be placed in a folder named with its title, year and provider identifier, and its file SHALL carry that name with its quality and release group, with unsafe characters replaced by one documented rule and every name within the filesystem's length limit.

#### Scenario: Name
- **WHEN** a film is imported with renaming on
- **THEN** its folder is `Title (Year) {tmdb-N}` and its file begins with the same text

#### Scenario: Very long title
- **WHEN** the full name would exceed the limit
- **THEN** the name is shortened without cutting a character in two and the import succeeds

### Requirement: Nothing is written while storage is read-only
The daemon SHALL NOT import while storage is set read-only or the destination volume is read-only or offline, and SHALL say which.

#### Scenario: Read-only
- **WHEN** storage is read-only and a film's download completes
- **THEN** nothing is written and the film waits, with the reason shown

### Requirement: Imports are confined to known volumes
Every path read or written by an import SHALL be beneath a known volume, reached through the volume guard, and symbolic links in a download SHALL NOT be followed.

#### Scenario: Symlink in a download
- **WHEN** a download contains a symbolic link pointing elsewhere
- **THEN** it is not followed and not imported

### Requirement: Episodes are imported file by file
For a series download the daemon SHALL match each video file to the episode or episodes it holds and import each into the series' season folder under a name both media servers read. A file that matches no wanted episode SHALL be left where it is and listed.

#### Scenario: Season pack
- **WHEN** a season pack completes
- **THEN** each episode file is imported to `Season NN` under the series' folder with its `SxxEyy` in the name

#### Scenario: Double episode file
- **WHEN** a file holds two episodes
- **THEN** it is imported once, named for both, and both episodes are in the library

#### Scenario: Unmatched file
- **WHEN** a file in the download matches no wanted episode
- **THEN** it is not imported and the import lists it

### Requirement: What happens to a torrent after import is a setting, and the default removes it
Unless the administrator has chosen to leave torrents seeding, the daemon SHALL take a torrent away from its client when it is imported: with files moved (the default), its client SHALL let go of it, without its files, before the first of them is moved; with "Files taken from a download" on hardlink, its client SHALL remove it with its downloaded files after every wanted file of it is imported and recorded. Neither SHALL affect the imported library files.

#### Scenario: Default
- **WHEN** a torrent's files are imported with the default settings
- **THEN** the torrent is removed from its client without its files before they are moved, its files are moved into the library, and its client deleted nothing

#### Scenario: Leave seeding
- **WHEN** the setting is to leave torrents seeding
- **THEN** the torrent and its files are left as they are

#### Scenario: Partly imported, hardlinked
- **WHEN** with "Files taken from a download" on hardlink some wanted file of the torrent is not yet imported
- **THEN** the torrent is not removed

#### Scenario: Partly imported, moved
- **WHEN** with files moved one file of the torrent cannot be placed
- **THEN** the torrent's client has let go of it already, the files not placed stay in its folder, the import ends partly imported, and a retry moves them once the client says again that it does not have the torrent

### Requirement: The recycle period defaults to two days
Replaced files SHALL be kept in the recycle folder for a settable time that defaults to two days.

#### Scenario: Default
- **WHEN** the recycle time has not been set
- **THEN** a replaced file is removed two days after it was replaced

### Requirement: A Usenet download's leftover folder is removed after its import
After a Usenet download's import is recorded and its client has removed the job, the daemon SHALL delete the job's folder that the client left, with everything in it, and SHALL do so only when all of these hold: the path the client reported for the download when it completed is a full path with no "." or ".." part; it lies strictly inside the folder the client keeps Axis's category in, as the client says now, and is not that folder; the path mapping that reaches it also holds that folder; through the mapping it is the place the download was recorded at, on a known volume, and the import took its files from it or, for a file the client reported inside a job's folder, from that job's folder; it is not the volume's top or a folder of Axis's own; it is not inside a library root other than a downloads root, and is neither a root nor holds one; no other download still at its client and no import still open is in it or around it; it is reached without a symbolic link or another filesystem, is a folder, and is still the folder imported from: it still holds a file the import took from it, by name and inode, or, when the import moved its files, it passes the check of a folder a move left (see "After a move, the folder is told by what the move left"). When the client reported the download's file rather than its folder (as SABnzbd reports a job of one file), the folder deleted SHALL be the job's folder that holds the file, the first part beneath the category's folder on the way to it, and the rules about the place SHALL apply to that folder; a file the client put straight into the category's folder SHALL be deleted alone, by its inode, and the category's folder never. The library's files SHALL keep their own names. A torrent's files SHALL be removed this way only when its client no longer has the torrent, as the requirement on a torrent gone from its client says, or let go of it before the import moved its files. A folder that a rule refuses SHALL be left, and the import SHALL say which rule.

#### Scenario: Removed
- **WHEN** a Usenet download completed in its own folder inside the category's folder is imported and the client removes the job but keeps its folder
- **THEN** the folder is deleted with every file in it, the library's file is intact, and the import says the folder was deleted

#### Scenario: Removed after a move
- **WHEN** the import moved the download's files and the client removes the job
- **THEN** the folder is deleted with what the import did not take (an .nfo, a sample, par2 files), and the import says so

#### Scenario: The client reported the file
- **WHEN** the client reported the download as its one file inside the job's folder, or deeper in it
- **THEN** the job's folder is deleted with everything in it, and the import names that folder

#### Scenario: A file straight in the category's folder
- **WHEN** the client reported the download as a file directly in the category's folder
- **THEN** only that file is deleted, the category's folder and what else is in it stay, and the import says so

#### Scenario: The reported file replaced
- **WHEN** another file is at the reported file's place than the one imported
- **THEN** nothing is deleted and the import says why

#### Scenario: The category's folder itself
- **WHEN** the client reported the job's files in the category's folder itself, with no job folder
- **THEN** nothing is deleted and the import says the folder is the category's

#### Scenario: Outside the category
- **WHEN** the reported folder is not inside the client's folder for Axis's category, or the client keeps the category in no folder of its own
- **THEN** nothing is deleted and the import says why

#### Scenario: Inside the library
- **WHEN** the folder is inside a library root that holds media, or holds a root
- **THEN** nothing is deleted and the import names the root

#### Scenario: A symbolic link
- **WHEN** a symbolic link is at the folder's place or on the way to it
- **THEN** it is not followed, nothing is deleted, and the import says so

#### Scenario: Not the folder imported from
- **WHEN** the file the import took from the folder is no longer in it, and the import did not move it
- **THEN** nothing is deleted

#### Scenario: Imported from the job's folder
- **WHEN** the import took its files from the job's folder around the file the client reported
- **THEN** the job's folder is deleted, the import's files being looked for in it as they were recorded

#### Scenario: Torrent
- **WHEN** a torrent's files are imported by hardlink and its client removes the torrent
- **THEN** Axis removes no folder of it, and what happens to the torrent is still the setting's

### Requirement: The leftover folder waits while nothing may be written
The daemon SHALL NOT delete a leftover folder while storage is read-only, or while its volume is offline or mounted read-only, or before the client's removal of the job is done; the folder SHALL wait, the import SHALL say why, and the hourly cleanup SHALL look at it again.

#### Scenario: Read-only
- **WHEN** storage is read-only once the client has removed the job
- **THEN** the folder is still there, the import says it waits because storage is read-only and is tried again every hour, and it is deleted by the first cleanup after storage is writable

#### Scenario: Offline
- **WHEN** the folder's volume is offline
- **THEN** nothing is written and the folder waits

#### Scenario: Client removal not done
- **WHEN** the client could not remove the job yet
- **THEN** the folder is not touched until it has

### Requirement: The import says what happened at the client
The import's page SHALL say, for a Usenet download, that the job was removed from its client (not that its data was) and, separately, whether its folder was deleted, waits and why, was already gone, or was left and by which rule; for a torrent, that it was let go of by its client without its files before they were moved, removed with its downloaded files, left seeding, or already gone from its client with none of its files deleted there, and separately what became of what it left (deleted, waiting and why, already gone, or left, by which rule and where it is), and, for one left seeding whose files something else deleted, that they are gone and whether the library's files are in place. It SHALL say how the files were placed (moved, moved by a copy, hardlinked, copied) and, for an import whose download's files disappeared before it, that they did, where they were and when Axis last saw them. The title's page SHALL say what happens to a download's files at its import and after.

#### Scenario: Usenet import page
- **WHEN** an imported Usenet download's page is opened
- **THEN** it shows how its files were placed, the client's removal and the folder's outcome, each as it happened

#### Scenario: A torrent gone from its client
- **WHEN** the page of an import whose torrent was already gone from its client is opened
- **THEN** it says the torrent was already gone and its client deleted none of its files, never that it was removed with them, and says on its own line what became of what it left

#### Scenario: A torrent let go of
- **WHEN** the page of an import whose torrent's files were moved is opened
- **THEN** it says the torrent was removed from its client without its files before they were moved, never that its client deleted them

### Requirement: Imports recorded before are looked at again
Imports of Usenet downloads recorded before the leftover folder was removed SHALL be looked at once by the same rules, without asking the client again.

#### Scenario: Folder left by the earlier release
- **WHEN** an imported Usenet download's client removal is done and its folder is still there from before
- **THEN** the cleanup deletes the folder under the same rules, or records that it is gone

### Requirement: A download already gone from its client is not a failed removal
When the daemon asks a client to remove a download after its import and the client answers that it does not have it, the daemon SHALL record the removal as gone, not as failed, SHALL NOT ask again, and the import SHALL say that the download was already gone from its client and that the client deleted none of its files. A removal recorded as failed before, whose client now answers so, SHALL be recorded as gone by the hourly cleanup.

#### Scenario: Removed by something else
- **WHEN** a torrent's import is recorded and its client says it no longer has the torrent
- **THEN** the removal is gone, it is never asked again, and the import's page says it was already gone and the client deleted none of its files

#### Scenario: Recorded as failed before
- **WHEN** a removal was recorded as failed because the client no longer had the torrent, and the hourly cleanup runs
- **THEN** the client is asked once more, and the removal is recorded as gone

### Requirement: What a torrent gone from its client left is removed by the same rules
When a torrent's client no longer has it after its import and the import settings remove torrents after import, the daemon SHALL delete what the torrent left under the rules of a Usenet download's leftover folder, with the folder the client keeps Axis's label or category in as the category's folder. When the client keeps Axis's torrents in no folder of their own, a torrent's folder SHALL NOT be deleted, and the import SHALL say why and where it is; a torrent of one file SHALL have that file deleted alone, by its name and inode, under every other rule, and only while the library still holds the file the import placed of it. With the settings leaving torrents seeding, nothing of a torrent SHALL be deleted. Nothing SHALL be deleted while storage is read-only or the volume is offline or mounted read-only; it SHALL wait, saying why.

#### Scenario: Inside the label's folder
- **WHEN** a torrent gone from its client lies in its own folder inside the client's folder for Axis's label
- **THEN** that folder is deleted with what is in it, the label's folder stays, and the library's file keeps its own name

#### Scenario: No folder of Axis's own
- **WHEN** a torrent gone from its client left a folder and the client keeps Axis's torrents in no folder of their own
- **THEN** nothing is deleted, and the import says why and where the folder is

#### Scenario: One file, no folder of Axis's own
- **WHEN** a torrent of one file is gone from its client and the client keeps Axis's torrents in no folder of their own
- **THEN** that file is deleted alone and the files beside it stay

#### Scenario: The library's copy missing
- **WHEN** the file the import placed in the library is no longer there
- **THEN** the torrent's file is kept, and the import says so

#### Scenario: Left seeding
- **WHEN** the import settings leave torrents seeding
- **THEN** nothing of a torrent is deleted

#### Scenario: Read-only
- **WHEN** storage is read-only
- **THEN** nothing is deleted, the import says it waits and why, and the hourly cleanup looks again

#### Scenario: Outside the label's folder
- **WHEN** the torrent is not inside the client's folder for Axis's label
- **THEN** nothing is deleted, and the import says why

### Requirement: What a download removed with its data outside an import left is removed by the same rules
When the daemon removes a download from its client with its data for any reason but its import (a person giving up on it, a failure put on the blocklist) and the client may have kept its files (a Usenet job, whose completed folder SABnzbd keeps; a torrent its client no longer had), the daemon SHALL delete what it left under the rules of an import's leftover folder, with what the client reported at the place when the download completed (the file or folder recorded then, by its inode) standing in for an imported file. It SHALL delete nothing when the client never reported where the download's files were, when nothing was recorded of what was there, when something else is at that place now, or when the client keeps Axis's downloads in no folder of their own, and the download SHALL then say why and where the files may be. A removal that keeps the files, and a torrent its client removed with its files, SHALL leave nothing for Axis to delete. The page after a person gives up on a download SHALL say what happens to its files, and the Downloads page SHALL say what became of them.

#### Scenario: Given up after it completed
- **WHEN** a person gives up on a Usenet download that completed at its client in its own folder inside the category's folder
- **THEN** the client removes the job, and the hourly cleanup deletes the folder SABnzbd kept, and the download's line says so

#### Scenario: Never said where
- **WHEN** the client never reported where the download's files were
- **THEN** nothing is deleted, and the download's line says that, if it finished, they are in the client's folder for Axis's category under its name

#### Scenario: Something else there now
- **WHEN** another folder of that name is at the place now
- **THEN** nothing is deleted, and the download's line says why

#### Scenario: No folder of Axis's own
- **WHEN** the client keeps Axis's downloads in no folder of their own
- **THEN** nothing is deleted, and the download's line says where the files are

#### Scenario: Kept
- **WHEN** a download is removed without its data, or a torrent is removed with its files by its client
- **THEN** Axis deletes nothing of it

### Requirement: A file reported inside a job's folder is imported with that folder
When a client reports a Usenet download by a file inside the job's folder (the first part beneath the folder the client keeps Axis's category in, as for the leftover folder), the import SHALL take its files from that job's folder, so that it imports what it would have had the client reported the folder (subtitles and extras beside the file). When the job's folder cannot be established (the file is straight in the category's folder, the category has no folder of its own, or the client cannot be asked), the import SHALL take the reported file alone.

#### Scenario: A subtitle beside the file
- **WHEN** SABnzbd reports a job of one file by that file, and a subtitle for it is beside it in the job's folder
- **THEN** the film and its subtitle are imported, and the job's folder is deleted afterwards

#### Scenario: Straight in the category's folder
- **WHEN** the reported file is straight in the category's folder
- **THEN** the file alone is imported

#### Scenario: The category's folder not known
- **WHEN** the client cannot say where it keeps Axis's category when the import runs
- **THEN** the reported file alone is imported

### Requirement: A download's files are moved into the library by default, or hardlinked
How a download's files get into the library SHALL be a setting, "Files taken from a download" (`import.take_files`): `move`, the default, or `hardlink`. With `move`, the files an import takes of a Usenet download, and of a torrent whose import settings do not leave it seeding, SHALL be moved: on the destination's volume each by one rename that never replaces a name, so that the file has its download name or its new one at every moment and never neither; on another volume by a copy that is fsynced, read back and verified by size and checksum before it gets its final name, the download's file being deleted only after the import is recorded, only once the library's folder that names the copy was flushed to the disk, and only while its copy is in the library and it is the very file copied, untouched since (its inode, size, modification time and change time as the copy found them on the descriptor it read, before its first byte and again after its last, never as looked at once the copy was verified; checked on the file opened). A torrent left seeding, a folder imported by hand from a downloads root, and every import with `hardlink` SHALL be hardlinked on the same volume, leaving the source untouched, and copied with verification across volumes, the download's file left where it is. The way each file was placed SHALL be recorded and shown, and when the setting moves but an import does not, why. This replaces "Files are hardlinked where possible and copied with verification where not": with hardlinks a file left in a download folder could be an orphan or an imported file's second name, which look the same and take no room, so a failed or missed download could not be told from an imported one; after a move, whatever is still in a download folder was not imported.

#### Scenario: Usenet on the same volume
- **WHEN** a Usenet download on the destination's volume is imported with the default setting
- **THEN** the library's file is the download's own file (the same inode, one name), and the download's folder no longer holds it

#### Scenario: Usenet on another volume
- **WHEN** the download and the destination root are on different volumes
- **THEN** the file is copied and verified, the record says it was moved by a copy, and the download's file is deleted once the import is recorded

#### Scenario: The download's file changed before its deletion
- **WHEN** another file has the copied file's name, the file was changed since it was copied (its times differ, its inode and size the same), or the library's copy is not in place, when the download's file is to be deleted
- **THEN** the download's file is kept and the import's file says why

#### Scenario: The download's file changed while its copy was read back
- **WHEN** the download's file is written over in place after it was read for its copy, while the copy is flushed and read back
- **THEN** the import completes with the copy of what was read, and the download's file, the newer one, is kept, the import's file saying it is not the file copied

#### Scenario: The download's file renamed over while it is copied
- **WHEN** another file is renamed over the download's file's name while it is read for its copy
- **THEN** the copy is discarded, the import says the download's file changed while it was copied and waits to try again, nothing is placed, and the newer file stays

#### Scenario: The library's folder not flushed
- **WHEN** the library's folder cannot be fsynced after the copy got its name there
- **THEN** the download's file is kept, the import's file saying why

#### Scenario: Left seeding
- **WHEN** the import settings leave torrents seeding
- **THEN** the torrent's files are hardlinked, the download's files are unchanged, and the import's page says it was not moved because the torrent is left seeding

#### Scenario: Hardlink kept by choice
- **WHEN** "Files taken from a download" is hardlink and the download and the destination root are on one volume
- **THEN** the library file is a hardlink of the download's file and the download's file is unchanged

#### Scenario: Hardlink only
- **WHEN** copying is not permitted by the setting and the volumes differ
- **THEN** the import is blocked with that reason, whether it would move or not

### Requirement: A torrent left seeding is not disturbed
Importing a torrent that the import settings leave seeding SHALL NOT move, modify, or change the permissions of its files, and SHALL NOT ask its client anything. A torrent that is not left seeding SHALL be let go of by its client before any of its files is moved (see "A torrent's client lets go of it before its files are moved"). A Usenet download SHALL be removed from its client after its import is recorded. This replaces "A seeding torrent is not disturbed", which said that importing never moves a download's files nor removes a torrent from its client: Axis is not a seedbox, and with moves the files of a torrent that does not seed leave its folder.

#### Scenario: Torrent left seeding
- **WHEN** a torrent's files are imported with the settings leaving torrents seeding
- **THEN** the files in the download folder are unchanged and the torrent remains in the client

#### Scenario: Usenet
- **WHEN** a Usenet download is imported
- **THEN** it is removed from its client afterwards

### Requirement: A torrent's client lets go of it before its files are moved
Before any file of a torrent is moved, the daemon SHALL have its client remove the torrent without its data and SHALL ask the client again until the client itself says it no longer has the torrent; only then SHALL any file be moved. When Axis first asks SHALL be recorded before it asks, and what the client answered before the first file is moved. Every run that moves a torrent's files SHALL ask again first, an earlier release notwithstanding: a torrent added to its client again since SHALL be let go of again before anything is moved. A run that finds the torrent gone after an earlier run asked SHALL NOT say that something else removed it. A client that cannot be reached, or that never says it no longer has the torrent, SHALL make the import wait with nothing moved, saying why, and tried again a few times, counted, then the import SHALL be blocked for a person, who may import anyway; a download whose record no longer lets its client be asked (removed on the Downloads page, lost by its client, its client gone) SHALL block the import at once, the cause said. Which client and which torrent Axis asked SHALL be recorded with the first request. When the release was recorded by an earlier run and the download's record can no longer be used to ask, the client, when it is still set up, SHALL be asked by the torrent's hash: the files SHALL be moved only when its whole list for Axis's category does not hold the torrent; when it does, the import SHALL be blocked as for a torrent added again; when that cannot be told, the import SHALL wait, counted, then be blocked; a client no longer set up SHALL NOT keep the files from being moved. Whenever the files are moved without the client being asked again, or because a person said to import anyway, the import's page SHALL say so and why. A torrent whose client is no longer set up, never let go of, SHALL be hardlinked instead, saying why. A run taken up again after the film got its name SHALL ask again before the sidecars are moved.

#### Scenario: Let go of first
- **WHEN** a torrent is imported with files moved
- **THEN** at the first file's temporary name the client no longer has the torrent, and it removed none of its files

#### Scenario: Client unreachable
- **WHEN** the torrent's client cannot be reached when the import would move its files
- **THEN** the import waits, nothing is moved and no folder is made, and it moves them once the client lets go

#### Scenario: Already gone
- **WHEN** the client says at once that it does not have the torrent
- **THEN** that is recorded (gone), the files are moved, and nothing more is asked of it in that run

#### Scenario: Added again before a retry
- **WHEN** a torrent let go of by an earlier run is at its client again when the import runs again
- **THEN** it is let go of again, without its files, before anything is moved

#### Scenario: Died after asking
- **WHEN** a run dies after asking the client to let go, and the next run finds the torrent gone
- **THEN** the import says that Axis had asked, not that something else removed it

#### Scenario: Its record removed after the release
- **WHEN** a person removed the download on the Downloads page after its client let go of the torrent, before the files were moved
- **THEN** the next run asks the client by the torrent's hash, moves the files when it does not list it, and the import's page says Axis went on without asking it to let go again, and why

#### Scenario: Its record removed, the torrent added again
- **WHEN** the same, and the torrent is at its client again
- **THEN** nothing is moved and the import is blocked for a person, saying the client lists it; once it is gone from the client, a retry moves the files

#### Scenario: Its record and its client gone
- **WHEN** the download's record is gone from Axis after the release, and then its client is removed from Axis
- **THEN** the files are moved, the page saying the client is no longer set up

#### Scenario: Unreachable after the release
- **WHEN** the client let go of the torrent before and cannot be reached at the runs after
- **THEN** the import waits a few times, saying so, then is blocked, and once a person imports anyway the files are moved

#### Scenario: Never let go of, unreachable
- **WHEN** the client cannot be reached, or refuses the removal, at every run before any release was recorded
- **THEN** the import waits a few times, counted, then is blocked naming the cause; once a person imports anyway the files are moved and the page says so

#### Scenario: Never let go of, removed on the Downloads page
- **WHEN** the download was removed on the Downloads page before its import moved anything
- **THEN** the import is blocked at once, saying so, with nothing moved

### Requirement: A move never leaves a file in no place, and an interrupted one converges
Each moved file SHALL have, at every moment, its download name, its temporary name in the library's folder, or its final name. A run that dies at any step of a move SHALL be carried on by the next so that every file is imported once, never doubled and never lost: a file left under its temporary name SHALL be given back its download name when it can be (and the import goes on as if it had not been moved), else its final name when that is free, else kept under its temporary name, recorded, with the import blocked for a person and saying where it is; files moved before the run died SHALL be taken from what the import recorded of them, and the decisions recorded then SHALL stand. A moved file's record not yet linked SHALL be cleared only when its library folder opens without its temporary name in it (then it is in neither place, or the download's name leads to it again); a folder that cannot be opened (renamed, unreadable), or a temporary name that cannot be looked at, SHALL leave the record as it is, the import blocked saying where the file should be. A moved file already linked SHALL NOT keep the import blocked because its folder cannot be opened now. A copy SHALL be taken as made, and removed again on a failure, as soon as it exists. A file kept under its temporary name while it was placed (a sidecar too) SHALL NOT let the import be recorded as imported: it is blocked, naming the file and where it is, and a retry places it. The cleanup SHALL remove a moved file's temporary name only beside a name of its own that leads to it, looked at then (its final name, or the download's); never for its link count, which a hardlink outside the library and the download would count too.

Whenever a temporary name is kept because it may be the file's only name (a copy or a link whose download's file went while it was placed, as a move's), its record SHALL be kept with it (its step and its inode): the import SHALL be blocked naming it and where it is, the cleanup SHALL keep it, and a retry SHALL give it its name. A copy SHALL be recorded as verified as soon as it is; a run taken up again that finds the download's file gone SHALL give a verified copy (or a link) its name, and SHALL keep one not known to be verified for a person, neither named nor removed. The library folder a temporary name was made in SHALL be recorded (its inode) and required when the run is taken up again: another folder at its path SHALL NOT be taken for it, and unless the download still has the file, the import SHALL be blocked saying the folder was replaced and where the file was put. A film given its name by an earlier run that is not under that name now SHALL block the import, saying what is there, never fail the run on every attempt. A folder or a temporary name that cannot be read now for an error time may clear (an I/O error, access refused, no file descriptor left) SHALL be looked at again a few times, counted, before the import is blocked.

#### Scenario: Died with the file under its temporary name
- **WHEN** a run dies after renaming the film into the library under its temporary name, and runs again
- **THEN** the film is put back and imported again once, and the library holds one film file, the download's own

#### Scenario: Died with the download's folder gone meanwhile
- **WHEN** something else deleted the download's folder before the next run
- **THEN** the film is given its final name, the import completes, and nothing is lost

#### Scenario: Neither name free
- **WHEN** another file has taken the film's download name and its final name
- **THEN** the film keeps its temporary name, the import is blocked saying so, and once the final name is free a retry completes it

#### Scenario: Died after the film was moved
- **WHEN** a run dies after the film was moved and published, before its subtitle was placed
- **THEN** the next run places the subtitle, decides nothing differently, and imports the film once

#### Scenario: Its library folder cannot be opened
- **WHEN** a run died with the film under its temporary name, and its library folder was renamed before the next run
- **THEN** the record is kept, the import is blocked naming the folder and the temporary name, never ended as files gone, and once the folder is back a retry imports the film once

#### Scenario: A hardlink outside
- **WHEN** a file kept under its temporary name has another name outside the library and the download
- **THEN** the hourly cleanup keeps the temporary name

#### Scenario: A sidecar kept
- **WHEN** a subtitle could be neither given its name nor put back
- **THEN** the import is blocked naming it and where it is, not recorded as imported, and a retry places it

#### Scenario: An episode linked, its season's folder renamed
- **WHEN** a series' import is taken up again after its first episode was moved and linked, and its season's folder was renamed meanwhile
- **THEN** the other episode is placed and the import completes

#### Scenario: Died before the rename, the download deleted
- **WHEN** a run died after recording a file's step before renaming it, and something else deleted the download
- **THEN** the import ends as files gone, never saying the file was moved

#### Scenario: A link whose download's name went, its name taken
- **WHEN** a hardlink's download name is deleted after the link was made, and the film's final name is taken
- **THEN** the temporary name keeps the file and its record, the import is blocked naming it, the cleanup keeps it, and once the name is free a retry gives it its name

#### Scenario: The movie's folder replaced between the two renames
- **WHEN** a run died with the film under its temporary name, and a person renamed the movie's folder and made a new one of the same name
- **THEN** the import is blocked saying the folder was replaced and where the file is, never ended as files gone; with the folder back, a retry imports the film once

#### Scenario: The film's name changed after it was given
- **WHEN** a run died after the film got its name, and the film's file was then renamed in the library
- **THEN** the import is blocked saying nothing (or another file) has that name now, and once the file is back a retry completes it

#### Scenario: Its folder unreadable for a while
- **WHEN** a run died with the film under its temporary name, and its folder cannot be read (access refused) at the next runs
- **THEN** the import waits a few times, counted, then is blocked; once the folder can be read a retry imports the film

### Requirement: After a move, the folder is told by what the move left
For an import that moved its files, the identity check of its leftover folder SHALL be, besides the rules of the place: no file of the import is kept (a move across volumes that kept the download's file leaves the folder, naming the file and why); the folder at the place has the inode the import recorded for it when it walked it, before anything was moved out; every video the import moved is in the library where it was placed, a regular file with the inode recorded; and nothing in the folder, within the removal's bounds, is a file of feature size (a quarter of the smallest video moved, and at most 1 GiB) that does not go with it: a video the import looked at and did not take, a file that is not media (a disc image, an archive), or anything the import did not record. What the import classified as a sample or an extra it does not import, and every smaller file, SHALL go with the folder. Every download Axis follows at the client that is not finally closed SHALL count among the downloads that keep a folder in or around them, by what the client says of it now (its place, in progress too; or its name), and a torrent left seeding by its place; the client SHALL be asked once per run of the folder step, after whether the target is already gone is decided. When what the client has cannot be fully known (it cannot be asked, its list is not whole or holds what Axis could not read), the folder SHALL wait. A download the client keeps loose in its folder for Axis SHALL make a file straight in that folder wait, and SHALL NOT keep a job's own folder (proven by the rules below). A folder that fails any of these SHALL be left, the import naming why (and a file that keeps it, by name); a folder already gone SHALL be "gone", not an error.

#### Scenario: Another video in it
- **WHEN** a feature-size video the import knows nothing of is in the folder
- **THEN** the folder is left and the import names the video

#### Scenario: A video the import did not take
- **WHEN** the import looked at a video of feature size in the folder and did not take it (too short to be the film and not named as an extra)
- **THEN** the folder is left and the import names it

#### Scenario: A disc image
- **WHEN** a large file that is not media is in the folder
- **THEN** the folder is left and the import names it

#### Scenario: A download's file kept
- **WHEN** a move across volumes kept a download's file because its copy was not in the library
- **THEN** the folder is left, naming the file and why

#### Scenario: A download in progress in it
- **WHEN** the client says a download Axis follows, still in progress, has its files in the folder
- **THEN** the folder is left

#### Scenario: A download kept loose
- **WHEN** the client keeps a torrent loose in its folder for Axis, and an import's own job folder is to be removed
- **THEN** the job's folder is removed; and a job's file moved out of that folder is recorded as gone

#### Scenario: Another folder of its name
- **WHEN** the folder was deleted and another made at its name
- **THEN** it is left: it is not the folder the import walked

#### Scenario: The moved file not in the library
- **WHEN** the file the import moved is not in the library as it was placed
- **THEN** the folder is left, saying so

#### Scenario: Gone already
- **WHEN** something else deleted the folder after the import
- **THEN** it is recorded as gone

### Requirement: A download's files gone before its import end it, and its title is wanted again
When a completed download's files are no longer at its place (deleted or moved by something else) before anything of it was imported, the daemon SHALL end the import as final ("files gone"), saying where the files were and when Axis last saw them there, and SHALL never try it again. Imports that wait (storage read-only, a volume offline or read-only) or are blocked for a person SHALL be looked at for this hourly, through a read guard; a volume that is offline SHALL tell nothing. The statement that ends an import so SHALL itself require that nothing of it is placed or moved and, for the hourly sweep, that it still waits or is blocked with no job running it, so that a sweep that saw it waiting never ends an import that began and moved its file meanwhile. A run whose import is not running when its end is to be recorded SHALL fail, saying so, and never report it imported. The grab SHALL then be closed with no blocklist entry, the download removed from its client without its data, and the title wanted again, a search for the next best queued when it is monitored.

#### Scenario: Deleted while waiting
- **WHEN** an import waits because storage is read-only and a cleanup of the download folder deletes its files
- **THEN** the hourly cleanup ends the import as files gone, saying where and when, it is not tried again, its grab closes, and the movie is searched for again

#### Scenario: Blocked, not monitored
- **WHEN** an import blocked for a person loses its files and its movie is not monitored
- **THEN** the import ends as files gone and the movie is wanted, with no search queued

#### Scenario: Volume offline
- **WHEN** the download's volume is offline
- **THEN** nothing is concluded and the import keeps waiting

#### Scenario: The sweep during a move
- **WHEN** the sweep found an import of one file waiting, and comes to end it after the import began and moved the file
- **THEN** it does not end it, and the import completes

### Requirement: A download's files deleted during an import leave no half-made file
When something else deletes a download's file while it is imported, the import SHALL complete with what it has, noting what was missing, or fail leaving nothing; a library file SHALL never be left under its final name unless it is complete and, for a copy, verified.

#### Scenario: A subtitle deleted while the film is moved
- **WHEN** the subtitle is deleted after the film is in the library
- **THEN** the import completes with the film and says the subtitle was not placed

#### Scenario: The source deleted while copied
- **WHEN** the download's file is deleted while it is copied to another volume
- **THEN** the copy is read to its end from the file opened, verified, and the import completes; the download's file is recorded as gone

#### Scenario: The source deleted while copied, the name taken
- **WHEN** the same, and the film's final name is taken when the verified copy would get it
- **THEN** the copy, the only one, keeps its temporary name and its record, the import is blocked naming it, the cleanup keeps it, and once the name is free a retry gives it its name

#### Scenario: The source deleted after a crash
- **WHEN** a run died with the verified copy under its temporary name, and the download's file was then deleted
- **THEN** the next run gives the copy its name and the import completes

#### Scenario: A copy not known to be verified
- **WHEN** a run died before it recorded that the copy was verified, and the download's file is not in the download at the next run
- **THEN** the copy is neither named nor removed, the import is blocked naming it; with the download's file back, a retry copies again and imports once

### Requirement: A seeding torrent's files deleted after its import are said to be gone
When something else deletes the files of a torrent left seeding after its import, the hourly cleanup SHALL record on the import that its files at the client are gone and that the torrent cannot seed them, and whether the library's files (separate names) are in place, looked at on disk.

#### Scenario: Deleted by a cleanup
- **WHEN** a cleanup by age deletes a seeding torrent's folder after its import
- **THEN** the import says its files at the client were deleted by something else and the library's files are in place, and the library's file is intact

### Requirement: Leftovers in the clients' folders are listed and deleted one at a time
Administrators SHALL have a page, reached from the Downloads page and the dashboard while it is not empty, that lists for each download client with a folder of Axis's own (its category or label folder, through its path mapping) what is in that folder that no download Axis follows and no import still open accounts for: its name, size, when it changed, and whether Axis's rules would let it be deleted and why not. Every download Axis follows at that client that is not finally closed SHALL account for its entry by what the client says of it now (its place, in progress too, or its name), and a torrent left seeding by its place; while one has no place known (being sent, or not listed, or the client cannot be asked), no entry SHALL be offered for deletion, each saying it may belong to that download. A name the client gives a download it has not finished (a job being unpacked, an incomplete file) SHALL never be offered; a failed job's folder SHALL be the client's while it still lists that job, and a leftover a person may delete once it does not. When the client's list cannot be had, is not whole, or holds what Axis could not read, or a download is kept loose in the folder, nothing (no top-level entry) SHALL be offered, the page saying why; an entry whose name differs from a download's only by case or Unicode normalisation SHALL NOT be offered. The count the Downloads page and the dashboard show SHALL be of what a person can delete; the rest SHALL be listed apart. The list SHALL be bounded and SHALL delete nothing. Each entry SHALL have a deliberate, confirmed Delete posted with the session's CSRF field that asks every rule again and goes through the volume write layer's strict removal: only one name directly in the client's folder for Axis, the very entry listed, never following a symbolic link, never entering another filesystem, never while storage is read-only or the volume is offline or mounted read-only. An entry that vanished between the list and the Delete SHALL be said to be gone, not an error.

#### Scenario: Listed
- **WHEN** an administrator opens the page and the client's folder for Axis holds a folder of a download given up on and the folder of a download Axis still follows
- **THEN** the first is listed with its size and whether it may be deleted, and the second is not listed

#### Scenario: Deleted
- **WHEN** the administrator confirms the deletion of a listed folder
- **THEN** it is deleted with what is in it, through the strict removal

#### Scenario: In progress, seeding, unfinished
- **WHEN** the folder holds a download in progress the client names, a torrent left seeding, and an unpacking job's folder
- **THEN** the first two are not listed, and the third is listed and never offered for deletion

#### Scenario: A download whose place is not known
- **WHEN** a download Axis follows at the client is being sent
- **THEN** no entry is offered for deletion, each saying it may belong to that download

#### Scenario: The list fails
- **WHEN** the client cannot list what it has, or lists something Axis cannot read
- **THEN** nothing is offered for deletion, and the count is 0

#### Scenario: A failed job
- **WHEN** the folder holds a failed job's folder its client no longer lists, and a job being unpacked
- **THEN** the first may be deleted and is counted, the second is listed and neither offered nor counted

#### Scenario: Gone since the list
- **WHEN** the entry was deleted by something else after the list was made
- **THEN** the page says it was already gone

#### Scenario: Read-only
- **WHEN** storage is read-only
- **THEN** nothing is deleted and the page says why

#### Scenario: Not for others
- **WHEN** a user who is not an administrator opens the page, or a post lacks the CSRF field
- **THEN** it is refused

### Requirement: Moves keep upgrades and the recycle folder as they were
An upgrade with files moved SHALL place the new file (moved) before the old one is renamed into the recycle folder of its volume, an upgrade to the same name SHALL link the old file into the recycle folder and swap the names, and the recycle rules (retention, removal by the cleanup of only what was recorded) SHALL be unchanged.

#### Scenario: Upgrade by a move
- **WHEN** a better release of a film in the library is imported with files moved
- **THEN** the new file is the download's own, the film is never without a file, and the old one is in the recycle folder until its retention ends

#### Scenario: Same name, interrupted
- **WHEN** a run dies after moving the new file under its temporary name with the swap still to make
- **THEN** the next run completes the upgrade once, the old file in the recycle folder

### Requirement: Every client that could account for an entry is asked
An entry of a client's folder for Axis SHALL be offered for deletion on the page "Leftovers in the download folders", and a download's folder SHALL be removed after its import, only when no download at any client, no download Axis follows and no import still open accounts for it. The client itself, and every other client configured (switched off in Axis too: its program may still run) one of whose path mappings reaches the folder (onto its volume, at a place that holds it or lies inside it), SHALL be asked where it keeps Axis's downloads and everything it has now, in every category or label (another program's downloads, and a torrent a person moved to a category of their own, among them), and its downloads in or around the entry, by their place or, without one, by their name exactly as the client gave it, SHALL account for it. A torrent Axis left at its client after its import (seeding, or moved by a person to a category of their own) SHALL account for its place whatever its download's state says. When such a client cannot be asked, or its list is not whole or holds an entry Axis cannot read, nothing in the folder SHALL be offered, a download's folder there SHALL wait, and what is said SHALL name the entry (its identifier and name, as far as they can be read) and why. When another client's folder for Axis is the same folder, or one holds the other, or cannot be worked out (it keeps Axis's downloads in no folder of their own, or in none Axis can use), the folder SHALL NOT be taken for the client's own: the page "Leftovers in the download folders" SHALL list nothing in it and say which client, the client's page SHALL say so with its problems, and a download's folder there SHALL wait, saying why.

#### Scenario: Two clients with one folder for Axis
- **WHEN** a Usenet client and a torrent client keep Axis's downloads in the same folder, and a torrent of the second is still downloading there
- **THEN** the first client's Leftovers list nothing in the folder and say it is not its own, naming the second client, and asked to delete the torrent's folder, Axis deletes nothing

#### Scenario: Another client's download inside the folder
- **WHEN** another client with a folder of its own has a download of Axis's saved inside the first client's folder for Axis
- **THEN** that download's entry is not offered, and an entry no download accounts for may still be deleted

#### Scenario: Another client cannot be asked
- **WHEN** another client whose path mappings reach the folder cannot be asked what it has
- **THEN** nothing in the folder is offered for deletion, the entries naming that client, and once it answers again a leftover may be deleted

#### Scenario: A name with its white space
- **WHEN** a download the client names, with two spaces in its name and no place said, has a folder of exactly that name in the client's folder for Axis
- **THEN** that folder is not offered for deletion

#### Scenario: A torrent a person took over
- **WHEN** a torrent was imported by hardlink after a person moved it to a category of their own, so that its download is gone to Axis while its client still seeds it
- **THEN** its folder is not offered for deletion

#### Scenario: Another client switched off in Axis
- **WHEN** a client switched off in Axis, still configured, has a download of another category in the first client's folder for Axis
- **THEN** that download's entry is not offered, and an entry no download accounts for may still be deleted

#### Scenario: An entry that cannot be read
- **WHEN** the client lists an entry whose identifier is read and not its name
- **THEN** nothing in the folder is offered, and each entry says which entry and why

#### Scenario: Another client's folder that cannot be worked out
- **WHEN** another client whose path mappings reach the folder keeps Axis's downloads in no folder of their own
- **THEN** the page "Leftovers in the download folders" lists nothing in the folder and says it may not be the client's own, naming the other client, and the client's page says so after its next look; once the client itself has no folder of its own, its page no longer says it

### Requirement: A dismissal never strands a file under its temporary name
Dismissing an import, or assigning its download to another title, SHALL first settle every file an interrupted run of it left under its temporary name, within its own transaction and on its own database connection: a file moved out of the download SHALL be put back under its download name, never over a name taken; a copy whose download's file is still there, the very file (its inode, its size and both its times as recorded when it was copied), and a link or a move whose download's name still leads to its file, need nothing. When a file cannot be put back, or is a copy or a link that may be its file's only name, or what is on disk cannot be told now, the action SHALL be refused, saying where the file is, and the import SHALL stay as it was. The hourly cleanup SHALL settle so the imports dismissed before, and SHALL open again for a person, blocked and saying where the file is, only one shown by what is on disk now to have a file under its temporary name that cannot be put back (its download's folder gone, its download's name taken, a link's or a copy's other name gone); one that cannot be told now (a volume away or mounted read-only, a folder or a file that cannot be opened for another reason than its absence, a copy whose times were never recorded) SHALL be left as it is and looked at again the next time, and while storage is read-only nothing at all SHALL be written. Each run SHALL go on after the last import it looked at, so that one that cannot be settled yet holds back none after it.

#### Scenario: Dismissed after a crashed move
- **WHEN** a run died with the film moved out of the download under its temporary name, and a person dismisses the import
- **THEN** the film is back in the download under its own name, the very file, nothing is left under a temporary name, and the import is dismissed

#### Scenario: It cannot be put back
- **WHEN** the same, and the download's folder is not there
- **THEN** the dismissal, and an assignment to another title, are refused, saying where the file is, and nothing changes

#### Scenario: Dismissed before this rule, put back
- **WHEN** such an import was dismissed without its file being settled, and the hourly cleanup runs
- **THEN** the film is put back in the download and the import stays dismissed

#### Scenario: Dismissed before this rule, its download gone
- **WHEN** the same, and the download's folder is gone
- **THEN** the import is open again, blocked, saying it was dismissed and where the file is, and a retry gives the film its name, once

#### Scenario: A copy that may be the only one
- **WHEN** a run died with a verified copy under its temporary name, and the download's file is not at its place
- **THEN** the dismissal is refused, saying the copy may be the only one, and the cleanup keeps it

#### Scenario: Storage read-only, or a folder that cannot be opened now
- **WHEN** such an import was dismissed without its file being settled, and the hourly cleanup runs while storage is read-only, or while the library's folder or the download's folder cannot be opened (not gone)
- **THEN** nothing is put back or written, the import stays dismissed, and once that passes the file is put back

#### Scenario: One that cannot be settled yet holds back none
- **WHEN** of two such imports the first cannot be told now, and the cleanup looks at one a run
- **THEN** the second is put back at the next run, and the first in its turn once it can be told

#### Scenario: One connection
- **WHEN** a dismissal settles a file while the database pool holds one connection only
- **THEN** it does so on the connection it holds, and the import is dismissed

### Requirement: A torrent a person moved elsewhere is imported without a move
When a torrent's client has it in another category or label than Axis's when its import would move its files (a person moved it there, to keep it), the import SHALL leave it at its client, untouched, SHALL take its files without moving them (a hardlink on one volume, a verified copy across volumes), SHALL record that its client keeps it and why, and SHALL remove or delete nothing of it then or afterwards.

#### Scenario: Moved to the person's own category
- **WHEN** a torrent completed in Axis's category is moved by a person to another category before its import
- **THEN** its import hardlinks its files, says the torrent is not Axis's any more, the torrent is still at its client with its files, and the hourly cleanup deletes nothing of it

### Requirement: A copy's temporary name goes while its source is there
The hourly cleanup SHALL remove the temporary name of a copy that no run takes up (its import dismissed, blocked or imported), not given its final name, while the download's file it was copied from is still at its place, the very file, untouched since: its inode and size, and its modification and change times as recorded when it was copied (as a move across volumes deletes it), looked at on the download's own volume; a copy whose download's file is not there, is another file now, or whose times were never recorded, SHALL be kept.

#### Scenario: Dismissed with its copy, the download's file there
- **WHEN** a run died with its copy under its temporary name, the import was dismissed, and the download's file is still there
- **THEN** the hourly cleanup removes the copy's temporary name, and the download's file stays

#### Scenario: The download's file touched since
- **WHEN** the same, and the download's file's times changed after it was copied (the same inode and size)
- **THEN** the copy is kept

### Requirement: An upgrade retires only the old file's own subtitles
An upgrade SHALL retire, with the old file, only the subtitles of its own name: a subtitle whose name begins with the name of another video beside it, longer than the old file's, SHALL be that video's and stay.

#### Scenario: Another cut beside the old file
- **WHEN** the old file "Film.1080p.mkv" lies beside another cut "Film.1080p.Extended.mkv", each with an English subtitle, and an upgrade of the first is imported
- **THEN** the old file and "Film.1080p.en.srt" go to the recycle folder, and the other cut and its subtitle stay

### Requirement: The import of a release grabbed by hand sets the profile aside
The import of a download whose grab a person chose with the profile set aside (Grab this one, or Grab it anyway on a release the profile rejected, from this version on; an older grab marked only "manual" sets nothing aside) SHALL NOT be blocked by the quality profile (its qualities, sizes, rules, unknown quality, whether it upgrades and its cutoff) for a file whose every episode is among the episodes that grab claimed, as its confirmation showed. Any other file of the download SHALL be judged by the ordinary rules, as if no grab by hand existed. It SHALL NOT replace a file in the library that the imported file is not better than, unless the person ticked replacing exactly that file (recorded with its quality as shown); a better file that arrived after the confirmation SHALL need the ordinary rule. Every other check SHALL apply as for any import: that each file is the title and the episode the grab was for (but for a grab by force, whose import is bound to what the person named), the runtime, samples, archives, a file that cannot be read, a profile that cannot be used, the hardlink and copy rules, and the read-only guard. A series' grab by hand whose import can take no file, with nothing a person could import anyway, SHALL be closed, its episodes freed, and the series' history SHALL say why; but a forced grab of a series whose download holds video files that cannot be placed on the episodes named SHALL stay open while its import is held for a person. A movie's grab by hand SHALL be closed in that case only when it was forced: the movie's history SHALL say why, and while that import stays blocked the movie SHALL NOT be searched for automatically and its page SHALL say what to do. A movie's grab by hand that is not forced SHALL stay open.

#### Scenario: The profile rejects the file
- **WHEN** the download of a release grabbed by hand is a file the title's profile rejects as too large
- **THEN** it is imported

#### Scenario: Not better than the file in the library
- **WHEN** the download of a release grabbed by hand, without the tick, is not better than the episode's file in the library
- **THEN** the import is blocked with "not better than the file on disk", and with the tick for that file it replaces it

#### Scenario: An episode the grab did not claim
- **WHEN** a pack grabbed by hand also holds a file of an episode it did not claim
- **THEN** that file is judged by the ordinary rules, and the profile's objections and the tick do not reach it

#### Scenario: An accepted grab
- **WHEN** an administrator grabs a release the profile accepts, or an older grab marked manual is imported
- **THEN** its import is judged by the profile as before

#### Scenario: Another title's file
- **WHEN** a release grabbed by hand, and not by force, holds a file whose name says another series
- **THEN** that file is not imported

#### Scenario: A forced movie grab whose import can take no file
- **WHEN** the download of a release forced for a movie holds no video file that can be taken
- **THEN** the grab is let go with the reason in the movie's history, the import stays blocked for a person, and no automatic search runs for the movie while it does

### Requirement: The import of a forced grab is bound to what the person named
The import of a download whose grab was forced SHALL NOT judge the title in a file's name. A movie's video file SHALL be imported as the movie the grab was for. A series' download that holds one video file SHALL have it imported as the episode the grab claimed, or as the several it claimed only when the person ticked on the confirmation that one file holds them (otherwise its own name must state them, else it waits for assigning). One that holds several SHALL have each file placed by the episode numbers in its own name, and only on episodes the grab claimed. A file whose name states a season that the series has under the numbering followed SHALL be read as that season, so a file of another season of a pack SHALL be left and listed for assigning, never imported elsewhere by the grab. Only when everything the grab claimed is in one season SHALL a name whose season the series does not have, or that states no season, be read by its episode numbers within that season, and then only when no video file of the download states that season itself and every file read so states one and the same season, or none. A file whose name gives none of the claimed episodes, or the same episode as another file, SHALL NOT be imported and SHALL say it can be assigned by hand. A pack some of whose files were placed SHALL end imported as any pack does: its grab closed, the claimed episodes that got no file free again, and each file left listed on the import with its reason. It SHALL replace only the library files the person ticked on the force confirmation, whether or not the new file is better; a file that would replace a library file that was not ticked, or that changed since, SHALL be blocked, and only a person's "import anyway" SHALL let it replace that file. Every check that is not about the name SHALL apply as for any import: a readable video, the choice of one video among several, the runtime, samples, archives, a movie split into parts, the hardlink and copy rules, the volume guards. When no file can be taken the import SHALL be held for a person, with Assign by hand offered when the download holds a video file, and nothing SHALL be guessed. The link it makes SHALL be marked as made by a person through a forced grab, with who and when, and kept by hand.

#### Scenario: A movie whose file is named otherwise
- **WHEN** the download of a release forced for Copper Heron (1995) holds one video file whose name reads as another title
- **THEN** it is imported as Copper Heron (1995), named as the import names a movie's file, and its link says it came from a forced grab by that person

#### Scenario: One file for the episode named
- **WHEN** the download of a release forced for S01E03 holds one video file with no episode in its name
- **THEN** it is imported as S01E03

#### Scenario: A forced pack whose files say their episodes
- **WHEN** a pack forced for season 1 holds files named E01 to E04 under another series' title
- **THEN** each is imported as that episode of season 1

#### Scenario: A file that cannot be mapped
- **WHEN** a pack forced for season 1 holds a video file whose name gives no episode
- **THEN** that file is not imported, its reason says to assign it by hand, and the others are imported

#### Scenario: A file not ticked
- **WHEN** a forced grab's download would replace a library file that was not ticked on its confirmation
- **THEN** the import is blocked for that file and the library file is not replaced

#### Scenario: Far from the runtime
- **WHEN** the file of a forced movie runs half the movie's runtime
- **THEN** the import is blocked with the runtime reason, as any import is

### Requirement: A download's files can be assigned by hand
An administrator SHALL be able to assign by hand the files of an import that is blocked or partly imported and holds a video file, and, for a movie, the files of a completed download that has no open import (the manual import page's tick "Assign by hand" opens an import that reads the files and imports nothing until they are assigned). For each video file the person says that it is the movie (one file at most), or which episodes of the series it holds (episodes of one season, written as S01E03 or S01E03 S01E04, in the numbering the series follows now), or that it is left alone. A review SHALL show, before anything is done, what each chosen file becomes and is named, each library file it would replace with its quality and whether the new file is better, its runtime beside the expected one, and the files left alone; it SHALL be the import's own decision on the files its last run recorded, so that the run does what the review showed. Replacing a library file SHALL need a tick for that file, and a runtime outside the tolerance a tick for that file. Applying SHALL be refused when the import is no longer blocked or partly imported, when another grab claims a chosen episode, when two files are given the same episode, and when the review is out of date. An assigned file SHALL be imported without judging its name, whether it is wanted, or the quality profile; it SHALL still have to be a readable video, and the volume guards, verification and placing rules SHALL apply. A missing tick SHALL never be overridden: "import anyway" SHALL NOT let an assigned file replace a library file, and a tick SHALL hold only while that library file has the quality the review showed. When one assigned file cannot be taken as assigned, none of the assigned files SHALL be imported, nothing in the library SHALL be replaced, and the import SHALL be held to be assigned again. A file that was not chosen SHALL NOT be imported, moved or deleted by the assignment, and after an import whose files were assigned the download's folder SHALL NOT be deleted. A replaced file SHALL be retired to the recycle folder by the rules of any upgrade. When the chosen files are placed the import SHALL end imported, its grab SHALL be closed, and episodes it claimed that got no file SHALL be free again. A download held because the series changed its numbering after the grab SHALL have its files read and recorded, and SHALL be imported once its files are assigned. The assignment SHALL be recorded with the person's name on the import and in the title's history, and each link it makes SHALL be marked as made by a person through an assignment, and kept by hand.

#### Scenario: A pack none of whose files matched
- **WHEN** a series' import is blocked because no file's name gives an episode, and an administrator assigns two of its four video files to S01E01 and S01E02 and applies
- **THEN** those two are imported and named as S01E01 and S01E02, the other two are left in the download with the reason that they were left alone, and the import ends imported

#### Scenario: A movie's file chosen
- **WHEN** a movie's import is blocked because its file's name reads as another movie, and an administrator assigns that file as the movie
- **THEN** it is imported as the movie

#### Scenario: A grab let go as not imported
- **WHEN** a grab by hand was closed because its import could take no file, and an administrator assigns a file of that import to an episode
- **THEN** the file is imported for that episode

#### Scenario: Grabbed under another numbering
- **WHEN** a download is held because the series changed its numbering after the grab, and an administrator assigns its file to S02E01
- **THEN** it is imported as S02E01 in the numbering the series follows now

#### Scenario: It would replace a file
- **WHEN** the chosen episode has a file and the administrator applies without ticking it
- **THEN** nothing is applied and the review says which tick is missing; with the tick, the new file is placed and the old one goes to the recycle folder

#### Scenario: An assignment without its tick
- **WHEN** an assignment is recorded for a file whose episode has a library file that was not ticked
- **THEN** the import is blocked, "import anyway" is refused, and nothing in the library is replaced

#### Scenario: Not a video
- **WHEN** a post assigns a file that is an archive
- **THEN** it is refused

#### Scenario: Out of date
- **WHEN** another file of the series is imported for a chosen episode after the review was shown
- **THEN** nothing is applied and the review is shown again

#### Scenario: A viewer
- **WHEN** someone who is not an administrator posts an assignment
- **THEN** it is refused

### Requirement: Duplicates are listed with a recommendation
A page named Duplicates SHALL list every movie with more than one linked video file that is present, and every episode the numbering has with more than one, each file with its quality, size, running time, disk, how it was linked and whether it is kept by hand, pinned, or on a disk that cannot be written. A file that holds several episodes SHALL NOT be a duplicate by itself, and a video file linked to nothing SHALL NOT be listed. The page SHALL recommend a file to keep, and set a tick for another, only when the files are copies of one thing as far as Axis can tell: every file of the group has an analysis that read the file as it is now (status ok, a running time, a size above 0), no two running times differ by more than 5% of the shorter, and no file name states a part of a set that another does not. Otherwise it SHALL recommend nothing, set no tick, and say why in plain words. When it recommends, it SHALL say in words with the reason which file to keep, telling two files apart as an upgrade does: by the title's quality profile when it ranks the two qualities in different tiers, else by the standard quality ladder (a quality the profile does not list SHALL NOT lose for that alone; unlike an upgrade, two qualities in one tier of the profile are also told apart by the ladder), then within one quality by the revision and the score, then the larger file, then the one kept or linked by hand. When nothing tells two files apart, nothing is known of the quality of one, or two files of one quality differ in size while their revision and score cannot be read, the page SHALL say that Axis cannot tell and SHALL recommend nothing. Nothing SHALL be recommended for removal when the file to keep is on a disk that is offline or mismatched. A file kept or linked by hand SHALL NOT be recommended for removal without saying that a person linked or keeps it. A pinned file, a file on a disk that is offline, mismatched or read-only, and the only file of another episode on a disk that is online SHALL NOT be offered for removal. Nothing SHALL be removed by the page being shown. The page SHALL link to Disk usage's report of likely duplicate files, which stays the one list of files with the same content and of titles in more than one place.

#### Scenario: A movie with two files
- **WHEN** Copper Heron (1995) has a WEB-DL 1080p file and an HDTV 720p file linked, and its profile prefers WEB-DL 1080p
- **THEN** the page lists the movie with both files, recommends keeping the WEB-DL 1080p file and removing the other, and says why

#### Scenario: A double episode
- **WHEN** one file is linked to S01E01 and S01E02 and neither episode has another file
- **THEN** the page lists nothing for the series

#### Scenario: An episode with two files
- **WHEN** S01E03 has two files linked
- **THEN** the page lists S01E03 with both

#### Scenario: Axis cannot tell
- **WHEN** a movie's two files have the same quality and the same size, and neither is kept by hand
- **THEN** the page says that Axis cannot tell which is better and recommends nothing

#### Scenario: Kept by hand
- **WHEN** the worse of two files is kept by hand
- **THEN** the page says that Axis would remove it but a person keeps it, and its tick is not set on the review

#### Scenario: Two parts of one movie
- **WHEN** a movie has two files linked that run 52 and 41 minutes, or that are named CD1 and CD2
- **THEN** the page says that they may not be copies of the same thing, recommends nothing, and sets no tick

#### Scenario: A file that could not be read
- **WHEN** the file with the better name could not be read by the analysis, was never analysed, changed since it was analysed, or is empty
- **THEN** the page says that Axis could not read one of the files, recommends nothing, and sets no tick

#### Scenario: A quality the profile does not list
- **WHEN** a movie has a Remux 1080p file that its profile does not list and a WEB-DL 720p file that the profile lists
- **THEN** the page recommends keeping the Remux 1080p file, as an upgrade would replace the other with it

#### Scenario: A viewer looks
- **WHEN** someone who is not an administrator opens the page
- **THEN** the groups are shown, with no way to review or remove

### Requirement: A duplicate is removed only through the recycle folder, by a person
An administrator SHALL be able to review one group: the review SHALL list exactly which files would go to the recycle folder and which stay, each removable file with a tick of its own, set beforehand only for a file Axis recommends removing that was not kept or linked by hand, and it SHALL write nothing. Removing SHALL be one post with the review's digest. It SHALL be refused, with nothing removed, when the digest no longer matches, when every file of the group is ticked, when more than 20 files are ticked, when no file that stays is one the title can be left with, when a ticked file is pinned, on a disk that cannot be written or the only file of another episode, when a ticked file is no longer linked to the movie or the episode of the group, when an import of the title is open, when a grab of the movie or the episode is open, and for someone who is not an administrator. A file the title can be left with SHALL be on a disk that is online, on that disk at that moment as the library records it (a regular file with the recorded inode and size), not empty, and read by the analysis as it is now; a row of the index that says present SHALL NOT be enough. Every other episode that a ticked file holds SHALL keep such a file too, judged by the same test, or the removal SHALL be refused. The digest SHALL cover everything the review showed and the recommendation stood on, among it who linked a file by hand, what else a file holds, and each file's analysis and running time. A file that was not ticked SHALL NOT be removed. A ticked file SHALL be retired into the recycle folder of its own disk by the import's own path for a replaced file, never deleted directly, and its link SHALL go with its index row; it SHALL be deleted from the recycle folder after `import.recycle_retention` by the cleanup job. Subtitle files beside a removed file SHALL be left where they are, and the review SHALL say so. When that retention is 0 the review SHALL say that the file is deleted for good. When a ticked file cannot be moved after the checks passed, the other ticked files SHALL still be moved, and the page SHALL say how each went. The removal SHALL be recorded in the title's history with the person's name, where each file is in the recycle folder, and when it is deleted from there. What the page says afterwards SHALL be what happened: a file that was moved while its record could not be written SHALL be said to be in the recycle folder, never "not removed", and the cleanup job SHALL finish such a record, or forget it when the file never moved. A page SHALL list the duplicates removed, with where each is, what it was a file of, who removed it and when, and when it is deleted; it SHALL offer no restore.

#### Scenario: The recommended file removed
- **WHEN** an administrator reviews Copper Heron (1995), leaves the tick on the HDTV 720p file and confirms
- **THEN** that file is in the recycle folder of its disk, it is no longer linked to the movie, the WEB-DL 1080p file is untouched, and the movie's history names the person and both files

#### Scenario: Every file ticked
- **WHEN** a post ticks every file of the group
- **THEN** it is refused and no file is moved

#### Scenario: A pinned file
- **WHEN** a post ticks a file that is hard-linked elsewhere
- **THEN** it is refused and no file is moved

#### Scenario: Its disk cannot be written
- **WHEN** a post ticks a file while Read-only storage is on, or on a disk that is offline
- **THEN** it is refused and no file is moved

#### Scenario: An import is open
- **WHEN** an import of the movie is blocked and a post removes one of its duplicates
- **THEN** it is refused and no file is moved

#### Scenario: The page is out of date
- **WHEN** a file of the group changed after the review was shown
- **THEN** nothing is removed and the review is shown again

#### Scenario: A viewer posts
- **WHEN** someone who is not an administrator posts a removal
- **THEN** it is refused

#### Scenario: Nothing is kept in the recycle folder
- **WHEN** `import.recycle_retention` is 0
- **THEN** the review says that the removed file is deleted for good

#### Scenario: A shared file whose other episode has no real file
- **WHEN** S01E01 has a file A that also holds S01E02 and a file B, S01E02 has A and a file C, C is recorded as present but is gone from its disk (or could not be read, or was never analysed), and a post ticks A on S01E01
- **THEN** it is refused, no file is moved, and the refusal names S01E02 and C

#### Scenario: A ticked file linked elsewhere meanwhile
- **WHEN** a ticked file's link is moved to another episode while the removal is under way
- **THEN** that file is not moved to the recycle folder

#### Scenario: The file that would stay cannot be read
- **WHEN** a post ticks the complete file of a movie and the file that would stay could not be read by the analysis
- **THEN** it is refused and no file is moved

#### Scenario: The database is lost after a file moved
- **WHEN** a ticked file was moved into the recycle folder and its record cannot be written, though tried twice
- **THEN** the page says that the file is in the recycle folder and that the library still lists it, the history says the same, and the cleanup job later marks the recycle row kept and drops the file's index row

#### Scenario: A removed duplicate found again
- **WHEN** a duplicate of Copper Heron (1995) was removed
- **THEN** the movie's history names its place in the recycle folder and when it is deleted, and the list of recycled duplicates shows the file, Copper Heron (1995), who removed it and when

### Requirement: An administrator can delete the download of an import that waits for a person
The page of an import that is blocked, waiting or partly imported, and whose files a download of Axis's brought, SHALL offer an administrator "Delete this download": beside "Dismiss this import", or, on a partly imported import, in a card of its own. On the Downloads page, the row of a completed download whose import is blocked or partly imported SHALL link an administrator to the same confirmation. The confirmation SHALL write nothing. It SHALL show the place that would be deleted on the volume and at the client, whether it is a folder or one file, the number of files and their total size, who deletes them (the client, or Axis), how many of the files are also in the library, the files this import already placed in the library, which stay, and what happens to the import and the title. For a torrent that its client deletes, it SHALL say that the client deletes the torrent's own files and that a file in the place that is not the torrent's stays. For an import with no open grab, it SHALL say that no grab is open to close. The confirmation and the deletion SHALL be an administrator's, and the post SHALL be checked for the form's token. The post SHALL look at the download and ask its client again before it writes anything: when what it finds differs from what the page showed, or the client cannot be asked, the post SHALL change nothing and show the page again.

Once confirmed, the import SHALL be closed as dismissed with the reason that its download was deleted and with the person's name, and the action SHALL be recorded with the user. Files that an interrupted run left under a temporary name SHALL be put back first, as for a dismissal; when they cannot be, the deletion SHALL be refused and the import SHALL stay as it is. While a job is placing the import's files, the deletion SHALL be refused, as a dismissal is. The grab that brought the download SHALL be closed, including a series' grab that is held for a person because the series' numbering changed since the grab, and the title's history SHALL say who deleted the download. The title SHALL be wanted again under the ordinary rules, and the deletion SHALL queue no search. The release SHALL be put on the title's blocklist, as blocked by that person, only when the person ticked the box for it. The box SHALL be off by default and shown only when the download came from a grab.

After the import is closed, the client SHALL be asked to remove the download: with its data only under the rule of the next requirement, and without its data otherwise. What the client keeps (a Usenet job's completed folder) and what a client that no longer has the download left SHALL be deleted by the daemon's own removal of a download's leftover, under that removal's rules, at once and not at the next hourly cleanup. A request that could not be finished because the client did not answer, or the volume could not be written in that moment, SHALL be finished by the hourly cleanup, and the import's page SHALL say what is still to be done. A request that a rule refuses SHALL end there, with the rule on the import's page, and SHALL NOT be tried again. The page SHALL say what was deleted, or which rule left it and where it is.

An import that a person dismisses SHALL keep leaving its download where it is.

#### Scenario: A Usenet download
- **WHEN** an administrator confirms Delete this download for a blocked import of a Usenet download that completed in its own folder inside the folder of Axis's category
- **THEN** the import is dismissed with their name, the client has removed the job, the folder is gone from the disk before the page answers, the grab is closed, the movie is wanted again, and no search was queued

#### Scenario: A torrent its client still has
- **WHEN** the download is a torrent that its client still lists, at the place recorded when it completed
- **THEN** the client is asked to remove it with its data, Axis deletes nothing itself, the confirmation had said that the client deletes the torrent's own files (and had not promised everything in the folder), and the page says the client deleted its files

#### Scenario: A torrent its client no longer has
- **WHEN** the client says it no longer has the torrent, and it keeps Axis's torrents in a folder of their own
- **THEN** Axis deletes the torrent's folder under the rules of a leftover, whatever the setting for torrents after an import says, and the page says so

#### Scenario: The Downloads page links to it
- **WHEN** an administrator opens the Downloads page while a completed download's import is blocked
- **THEN** the download's row links to that import's Delete this download confirmation, and a user who is not an administrator sees no such link

#### Scenario: The confirmation writes nothing
- **WHEN** an administrator opens the confirmation and leaves it
- **THEN** the import, the download, the grab, the client and the disk are as they were

#### Scenario: The page is out of date
- **WHEN** the download's place, its state or the import's state changed after the confirmation was shown
- **THEN** the post changes nothing and the confirmation is shown again with what is so now

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks for the confirmation or the deletion
- **THEN** it is refused and nothing changes

#### Scenario: The import is being placed
- **WHEN** a job is placing the import's files
- **THEN** the deletion is refused, as a dismissal is

#### Scenario: A file under a temporary name
- **WHEN** an interrupted run left a file of the download under a temporary name in the library
- **THEN** it is put back first, as for a dismissal, and when it cannot be, nothing is deleted and the import stays as it is

#### Scenario: The client does not answer
- **WHEN** the client stops answering after the post has looked at it and before the client is asked to remove the download
- **THEN** the import is closed, its page says the removal is still to be made, the hourly cleanup makes it, and nothing was deleted on a guess

#### Scenario: The client is away when the person confirms
- **WHEN** the client does not answer at the moment the person confirms
- **THEN** nothing is written, the import stays as it was, and the confirmation is shown again saying that the client could not be asked

#### Scenario: The blocklist tick
- **WHEN** the person ticks the box that blocks the release
- **THEN** the release is on the title's blocklist as blocked by that person; without the tick it is not on it

#### Scenario: Recorded with the person's name
- **WHEN** a download was deleted
- **THEN** the import's actions and the title's history each say who deleted it and when

#### Scenario: A partly imported pack
- **WHEN** a season pack is partly imported and its download is deleted
- **THEN** the episodes placed keep their files and their links, and the other episodes the grab held are wanted again

#### Scenario: An ordinary dismissal
- **WHEN** a person dismisses an import
- **THEN** its download is still at its client and on the disk after the hourly cleanup has run

### Requirement: Deleting a download never touches a library folder or anything that is not the download's own
The deletion SHALL be refused, on the confirmation and again when confirmed, with the rule said in words, where the files are, and no button, in each of these cases:

- the place is not the one the client reported when the download completed, mapped through that client's path mapping as it is now onto the place recorded then, or the reported path has a `.` or `..` part;
- the place is not on a known volume, or is the volume's top or a folder of Axis's own;
- the client keeps Axis's category or label in a folder of its own, and the place is that folder itself or is not strictly inside it;
- Axis would delete the files itself, and the client keeps Axis's category or label in no folder of its own;
- the place is inside a library folder, is a library folder, or holds one;
- another download Axis knows that is still at its client, or another open import, is in the place or around it;
- what is at the place is not what was recorded there when the download completed, or, when Axis would delete the files itself, nothing was recorded there;
- the way to the place passes a symbolic link or another filesystem, or the place is a symbolic link;
- the volume is offline or mounted read-only, or storage is read-only;
- Axis recorded the download as gone from its client, and the client lists it now.

These rules SHALL hold when the client does the deleting too. A client SHALL be asked to remove a download with its data only when, at that moment, the client lists the download, something is at the place recorded when the download completed, that place passed every rule above, and, for a torrent, the client says the torrent is at that recorded place, in a folder or file named as the torrent is. This one rule SHALL be enforced where the client is asked, for the first post, the hourly retry and any later caller, and not by the order of the checks. A torrent that its client still lists and that is not at the recorded place (nothing is there, or the client names another place) SHALL be refused, on the confirmation and in the step: the client deletes a torrent wherever it is, so Axis asks it nothing. A download that its client does not list SHALL never be asked for with its data. A symbolic link inside the folder SHALL be removed as a link and never followed. No file outside the place SHALL be deleted. The last look is made just before the client is asked; a torrent moved in its client in the few milliseconds between that look and the removal is not seen, and its client deletes it where it is.

Only one step SHALL run for an import at a time: when the post's step and the hourly cleanup's come for the same import together, the second SHALL pass it over.

A file of the download that has another name in a library folder (a hard link) SHALL NOT stop the deletion: the download's name is removed, and the library's name and its content SHALL stay. The confirmation SHALL say how many of the files are also in the library.

#### Scenario: Inside a library folder
- **WHEN** the download's place is inside a library folder
- **THEN** the confirmation says so, shows no button, and a post deletes nothing

#### Scenario: A library folder, or one that holds a library folder
- **WHEN** the download's place is a library folder itself, or a folder with a library folder inside it
- **THEN** nothing is deleted and the page says which library folder

#### Scenario: Outside the client's own location
- **WHEN** the place is not inside the folder the client keeps Axis's downloads in, or is that folder itself
- **THEN** nothing is deleted and the page says where the files are

#### Scenario: Not on a known volume
- **WHEN** the client's path no longer maps onto a known volume, or maps onto another place than the one recorded
- **THEN** nothing is deleted and the page says why

#### Scenario: Something else is there now
- **WHEN** another folder or file has the download's name at its place
- **THEN** nothing is deleted

#### Scenario: Nothing recorded of it
- **WHEN** nothing was recorded of what was at the place when the download completed
- **THEN** Axis deletes nothing itself and the page says where the files may be

#### Scenario: Another download shares the place
- **WHEN** another download that is still at its client is in the place or around it
- **THEN** nothing is deleted

#### Scenario: A symbolic link on the way
- **WHEN** a part of the path to the place is a symbolic link
- **THEN** nothing is deleted and the page says Axis never follows one

#### Scenario: A symbolic link inside the folder
- **WHEN** the download's folder holds a symbolic link to a file outside it
- **THEN** the link goes with the folder and the file it points to is untouched

#### Scenario: A file that is also in the library
- **WHEN** a file of the download was hard-linked into a library folder by a partial import
- **THEN** the download's name is deleted, the library's file has the same inode and content as before, and the confirmation had said that the file stays in the library

#### Scenario: The disk is offline
- **WHEN** the download's volume is offline
- **THEN** the confirmation says so and shows no button, the client is not asked, and nothing is deleted

#### Scenario: The disk is mounted read-only
- **WHEN** the download's volume is mounted read-only
- **THEN** the confirmation says so and shows no button, and nothing is deleted

#### Scenario: Storage is read-only
- **WHEN** storage is read-only
- **THEN** the confirmation says so and shows no button, and nothing is deleted

#### Scenario: A torrent pointed into a library folder
- **WHEN** a torrent's client reports its files inside a library folder
- **THEN** the client is not asked to remove it with its data, and the page says why

#### Scenario: A torrent its client moved
- **WHEN** a torrent's client says the torrent's files are at another place now than the one recorded when it completed
- **THEN** the client is not asked to remove it with its data, and the page says both places

#### Scenario: A torrent moved into a library folder in its client
- **WHEN** a torrent that its client still lists was moved, files and all, into a library folder after it completed, so that nothing is at the recorded place
- **THEN** the confirmation says the torrent is no longer where Axis recorded it, that it was probably moved in the client and that Axis will not delete it, tells the person to remove it in the client or dismiss the import, and shows no button; a post changes nothing; the client is not asked to remove anything; and the file in the library is still there

#### Scenario: A torrent moved while its removal waited
- **WHEN** the request was recorded while the client did not answer, and the torrent was moved in the client before the hourly cleanup came
- **THEN** the cleanup asks the client nothing, the import's page says the torrent is no longer where Axis recorded it and that nothing was deleted, the file in the library is still there, and the cleanup does not try again

#### Scenario: Nothing at the place, and the client no longer lists it
- **WHEN** nothing is at the recorded place and the client does not list the download
- **THEN** the confirmation says nothing is left on the disk to delete, the import may be closed, and the client is not asked to remove anything with its data

#### Scenario: A download its client does not list
- **WHEN** the client does not list the download at the look made before it is asked
- **THEN** it is not asked to remove the download with its data, whatever state Axis recorded for the download

#### Scenario: Two steps for the same import
- **WHEN** the hourly cleanup comes for an import whose step the post is running
- **THEN** the cleanup passes that import over, the client is asked once, and the next cleanup finds nothing more to do

#### Scenario: A torrent reported at the client's download folder itself
- **WHEN** a torrent's client reports its files at a place that is not named as the torrent is (a torrent with no folder of its own)
- **THEN** the client is not asked to remove it with its data, and the page says that it can be removed at the client

#### Scenario: A download of one file
- **WHEN** the download is one file with no folder of its own, straight in the client's folder for Axis
- **THEN** only that file is deleted, and the folder and everything else in it stay

#### Scenario: A download recorded as gone that its client lists
- **WHEN** Axis recorded the download as gone from its client and the client lists it now
- **THEN** nothing is deleted, and the page says that Axis deletes nothing under a download its client has

