# youtube Specification

## Purpose
Define the YouTube downloader: an administrator, after switching it on, pastes the address of one video, and Axis fetches it with yt-dlp, run as a contained subprocess, into a YouTube library folder without re-encoding it, without an account, and only under the outbound binding's rules.
## Requirements
### Requirement: Only an administrator downloads from YouTube, and only after switching it on
The YouTube downloader SHALL be off until an administrator switches it on from a page that shows that the terms of service and the law are theirs to answer for, and SHALL record who switched it on and when. While it is off, no video SHALL be looked up or fetched and no yt-dlp SHALL be run. Looking a video up, downloading it and the switch SHALL be for administrators only. Switching YouTube off SHALL stop a fetch that is running. Axis SHALL ship no channel, playlist or video.

#### Scenario: A fresh installation
- **WHEN** Axis starts for the first time
- **THEN** YouTube is switched off, no yt-dlp is run, and the YouTube page offers no address box until an administrator switches YouTube on

#### Scenario: Before the switch
- **WHEN** an administrator pastes a video's address while YouTube is switched off
- **THEN** the request is refused saying that YouTube is switched off, and yt-dlp is not run

#### Scenario: Someone who is not an administrator
- **WHEN** a user who is not an administrator posts an address to look up, a video to download, or the switch
- **THEN** the request is refused and nothing is looked up, queued or changed

#### Scenario: Switched off while a fetch runs
- **WHEN** an administrator switches YouTube off while a video is being fetched
- **THEN** yt-dlp is stopped, the folder it worked in is removed, the video and its download say that YouTube was switched off, and the page says that the running download was stopped

### Requirement: Only one video's https address at YouTube is taken
The downloader SHALL accept only an `https` address at `youtube.com` (also with `www.`, `m.` or `music.`) or `youtu.be` that names one video by an id of 11 letters, digits, `-` and `_`, in printable ASCII of at most 2048 bytes. It SHALL refuse every other scheme and host, a host with a user name or a port, and a playlist's or a channel's address, each with its own words. What was typed SHALL NOT be passed to yt-dlp: yt-dlp SHALL be given the address Axis writes from the video id, as its last argument after `--`.

#### Scenario: A video beside a list
- **WHEN** the address of a video carries a `list=` parameter
- **THEN** the one video is taken and the list is ignored

#### Scenario: A playlist or a channel
- **WHEN** a playlist's or a channel's address is pasted
- **THEN** it is refused, saying that this version downloads one video at a time

#### Scenario: Text that looks like an option
- **WHEN** what is pasted begins with a dash, holds a space or a line end inside it, or names another host
- **THEN** it is refused, and no part of it reaches yt-dlp's command

### Requirement: yt-dlp is run as a contained subprocess with a fixed set of options
yt-dlp SHALL be a separate program run through an argument vector and never a shell, with one fixed set of options, no configuration file, no cookies and no hook, a minimal environment, the lowest CPU and I/O priorities, a time limit, a limit on what it prints and on the size of a file it writes, and its whole process group killed when it is stopped. Its home and its settings folder SHALL be folders of Axis's own in the data directory, readable by the daemon's user alone, and yt-dlp SHALL NOT be run while either holds anything. A proxy SHALL reach yt-dlp through its environment and never through its command. At most one fetch and one look SHALL run at a time. yt-dlp SHALL be asked its version only on the administrator's page and in the built-in client's test, so that a page every signed-in user can open starts no program. A yt-dlp that fails, hangs, floods its output or dies SHALL end as a failed download with a plain reason and SHALL NOT stop the daemon.

#### Scenario: A failure
- **WHEN** yt-dlp exits with an error
- **THEN** the download is failed with yt-dlp's last error line, cleaned and cut, and nothing of it is left in the library folder

#### Scenario: A hang
- **WHEN** yt-dlp does not finish within the time limit
- **THEN** its process group is killed, the download is failed saying that it did not finish in time, and nothing of it is left

#### Scenario: A flood
- **WHEN** yt-dlp prints more than the limit
- **THEN** it is killed, the download is failed saying so, and nothing of it is left

#### Scenario: Something in its home
- **WHEN** a folder of plugins is found in yt-dlp's home before a look
- **THEN** yt-dlp is not run, and the look and the administrator's page say which folder is not empty

### Requirement: Nothing fetched from YouTube is re-encoded or cut
A fetch SHALL keep the video and audio streams as YouTube served them. yt-dlp SHALL be asked for the best picture within the height setting (`youtube.max_height`), H.264 video with AAC audio first, and SHALL merge separate streams into a Matroska file by a stream copy. A video YouTube serves as one file SHALL be kept in the container it came in (MP4 or WebM). No option that re-encodes a stream or removes a segment SHALL ever be passed: no SponsorBlock request and no cut.

#### Scenario: A video served as separate video and audio streams
- **WHEN** a video is fetched
- **THEN** yt-dlp's command asks for the streams to be merged into a Matroska file and carries no option that re-encodes or cuts

