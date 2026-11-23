# storage-volumes Specification

## Purpose
How Axis knows a disk is the disk it thinks it is: volumes identified by a marker file, their states, the guard every file operation must hold, library roots, and the rule that an absent disk never looks like an empty one.
## Requirements
### Requirement: Volume identity
A volume SHALL be identified by a unique marker stored on the filesystem itself, so that it is recognised regardless of where it is mounted and cannot be confused with another filesystem at the same path. A directory whose filesystem is mounted read-only and has no marker SHALL be added without writing anything, with a new identifier for its marker later, and SHALL be recognised until then by its filesystem identity (below), never by its path. One filesystem SHALL be one volume: a directory on the filesystem of a registered volume SHALL be refused while that volume is online, and also while it is not when what is recorded of that volume proves the filesystem the same (an identifier the filesystem reports of its own, not a device number, that is the one recorded; or, where the identifier is only the device's number, the filesystem's UUID recorded); a device number alone, which another disk can have after a reboot, SHALL only warn while that volume is not online, and the refusal SHALL say how to proceed (point that volume here, or where its disk is, or remove it). What is recorded of a volume's filesystem (its identifier, device and UUID, for every volume, its marker or not; the marker stays what a volume with one is recognised by) SHALL be what was found when the volume was added, and then at every check that finds it and every re-pointing, so that a filesystem a volume no longer lives on stops counting once it is found elsewhere; what a volume added without a marker was first recognised by SHALL NOT count once its marker is written.

#### Scenario: Adding a volume writes its marker
- **WHEN** an administrator adds a directory on a filesystem mounted read-write as a volume and confirms
- **THEN** a marker file with a new unique identifier is written at the top of that directory and the volume is recorded as online

#### Scenario: Existing marker is adopted
- **WHEN** a directory that already holds a valid marker not known to this database is added
- **THEN** the existing identifier is used and no new marker is written

#### Scenario: A read-only disk without a marker is added
- **WHEN** an administrator adds a directory whose filesystem is mounted read-only, holds files and no marker, and reports an identifier of its own, and confirms
- **THEN** the volume is recorded as read-only and recognised by its filesystem identity, and nothing on the disk is written or changed

#### Scenario: Same filesystem twice
- **WHEN** a directory on a filesystem already registered as a volume, or nested inside another volume, is added
- **THEN** it is refused with an explanation

#### Scenario: Same filesystem twice, the first volume away
- **WHEN** a volume's disk is away from its mount point and a folder of its filesystem, mounted at another path, is added, the filesystem reporting an identifier of its own
- **THEN** it is refused, naming the volume and saying it is not online, and posting the confirmation anyway adds nothing and writes nothing

#### Scenario: Only the device number is the same
- **WHEN** a directory is added on the device a volume that is not online was last seen on, and nothing else of that volume's filesystem is the same
- **THEN** it is added with a warning naming that volume

#### Scenario: An xfs volume with a marker, its disk renumbered
- **WHEN** a volume with a marker on a filesystem whose identifier is its device's number is away, and a folder of its filesystem is added from another mount point at another device number
- **THEN** it is refused, the UUID the volume's last check recorded proving the filesystem the same

#### Scenario: The volume lives elsewhere now
- **WHEN** a volume added without a marker got its marker, was moved to another disk with it and pointed there, and its first disk is added
- **THEN** the first disk is not refused as that volume's filesystem

### Requirement: Presence detection
The engine SHALL treat a volume as online only while its marker is present and matches, or, for a volume that has no marker yet, while the directory at its path is not empty and its filesystem identity is the one recorded, and SHALL otherwise report it as offline or mismatched.

#### Scenario: Unmounted disk is offline, not empty
- **WHEN** a volume's filesystem is unmounted, leaving an empty directory at its path
- **THEN** the volume is reported offline

#### Scenario: Different disk at the same path
- **WHEN** another filesystem carrying a different marker is mounted at a volume's path
- **THEN** the volume is reported as mismatched and is not used

