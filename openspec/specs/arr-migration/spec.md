# arr-migration Specification

## Purpose
Define how an administrator migrates from Radarr and Sonarr: Axis reads their HTTP APIs with an address and a key and changes nothing there, shows a preview before anything changes, and in one confirmed run adds the movies and series with their monitoring and links their files by path without touching them. Nothing is searched for until a person starts it.
## Requirements
### Requirement: Axis only reads from Radarr and Sonarr
Axis SHALL read a Radarr or a Sonarr through its HTTP API (version 3) with GET requests only, through a connection that cannot send any other kind of request. Axis SHALL NOT change, unmonitor or remove anything in Radarr or Sonarr, and SHALL NOT read their database files.

#### Scenario: Only GET
- **WHEN** a reading, a test of the connection and a check of the download queue have run against Radarr
- **THEN** every request Radarr received was a GET

#### Scenario: A request of another kind
- **WHEN** the connection is asked to send a request that is not a GET
- **THEN** it fails and nothing is sent

### Requirement: A connection is an address and a key, and the key is a secret
A connection SHALL hold its app (Radarr or Sonarr), a name, an address with an optional URL base, whether a self-signed certificate is accepted, and an API key. A connection SHALL keep the app it was made for. The key SHALL be stored apart from the connection's other fields, SHALL be sent only as the header `X-Api-Key` to the connection's own address and never in a URL, and SHALL NOT appear in any page, notice, job, history line, error, log line or URL after it was typed. The form SHALL offer an empty field to replace the key and SHALL NOT show it. Changing a connection's address SHALL need the key typed again. Forgetting a connection SHALL delete its key and its plans and SHALL leave what was migrated.

#### Scenario: Never shown again
- **WHEN** an administrator opens the form of a connection whose key is stored
- **THEN** the page holds no part of the key, and says a key is stored

#### Scenario: In no log line
- **WHEN** a reading fails because Radarr refuses the key
- **THEN** neither the job's error, nor the page, nor the log holds the key

#### Scenario: A new address
- **WHEN** an administrator changes a connection's address and leaves the key field empty
- **THEN** the change is refused and the stored key is not sent to the new address

#### Scenario: Forgotten
- **WHEN** an administrator forgets a connection that migrated 40 movies
- **THEN** the key and the plans are gone, and the 40 movies, their links, their marks and their holds are as they were

### Requirement: Only administrators migrate, from an address they state
Every page and action of migrating SHALL be an administrator's, and every post SHALL carry the CSRF token. The address SHALL be an http or https origin with an optional path prefix, without user information or a query; private, loopback and link-local addresses SHALL be allowed. A redirect to another origin SHALL NOT be followed. Before anything else is asked of an address, its answer to `system/status` SHALL have named the app the connection is for. Nothing of an answer SHALL be shown except the fields Axis reads from an answer of that app, and an error SHALL be stated in fixed words.

#### Scenario: A viewer
- **WHEN** a user who is not an administrator opens the page or posts to it
- **THEN** the request is refused and nothing is read or changed

#### Scenario: An address on a private network
- **WHEN** an administrator gives an address in a private range, or a container's name
- **THEN** the connection is accepted and can be tested

#### Scenario: Not Radarr
- **WHEN** the address of a Radarr connection answers, and the answer is not Radarr's
- **THEN** the test says the address does not answer as Radarr, shows nothing of the answer, and no further request is made

#### Scenario: A redirect elsewhere
- **WHEN** the address answers with a redirect to another host
- **THEN** no request is made to that host and the key is not sent there

### Requirement: A preview shows everything before anything changes
Before a run, Axis SHALL read what the app has into a plan and show a preview of it. The reading and the preview SHALL NOT add a movie or a series, link a file, add a blocklist entry or start a search. Viewing the preview SHALL write nothing, and saving its choices SHALL write them to the connection and nothing else. A plan SHALL be usable only when its reading completed, and for one day. The preview SHALL show how many titles the app has, how many are monitored, and how many a run would migrate, monitored and unmonitored; how many files it would link; the mapping tables; and every title that would be skipped with its reason: an incomplete answer, listed twice by the app, no identifier, no match, not looked up yet, already in Axis, removed in Axis after it was migrated, not monitored, quality profile not mapped, folder not mapped, or disk offline. It SHALL list every file of the app's that would not be linked, with the app's path, the mapped place and the reason: not in Axis's index at that path, another size than the app recorded, linked in Axis to another title, a path that cannot be mapped, or no path or size given by the app. It SHALL say how many of the titles are in Axis already and that a run leaves them as they are.