#### Scenario: A video served as one file
- **WHEN** YouTube serves a video as one MP4 file
- **THEN** it is saved with the `.mp4` extension, as it came

### Requirement: No account is used, and a video that needs one is not fetched
The downloader SHALL fetch from YouTube without cookies and without signing in. A video that needs an account (private, for members, or age restricted), a live stream that is still running, and a video not shown yet SHALL be refused when it is looked up, with a sentence that says why, and SHALL NOT be fetched.

#### Scenario: A video that asks for sign-in
- **WHEN** a video can be watched only when signed in
- **THEN** the look says that the video needs a YouTube account and that Axis does not sign in, and nothing is queued

#### Scenario: A live stream
- **WHEN** the video looked up is a live stream that is still running
- **THEN** the look says that Axis does not record live streams, and nothing is queued

### Requirement: YouTube traffic fails closed under the outbound binding
yt-dlp makes its own connections, which the outbound binding does not cover. While `outbound.bind` is set, the downloader SHALL NOT run yt-dlp unless a proxy for YouTube is set (`youtube.proxy`) or an administrator has confirmed that the whole network of the daemon goes through the bound interface (`youtube.network_bound`). The proxy's address SHALL appear in no page.

#### Scenario: A binding and no proxy for YouTube
- **WHEN** `outbound.bind` names an interface, no YouTube proxy is set, and nothing was confirmed
- **THEN** a look is refused saying why and what resolves it, yt-dlp is not run, and the YouTube page offers no address box

#### Scenario: A binding and a proxy
- **WHEN** `outbound.bind` names an interface and `youtube.proxy` is set
- **THEN** yt-dlp runs with the proxy in its environment, its command holds no proxy, and the administrator's page does not show the proxy's address

### Requirement: A video is saved into a YouTube library folder, never over a file
A fetched video SHALL be written only inside the YouTube library folder it was asked for: yt-dlp works in a folder of its own there, and the finished file is moved under a temporary name and then given its final name by a rename that never replaces a file. The names SHALL come from what yt-dlp says of the video (channel, upload date, title, ids), made safe by the import's character rule and byte budget. Nothing SHALL be written while storage is read-only, the disk is read-only or offline, or the disk would be left with less than the free space imports keep; free space SHALL be checked before the download is queued, before yt-dlp starts and about once a minute while it runs. The folder yt-dlp worked in SHALL be removed however the fetch ends.

#### Scenario: A download to completion
- **WHEN** an administrator looks a video up and downloads it
- **THEN** its file is at `<Channel> [<channel id>]/Season <year>/<Channel> - <date> - <Title> [<video id>].mkv` in the YouTube library folder, the download is listed as imported, and no other file or folder of the fetch remains

#### Scenario: A file of that name is there
- **WHEN** a file with the video's name is already in the library folder
- **THEN** the download is refused, and the file is as it was

#### Scenario: Read-only storage
- **WHEN** a download is asked for while `storage.read_only` is on
- **THEN** it is refused saying so, nothing is queued and nothing is written

#### Scenario: Not enough room
- **WHEN** the disk would be left with less than the free space imports keep
- **THEN** the download is refused, or the running fetch is stopped, saying so, and nothing of it is left

### Requirement: A YouTube download is listed, followed and stopped like any download
A fetch SHALL be a download of the built-in client "YouTube (yt-dlp)": recorded once, listed on the Downloads page with its progress, and removable there. Removing one that is running SHALL kill yt-dlp and remove the folder it worked in. The downloader SHALL keep one record per YouTube video id, and a video that is being fetched, or is in the library with its file still there, SHALL NOT be fetched again. A fetch SHALL be tried once: a fetch cut short by Axis stopping or by a killed daemon SHALL end as failed with a reason that says so, and the video can be asked for again.

#### Scenario: Progress
- **WHEN** yt-dlp reports how much of a video it has
- **THEN** the download's record carries it, and the Downloads page and the YouTube page show it

#### Scenario: Stopped by a person
- **WHEN** an administrator removes a YouTube download that is running
- **THEN** yt-dlp is no longer running, the folder it worked in is gone, no file of the video is in the library folder, and the download is listed as removed

#### Scenario: The same video again
- **WHEN** a video that is in the library is looked up again
- **THEN** yt-dlp is not run, the page says where the video is, and a download of it is refused

#### Scenario: A killed daemon
- **WHEN** the daemon is killed while a video is being fetched, and starts again
- **THEN** the video and its download are failed with a reason that says the download was cut short, the folder yt-dlp worked in is removed, and nothing fetches the video again by itself

### Requirement: YouTube work pauses when YouTube refuses the server
When yt-dlp reports that YouTube refused the server (too many requests, or a check that the visitor is not a bot), the downloader SHALL run no yt-dlp for a time that starts at one hour and doubles up to one day, SHALL stop a fetch that is running, SHALL say so with the time on both YouTube pages, and SHALL NOT try again by itself.

#### Scenario: A bot check
- **WHEN** a look fails because YouTube asked to confirm that the visitor is not a bot
- **THEN** the next look is refused saying until when YouTube work is paused, and yt-dlp is not run