#### Scenario: Volume returns
- **WHEN** an offline volume's filesystem is mounted again
- **THEN** the volume is reported online at the next check without any user action

#### Scenario: Hung filesystem
- **WHEN** a volume's filesystem stops responding to file operations
- **THEN** the check times out, the volume is reported offline, and the daemon stays responsive

### Requirement: Nothing is forgotten while a volume is absent
The engine SHALL NOT delete, or mark as missing, any record of a volume's roots or files while that volume is not online.

#### Scenario: Records survive an outage
- **WHEN** a volume with recorded roots goes offline and later returns
- **THEN** every record that existed before the outage still exists, unchanged

### Requirement: Volume guard
Code that reads or writes files on a volume SHALL first obtain a guard that verifies the volume is online at that moment, and write guards SHALL be refused on read-only volumes and while storage is in read-only mode. A write guard SHALL need the volume's marker: on a volume that has none, it SHALL write the marker first and be refused when that fails.

#### Scenario: Guard refused when offline
- **WHEN** a guard is requested for an offline or mismatched volume
- **THEN** it is refused with an error that identifies the volume as unavailable, distinct from a file-not-found error

#### Scenario: Guard fails mid-operation
- **WHEN** a volume goes offline while a guard is held and the guard is re-checked
- **THEN** the re-check fails with the volume-unavailable error

#### Scenario: Write refused in read-only mode
- **WHEN** storage is in read-only mode and a write guard is requested
- **THEN** it is refused

#### Scenario: Read-only mount
- **WHEN** a volume's filesystem is mounted read-only
- **THEN** read guards are granted and write guards are refused

### Requirement: Library roots
An administrator SHALL be able to define library roots, each a directory on a volume with a media type, and roots on the same volume SHALL NOT contain one another.

#### Scenario: Add a root
- **WHEN** an existing directory on an online volume is added as a root with a media type
- **THEN** the root is recorded relative to its volume

#### Scenario: Nested root refused
- **WHEN** a root is added inside, or containing, an existing root on the same volume
- **THEN** it is refused

#### Scenario: Path escaping the volume refused
- **WHEN** a root path resolves, through `..` segments or symbolic links, to a location outside its volume
- **THEN** it is refused

### Requirement: Removing never deletes media
Removing a volume or a root from Axis SHALL remove only Axis's records and SHALL NOT delete or modify any media file.

#### Scenario: Remove a root
- **WHEN** an administrator removes a root
- **THEN** its records are removed and every file under its directory is untouched

### Requirement: Free space
The engine SHALL record each online volume's total and free space at every presence check.

#### Scenario: Free space is shown
- **WHEN** the volumes page is opened
- **THEN** each online volume shows its total and free space as of its last check, and each offline volume shows when it was last seen

### Requirement: Work area
Each volume SHALL have a work area at its top, `.axis-work`, made by the daemon through a write guard without following links, which is the only place temporary and trial files of conversions are written. Its leftovers SHALL be shown with their sizes.

#### Scenario: Created on first use
- **WHEN** a conversion first needs a volume's work area
- **THEN** it is created at the volume's top with access for its owner only

#### Scenario: A link in its place
- **WHEN** the work area's name exists as a symbolic link or a non-directory
- **THEN** it is not used and conversion on that volume is refused

### Requirement: Conversion's writes go through the volume write layer
Every change the daemon makes to a file for a conversion SHALL be made by the volume write layer through a write guard: settling the output's mode, owner and time in the work area, moving the output from the work area into the source's directory under a temporary name without replacing anything, renaming a trial within the work area, removing a file of the work area by its Axis name and inode, and the existing exchange, publish, retire and recycled-file removal. A library file SHALL never be removed except by retiring it into the recycle folder of its own volume.

#### Scenario: Nothing while read-only
- **WHEN** storage is read-only, or the volume is mounted read-only
- **THEN** every one of these operations refuses and nothing changes

#### Scenario: A file replaced underneath
- **WHEN** the file at a name is not the inode the caller expects
- **THEN** the operation refuses, or puts back what it moved