#### Scenario: Nothing changed
- **WHEN** an administrator reads from Radarr, opens the preview, changes the mappings and leaves
- **THEN** no movie, link, blocklist entry or search was made

#### Scenario: Skipped, with reasons
- **WHEN** Radarr holds a movie that Axis manages already, a movie whose identifier TMDb does not know, and a movie in a folder that is not mapped
- **THEN** the preview lists the three as "Already in Axis", "No match" and "Folder not mapped"

#### Scenario: Disk offline
- **WHEN** a folder is mapped to a library folder on a disk that is offline
- **THEN** its titles are listed as skipped because the disk is offline, and a later run migrates them

### Requirement: The preview maps profiles and folders, and arrives filled in
The preview SHALL ask, for each quality profile of the app that a title to migrate uses, which of Axis's quality profiles it becomes, and for each folder of the app that holds titles, which of Axis's library folders of that media type, and which path below it, it is. A row with no saved choice SHALL show a suggestion, marked as suggested with its evidence, that counts as a saved choice does: for a profile, Axis's profile of the same name, whatever the case, else the default profile for new titles of the administrator who started the reading, else Axis's default profile; for a folder, the one place (a library folder and a path below it) where Axis's index holds most of a sample of the app's files under that folder at the same relative paths, or nothing, with the reason. A suggestion SHALL be worked out from Axis's index only, and SHALL never be stored. A choice an administrator saves, "(not mapped)" and "(none: skip ...)" included, SHALL never be replaced by a suggestion. The folder mapping SHALL also be the mapping of paths between the app and Axis: a path of the app below a mapped folder is the same path below the library folder, the longest mapped prefix by whole segments winning, and a relative path or one with `.` or `..` SHALL be refused. A title whose profile or folder is not mapped SHALL be skipped. For each folder row the preview SHALL show how many of the app's files are found at the mapped paths.

#### Scenario: Another mount
- **WHEN** Radarr's root folder is `/movies`, mapped to a library folder Axis sees at `/media/disk1/Movies`, and Radarr lists `/movies/Paper Lanterns (2011)/Paper Lanterns (2011).mkv`
- **THEN** Axis looks for that file below the library folder at `Paper Lanterns (2011)/Paper Lanterns (2011).mkv`

#### Scenario: Suggested
- **WHEN** nothing was saved for a Radarr folder, and Axis's index holds that folder's files at the same relative paths below one library folder
- **THEN** the row shows that library folder, marked "Suggested", and a run can be confirmed without saving anything

#### Scenario: The wrong folder
- **WHEN** a root folder with 900 files is mapped to a library folder that holds none of them
- **THEN** its row says 0 of 900 files are found there

#### Scenario: Not mapped
- **WHEN** an administrator saves "(none: skip these movies)" for a quality profile of Radarr's
- **THEN** the movies that use it are skipped as "Quality profile not mapped", also after a later reading

### Requirement: One confirmed run migrates what the preview showed
A run SHALL start only from a confirmation that states what it will do, and SHALL be refused, with the preview shown again, when the plan, the choices in force (saved or suggested), the options or any count of the preview changed after the page was made. It SHALL run as a job with progress that can be cancelled on the Jobs page. It SHALL migrate each monitored movie with its mapped quality profile and library folder, its monitoring and its minimum availability, and each monitored series as the `tv` capability states. Titles that are not monitored in the app SHALL be migrated only while the option to migrate them is ticked, and SHALL stay unmonitored. Until an administrator saves a choice, the option SHALL be off for a Radarr and on for a Sonarr. Each title SHALL be checked again as it is migrated, and nothing the preview did not show SHALL be migrated. A run SHALL write to the database only, and SHALL work while storage is read-only.

#### Scenario: Monitored movies
- **WHEN** Radarr holds 30 monitored movies and 5 that are not, and the run is confirmed with the options as they are
- **THEN** 30 movies are managed by Axis, each with its mapped profile and library folder, and the 5 are not

#### Scenario: Unmonitored, when asked
- **WHEN** the option for unmonitored movies is ticked
- **THEN** the 5 are migrated too, and none of them is monitored

#### Scenario: A Sonarr's option
- **WHEN** an administrator opens the preview of a Sonarr for which no choice was saved
- **THEN** the option to migrate unmonitored series is ticked

#### Scenario: Changed since
- **WHEN** the mapping is changed in another window and the first window's confirmation is posted
- **THEN** nothing is migrated and the preview is shown again

#### Scenario: Read-only storage
- **WHEN** storage is read-only and the library folder's disk is mounted read-only
- **THEN** the run migrates the movies and links their files all the same