### Requirement: Every write proves the volume again
Every operation of the volume write layer, whoever calls it (the import pipeline and its cleanup, the TV series' per-file path, the conversions), SHALL prove the volume again before it changes anything: the volume's path still leads to the directory the guard holds, on the mount the guard holds (its mount id, where the kernel gives one), the marker there is the volume's, and it is mounted read-write, read through the path as well as through the guard. A guard whose volume fails this SHALL refuse every change from then on.

#### Scenario: Disk replaced after the guard was taken
- **WHEN** the volume's disk is moved away, unmounted lazily, or has another disk mounted over it after a write guard was taken, with another volume's marker or a copy of its own at the path
- **THEN** every write operation through that guard refuses, and neither the old disk's tree nor the new one changes

#### Scenario: The same filesystem mounted again read-only
- **WHEN** the volume's disk is unmounted lazily under a held write guard and the same filesystem is mounted at the path again, read-only
- **THEN** the volume's check says read-only, the guard's check fails, and nothing is written through it onto the detached mount

### Requirement: A download's leftover folder is removed by the volume write layer
The volume write layer SHALL be able to remove one folder with everything in it, for the import's removal of a Usenet download's leftover folder, through a write guard: it SHALL first look at the whole tree without changing anything and remove nothing when a folder in it is on another filesystem or mount or the tree is deeper or larger than its bounds; it SHALL remove only the folder's own names, deepest first, a symbolic link as a name and never followed, so that a file with another name elsewhere keeps that name and its data; it SHALL leave a folder whose inode is not the one the caller looked at, and something at the name that is not a folder; and it SHALL prove the volume again before it starts and refuse every change while storage is read-only or the volume is mounted read-only.

#### Scenario: A hardlinked film in the folder
- **WHEN** the folder holds a file whose other name is in the library
- **THEN** the folder's name of it is removed and the library's name and its data are unchanged

#### Scenario: A link out of the volume
- **WHEN** the folder holds a symbolic link to a file or folder outside the volume
- **THEN** the link is removed and what it points to is unchanged

#### Scenario: Another filesystem inside
- **WHEN** a folder inside it is another filesystem
- **THEN** nothing is removed

#### Scenario: Disk replaced after the guard was taken
- **WHEN** the volume's disk is replaced after the write guard was taken
- **THEN** the removal refuses and neither tree changes

### Requirement: A download's one file is removed by the volume write layer
The volume write layer SHALL be able to remove one file of a download by its name and inode, through a write guard, for a job the client put straight into its category's folder: only a regular file with the inode the caller looked at, never a symbolic link or another file at the name, and only its name, so that the library's name of it keeps the data. It SHALL prove the volume again first and refuse while storage is read-only or the volume is mounted read-only.

#### Scenario: Another file at the name
- **WHEN** the file at the name is not the inode the caller looked at, or is a symbolic link
- **THEN** it is left as it is

### Requirement: A download's file is moved by the volume write layer
The volume write layer SHALL be able to move a download's file into the library, through a write guard: one rename of the regular file with the inode the caller checked to a temporary name in a folder on the same filesystem, never replacing a name (where the filesystem has no rename that refuses to replace, a link that is checked and then the removal of the download's name), so that the file has one of its names at every moment; and the same back to its download name. A file renamed by mistake (its name replaced between the look and the rename) SHALL be put back by the same rename; a link of the wrong file SHALL be removed again, the wrong file keeping its own name. It SHALL refuse another inode, a symbolic link, a name that is taken, a name that is not one component or is a temporary name where a download's name is expected, another filesystem, read-only storage and a volume that does not prove itself, changing nothing; the file's data, mode, owner and times SHALL be unchanged.

#### Scenario: Moved
- **WHEN** a download's file is moved into the library's folder
- **THEN** it has the same inode and one name, its download name is gone, and its contents and time are unchanged

#### Scenario: A name taken
- **WHEN** the temporary name, or on the way back the download's name, is taken
- **THEN** nothing is moved and the other file is unchanged

#### Scenario: Another filesystem
- **WHEN** the destination folder is on another filesystem
- **THEN** it is refused and the download's file stays where it was

### Requirement: A download's file whose copy is in the library is removed by the volume write layer
After a move across volumes the volume write layer SHALL be able to remove the download's file through a write guard only while it is the regular file copied, untouched since: by its name, inode, size, modification time and change time as they were when it was read for the copy, checked on the file opened (without following a link, and only once it is seen to be a regular file), its name looked at again just before it goes; never a symbolic link, another file of its name or a temporary name, and only that name. What it was when it was read SHALL be taken by the copy on the descriptor it reads, before its first byte is read, and SHALL be the same, the device, inode, size and both times to the nanosecond, after its last byte is read, or the copy is refused (a file deleted meanwhile, its data unwritten and nothing at its name in its directory afterwards, is the one change of its change time that stands; one renamed over, its name leading to another file, is a change); that, and never a look at the file taken afterwards, SHALL be what its removal requires. It SHALL refuse without the inode and the times, while storage is read-only or the volume is mounted read-only, and prove the volume first.

#### Scenario: Another file at the name
- **WHEN** the file at the name has another inode or size, or is a symbolic link
- **THEN** it is left as it is

#### Scenario: Touched since
- **WHEN** the file at the name has the inode and size copied but another modification or change time (it was written, or its mode changed)
- **THEN** it is left as it is

#### Scenario: Changed while the copy was read back
- **WHEN** the download's file is written over in place after it was read and checked, while the copy is flushed and read back
- **THEN** the copy succeeds, the removal given what the copy read leaves the changed file, and the library holds what was read

#### Scenario: Changed while it is read
- **WHEN** the download's file's mode changes while it is read for the copy (its change time alone)
- **THEN** the copy is refused and nothing is left under its temporary name

#### Scenario: Renamed over while it is read
- **WHEN** another file is renamed over the download's file's name while it is read for the copy
- **THEN** the copy is refused, nothing is left under its temporary name, and the newer file stays

### Requirement: A moved file's temporary name is removed only beside a name of its own
The volume write layer SHALL remove the temporary name of a file moved out of a download only when another name the caller names (the file's final name, or the download's own), looked at just before, leads to the very file on the same filesystem; the file's link count SHALL NOT be taken for another name, since a hardlink outside the library and the download counts too.

#### Scenario: A hardlink outside
- **WHEN** the file has two links, its temporary name and one outside, and the name named leads to nothing or to another file
- **THEN** the temporary name is kept

#### Scenario: Beside its final name
- **WHEN** the final name leads to the file
- **THEN** the temporary name is removed and the final name stays

### Requirement: A publish says whether its folder reached the disk
The volume write layer SHALL say, of a publish and of a swap, whether the directory's fsync after the file got its name succeeded, and of a folder it makes, whether its parent's fsync did; the file or folder keeps its name either way.

#### Scenario: The fsync fails
- **WHEN** the directory cannot be fsynced after a publish
- **THEN** the publish succeeds and says the folder was not flushed

### Requirement: A volume without a marker is recognised by its filesystem identity
Until its marker is written, a volume added from a read-only disk SHALL be recognised by its filesystem's type, its identifier (statfs `f_fsid`), its UUID when the kernel gave one, and its directory's inode, as recorded when it was added, and only while its directory is not empty and (once it was one) a mount point. Where a UUID was recorded and the identifier is the device's number (xfs, f2fs), the UUID SHALL decide and the identifier SHALL NOT be compared. The filesystem's size SHALL count only where neither a UUID nor an identifier of the filesystem's own exists; where one of those matched, another size (a pool written, a filesystem grown) SHALL be noted in the log, never taken for another filesystem. Anything else at its path SHALL make it offline, never taken for the volume, and its marker SHALL never be written there. A filesystem that cannot be told from another without a marker (a network, FUSE or layered filesystem, one that reports only its device number and no UUID, an empty directory) SHALL be refused when adding, with the way out named. The confirmation of adding SHALL compare the filesystem's size only where its identifier is the device's number.