### Requirement: Files are linked by path and never touched
A file the app lists SHALL be linked to its movie or its episodes only when Axis's index holds a file at the mapped path with the size the app recorded. Axis SHALL NOT move, rename or delete any file or folder, and SHALL NOT open one. A file that is not in the index at its path, has another size, whose path cannot be mapped, or for which the app gives no path or size SHALL be reported with its reason and SHALL NOT be linked by its name, its size or any other guess. A file that an import or a person linked to the same title SHALL stay as it is. A file that only identification linked to the same title SHALL be marked as migrated and kept, as a file the run links is. A file linked to another title SHALL stay there and be reported with that title, and a file a person unlinked by hand SHALL stay unlinked and be reported.

#### Scenario: Found at its path
- **WHEN** the mapped path of a movie's file is in the index with the size Radarr recorded
- **THEN** the file is linked to the movie

#### Scenario: Not seen
- **WHEN** Radarr lists a file and the index holds nothing at the mapped path, though a file of the same name is in another folder
- **THEN** the file is reported as not seen at that path, and no file is linked

#### Scenario: Nothing touched
- **WHEN** a run links files
- **THEN** every file has the same path, name, size, inode and content as before

#### Scenario: Linked by identification first
- **WHEN** identification linked a movie's file to that movie before the run
- **THEN** the run marks the file as migrated from Radarr and keeps it

#### Scenario: Linked elsewhere
- **WHEN** the file at the mapped path is linked to another movie in Axis
- **THEN** that link stands and the file is reported with the other movie's name

### Requirement: A run starts no search by itself
A run SHALL NOT search for or grab anything. Every title it migrates SHALL be held from automatic searches until an administrator starts searching for the connection's titles or for that one title, or the run's own tick starts them at its end. The connection's page SHALL count the held titles that starting would have Axis search for (a monitored movie without a file, a series with a monitored episode without a file) apart from those that have their files. Starting SHALL show those counts, the pace of the scheduler in plain numbers and the titles left out, SHALL search nothing at once, and SHALL leave the titles to the scheduler at its ordinary pace. When nothing would be searched for, starting SHALL end the holds and search nothing. A title held because the app has a file that Axis does not see SHALL stay held when the others are started.

#### Scenario: A thousand missing movies
- **WHEN** a run confirmed without its tick migrates 1,000 monitored movies without files
- **THEN** no search is queued and no release is grabbed, by the run or by the scheduler's next runs

#### Scenario: Started by a person
- **WHEN** an administrator confirms "Start searching for N movies without a file"
- **THEN** those movies are searched by the scheduler at its ordinary number per run, and none is searched at once

### Requirement: Running beside Radarr or Sonarr is warned about
When searches are started for migrated titles and the connection is stored, Axis SHALL read the app's download queue again, SHALL leave out and keep held every title the app is downloading, and SHALL say, from the connection's reading of the last day, how many of the titles to start the app still monitors and that both would download them; with no reading that young it SHALL say that it does not know. When the app does not answer, or no key is stored, starting SHALL need the administrator's statement that the app is stopped or no longer monitors them. When the app answers and Axis cannot tell which titles its downloads are for, the page SHALL say so as a warning, and starting SHALL need no statement and hold nothing back.

#### Scenario: Radarr is downloading it
- **WHEN** searches are started and one of the movies is in Radarr's download queue
- **THEN** that movie stays held, is listed as left out, and the others are started

#### Scenario: Still monitored there
- **WHEN** Radarr still monitors 212 of the movies about to be started
- **THEN** the confirmation says so, with that number, before anything is started

#### Scenario: Radarr does not answer
- **WHEN** searches are started and Radarr does not answer
- **THEN** the page says that Axis could not check what Radarr is downloading, and nothing is started without the tick "Radarr is stopped, or no longer monitors these movies"

### Requirement: A second run migrates only what is new
Running again SHALL migrate the titles that are new in the app. For a title the connection migrated before, it SHALL link a file of the app's that has become visible in Axis's index, linked to nothing and not unlinked by hand, and SHALL mark as migrated and keep a file of it that only identification linked; a hold kept because the app had a file Axis did not see SHALL end once its files are found. It SHALL NOT make a second movie, series or link for anything in Axis. A title Axis manages already SHALL keep its monitoring, profile, library folder and other links as they are in Axis. A title the connection migrated and a person removed in Axis since SHALL be skipped as "Removed in Axis after it was migrated". A title the app no longer has SHALL change nothing in Axis.

#### Scenario: Twice
- **WHEN** a run is made twice with nothing changed in Radarr
- **THEN** the second run migrates nothing and Axis holds what it held after the first, with no second history line

#### Scenario: A disk that came back
- **WHEN** a movie's file was not seen by the first run and is in the index at its path at the second
- **THEN** the second run links it

#### Scenario: Changed in Axis since
- **WHEN** a migrated movie's quality profile was changed in Axis and Radarr still names the old one
- **THEN** a second run leaves the movie's profile as it is in Axis

#### Scenario: Removed in Axis
- **WHEN** a person removed a migrated movie in Axis and Radarr still has it
- **THEN** a second run skips it as "Removed in Axis after it was migrated"

### Requirement: A reading or a run that stops can go on
A reading that fails SHALL end with its reason in fixed words, SHALL keep no item of its plan, and SHALL leave a plan that cannot be confirmed; an earlier complete plan SHALL stay usable until it is a day old. A run SHALL migrate each title whole or not at all, SHALL record each title's outcome, and when it is cancelled or fails SHALL be continued by the queue's retry or by confirming again, which migrates the titles not migrated yet and none twice. Its report SHALL list what was migrated, what was skipped with its reason, each file that was not linked with its reason, and what is held.

#### Scenario: Radarr stops answering
- **WHEN** Radarr stops answering during a reading
- **THEN** the reading ends with that reason, and no run can be confirmed from it

#### Scenario: Interrupted
- **WHEN** a run fails with a database error after 20 of 60 movies were migrated, and the run is retried or confirmed again
- **THEN** the other 40 are migrated, and none of the 20 is migrated twice

### Requirement: Large libraries are read within stated limits
Axis SHALL read the list of every movie or every series up to 128 MB and every other answer up to 16 MB, and SHALL refuse a larger answer with that reason. It SHALL read the download queue 250 records a page, at most 400 pages. For a Sonarr it SHALL ask for each series' episodes and episode files one request at a time, at most 4 requests a second, within 32 MB a list, 1 GB for all the lists and 5,000 series. A whole reading SHALL take at most one hour, and SHALL ask the metadata provider for at most 2,000 titles, the rest being "Not looked up yet" until a later reading. Every list the pages show SHALL be paged. What an answer holds SHALL be treated as untrusted: parsed by a bounded parser, cut in length and checked in range, and a title whose answer is incomplete SHALL be skipped alone.

#### Scenario: Thousands of movies
- **WHEN** Radarr holds 8,000 movies
- **THEN** the reading completes, and the preview's lists are shown a page at a time

#### Scenario: Too large
- **WHEN** an answer is larger than the stated limit
- **THEN** the reading ends saying the list is larger than Axis reads, and nothing is migrated

#### Scenario: An incomplete title
- **WHEN** one movie of Radarr's answer has no title
- **THEN** that movie is listed as skipped for an incomplete answer and the others are read

### Requirement: The blocklist is not migrated
A run SHALL NOT read the app's blocklist and SHALL add no blocklist entry. No page SHALL offer to migrate it: a person blocks a release in Axis as needed.

#### Scenario: No option
- **WHEN** an administrator opens the preview
- **THEN** no blocklist option is offered, and a run adds no blocklist entry

### Requirement: The run can start the searches at its end
The run's confirmation SHALL offer a tick, not pre-ticked, "Start searching for these as soon as they are migrated". The tick SHALL be part of what the confirmation's digest covers and SHALL be stored with the job. With it, a run that has taken every title, whether or not it could migrate each, SHALL do at its end what "Start searching" does, for the titles of this run only: read the app's download queue, leave the titles in it held as downloading, start the others, and never start a title or an episode held because the app has a file Axis does not see. When the app does not answer, or answers without saying which titles it is downloading, the run SHALL start nothing. The connection's page SHALL say what was started, what was left out, and what the run could not migrate. A run that is cancelled or stops before it has taken every title SHALL start nothing, and without the tick everything migrated SHALL stay held.

#### Scenario: Ticked
- **WHEN** a run confirmed with the tick ends, and Radarr is downloading one of the movies it migrated
- **THEN** that movie is held as downloading, the movies whose files Axis does not see stay held, the others are started, and the connection's page says how many

#### Scenario: The queue cannot be read
- **WHEN** a run confirmed with the tick ends and Radarr does not answer
- **THEN** nothing is started, everything migrated is held, and the connection's page says so

#### Scenario: Only its own titles
- **WHEN** a run confirmed with the tick follows an earlier run that was not ticked
- **THEN** only the titles of the ticked run are started, and those of the earlier run stay held

#### Scenario: Not ticked
- **WHEN** a run is confirmed without the tick
- **THEN** everything migrated is held until "Start searching"