#### Scenario: Recognised after a restart and a remount
- **WHEN** the daemon restarts, or the disk's mount goes and comes back, or the disk is mounted at another path and the volume pointed there
- **THEN** the volume is recognised again by its filesystem identity

#### Scenario: Another filesystem at the same path
- **WHEN** another filesystem, even with the same files, or another directory of the same filesystem is at the volume's path
- **THEN** the volume is offline, read and write guards are refused, and nothing is written there, even when it is mounted read-write

#### Scenario: An empty directory never matches
- **WHEN** the volume's directory is empty, whether or not it is a mount point, or an empty disk mounted read-only is added
- **THEN** the volume is offline, or the adding is refused

#### Scenario: Refused when the filesystem cannot be told apart
- **WHEN** a read-only directory without a marker is on a network, FUSE or layered filesystem, or on one that reports only its device number and the kernel gives no UUID
- **THEN** adding it is refused, saying to mount it read-write once so that the marker can be written

#### Scenario: Disks found in another order
- **WHEN** an xfs disk added without a marker, whose kernel gave a UUID, is found after a reboot at another device number, its identifier with it
- **THEN** it is recognised by its UUID; another UUID, or none given now, is another filesystem

#### Scenario: The size moves
- **WHEN** a filesystem recognised by an identifier of its own or its UUID reports another size than when it was added
- **THEN** it is still the volume, and the log says so once

### Requirement: The marker is written as soon as it can be
The marker of a volume added without one SHALL be written, holding the volume's identifier, the first time a check finds the volume recognised by its filesystem identity and its filesystem mounted read-write, through the directory the check looked at, also while storage is in read-only mode (it is the write confirmed when adding). From then on the volume SHALL be recognised by its marker only. A marker that cannot be written SHALL be recorded, shown and tried again at later checks.

#### Scenario: Mounted read-write
- **WHEN** a volume without a marker is found recognised and mounted read-write at a check
- **THEN** its marker is written, nothing else is written, and the volume is recognised by its marker from then on

#### Scenario: The marker takes over
- **WHEN** the marker of such a volume has been written and is then deleted
- **THEN** the volume is a mismatch, as any volume whose marker is missing, and is not recognised by its filesystem again

#### Scenario: The marker cannot be written
- **WHEN** a recognised volume is mounted read-write but its marker cannot be written
- **THEN** the volume stays recognised for reading, the reason is shown on its page, and it is logged once

### Requirement: Nothing is written to a volume before its marker
No write SHALL be made to a volume that has no marker, other than its marker. A write guard on such a volume SHALL write the marker first, through the directory the guard then holds, and SHALL be refused when it cannot; every check of a write guard SHALL demand the marker.

#### Scenario: A write guard first
- **WHEN** a write guard is asked for on a recognised volume without a marker, mounted read-write, with storage writable
- **THEN** the marker is written before the guard is given

#### Scenario: The marker cannot be written
- **WHEN** the marker of such a volume cannot be written
- **THEN** the write guard is refused as unavailable and nothing on the disk changes

### Requirement: The volume's page says what it is recognised by
The volume's page SHALL say whether the volume is recognised by its marker or, having none yet, by its filesystem identifier, what that means (offline on any difference, nothing written, no worker), and when the marker is written; the volume list SHALL mark a volume without a marker.

#### Scenario: A volume without a marker
- **WHEN** the page of a volume added from a read-only disk is opened
- **THEN** it says the volume is recognised by its filesystem identifier and has no marker yet, and that the marker is written the first time the disk is found mounted read-write

### Requirement: A marker is restored only onto the volume's filesystem
Restoring a deleted marker SHALL compare the filesystem at the volume's path with the one the volume was last seen on and say which it is: the same (the same type and an identifier of its own that is the one recorded, or, where the identifier is the device's number, the UUID recorded), one Axis cannot tell apart (the same type, size and identifier, the identifier being the device's number, no UUID to compare), another (its type, identifier, UUID, or for an identifier that is the device's number without a UUID its size, differ), or unknown (nothing recorded). Folders of the roots' names SHALL NOT be taken alone for the volume's disk. The marker SHALL be written without a further confirmation only onto the same filesystem; otherwise only when the administrator confirms that it is the volume's disk. It SHALL never be written into a directory of another volume's filesystem (as adding refuses it), whether or not that volume is online.

#### Scenario: Another disk with the same folders
- **WHEN** the volume's marker is gone and another disk with the same files is at its path
- **THEN** the page says the filesystem is not the one the volume was last seen on, the marker is not written unless that is confirmed, and once confirmed it is written and the volume is online

#### Scenario: Its own disk
- **WHEN** the volume's marker is gone from its own disk
- **THEN** the page says the filesystem is the one the volume was last seen on

#### Scenario: Another volume's filesystem at its path
- **WHEN** a folder of another volume's filesystem, with files and no marker, is at the volume's path
- **THEN** restoring is refused naming the other volume, confirmed or not, the other online or not, and nothing is written

### Requirement: Two swapped disks are re-pointed together
Re-pointing a volume onto exactly another volume's path SHALL be allowed when the directory there proves this volume and the other volume is proven at this one's path; both volumes SHALL be taken as the database has them while the change holds the storage lock, neither new path SHALL hold or be inside a third volume, and both SHALL be re-pointed in one statement, so that no moment has two volumes at one path. Otherwise it SHALL be refused, naming the other volume (or the third), and nothing SHALL change.

#### Scenario: Swapped
- **WHEN** two marked disks are swapped between their mount points and one volume is pointed at the other's path
- **THEN** both volumes are re-pointed and online

#### Scenario: The other disk is not here
- **WHEN** the other volume's disk is not at this volume's path
- **THEN** the re-pointing is refused naming the other volume, and both paths are as they were

#### Scenario: Re-pointed meanwhile elsewhere
- **WHEN** another daemon on the database re-pointed this volume meanwhile, so that its recorded path is not the one this daemon remembers
- **THEN** the swap is decided on the recorded path, and refused when the other volume is not proven there

#### Scenario: A third volume inside
- **WHEN** a third volume is recorded inside the other volume's path
- **THEN** the swap is refused naming the third volume, and nothing changes

### Requirement: A marker write's leftovers are removed
A write guard SHALL remove from the volume's top what an interrupted marker write left: names of exactly the form the marker write makes, regular files no bigger than a marker, found the very same file (inode, size, modification time) by two looks at least ten minutes apart by the monotonic clock, never by comparing a file's time with a clock, never a name this process's own marker writes made, never a symbolic link and never another name; at most one look in ten minutes per volume. A read guard SHALL remove nothing.

#### Scenario: Left by a crash
- **WHEN** a temporary marker file an hour old by its time, one written to between two looks, a name of another form and a symbolic link of that form are at the volume's top, and write guards look twice
- **THEN** nothing is removed at the first look, and at the second only the one found the same, which is logged

#### Scenario: Clocks that disagree
- **WHEN** a temporary marker file has a time a year back or an hour ahead of the clock
- **THEN** it is removed at a look ten minutes after one that found it the same, never at the first

#### Scenario: This process's own write
- **WHEN** a temporary name this process's marker write made is at the volume's top
- **THEN** it is never removed

### Requirement: Checks write no access time
Reading the marker and looking whether a volume's top is empty, at every check, SHALL NOT update access times where the kernel allows that, and SHALL work the same where it does not.

#### Scenario: Read at a check
- **WHEN** a marker and its directory with access times older than their last change are read by a check
- **THEN** their access times are unchanged

### Requirement: A library root may hold YouTube videos
A library root SHALL be able to have the media type YouTube, beside the existing types. It is where the YouTube downloader saves, and nothing else is saved there by Axis. Inside it Axis keeps one folder of its own, `.axis-youtube`, where yt-dlp works; Axis's scans SHALL ignore it.

#### Scenario: Adding the root
- **WHEN** an administrator adds a library folder and chooses the kind YouTube
- **THEN** the root is stored with that type, and the YouTube page offers it as the place a video goes

