# Jellyfin fixtures

Answers of a Jellyfin server, for the parsers and for the fake server the
tests run against (`tests/axis-test-plex.h` serves them). `make check` needs
no container and no network: it only reads these files. `make check-jellyfin`
runs the daemon's collection jobs against a real server and needs a container
runtime.

## recorded/

Real answers of **Jellyfin Server 10.11.8** (`recorded/VERSION`), the image
`docker.io/jellyfin/jellyfin:10.11.8` pinned by digest in
`tests/jellyfin/up.sh`, started in a container on a network with no route
out, with the media tree of `tests/jellyfin/make-media.sh` (synthetic video
and audio from ffmpeg's test sources). The files were written by
`tests/jellyfin/record.sh`: each answer is the server's JSON, pretty-printed
with `jq .` (two spaces; key for key and value for value what the server
sent), and nothing else was changed. Nothing was replaced: the server's
identifiers, the container's host name (`ServerName`), the container
network's addresses (`10.89.x.x`) and the paths inside the container
(`/media/...`) are what the server returned; none belongs to the machine that
ran it.

| File | Request | State |
|---|---|---|
| `identity.json` | `GET /System/Info` | authenticated |
| `public.json` | `GET /System/Info/Public` | no key |
| `libraries.json` | `GET /Library/VirtualFolders` | two libraries |
| `devices-idle.json` | `GET /Devices` | before any client played: the setup session's device |
| `sessions-idle.json` | `GET /Sessions` | the same: one session, playing nothing |
| `items.json` | `GET /Items?Recursive=true&IncludeItemTypes=Movie,Audio&Fields=Path` | the four items |
| `sessions-playing.json` | `GET /Sessions` | seven test clients playing (see below) |
| `devices-playing.json` | `GET /Devices` | after they played |
| `api-description.json` | `GET /api-docs/openapi.json` | the enumerations the parsers must know, taken from the server's own description (not from memory) |

The seven clients of `sessions-playing.json` (`tests/jellyfin/play.sh`
reports playback the way a client does; each has its own device id, name and
device profile, which is what the server decides on):

| Device | Plays | The server's play method | `TranscodingInfo` |
|---|---|---|---|
| Living Room TV | H.264 film | DirectPlay | none |
| Kitchen Speaker | FLAC | DirectPlay | none |
| Pixel Phone | HEVC 10-bit film | Transcode | video converted, audio direct; `ContainerNotSupported`, `VideoCodecNotSupported` |
| Bedroom Stick | H.264 film | Transcode | video and audio direct (only the container is converted); `ContainerNotSupported` |
| Tablet | HEVC 10-bit film | Transcode | video converted; `VideoBitDepthNotSupported` |
| Hotel TV | H.264 film, 500 kb/s limit | Transcode | video and audio converted; `ContainerBitrateExceedsLimit` |
| Games Console | film with two audio tracks | Transcode | video direct, audio converted; `AudioCodecNotSupported` |

`play.sh` was extended after these files were recorded so that its clients also report the audio and subtitle track they chose, as a real player does; the Games Console's session here therefore has no `PlayState.AudioStreamIndex`, which is the case "a client that did not report its track" and is kept as it was. `make check-jellyfin` uses the extended script (the console's audio track is then known).

What the server really did, found out by trying:

* `TranscodingInfo` is present only while the server's conversion is
  running, and then carries `TranscodeReasons`. A conversion that is
  already over when the session is read shows `PlayMethod: DirectPlay` and
  no `TranscodingInfo` (seen once, with a three-second film that converted in
  under a second). The test films are a minute long for this reason.
* The reasons seen: `ContainerNotSupported`, `VideoCodecNotSupported`,
  `VideoBitDepthNotSupported`, `ContainerBitrateExceedsLimit`,
  `AudioCodecNotSupported` in these files; in live runs also
  `VideoLevelNotSupported` and `SubtitleCodecNotSupported`. The full list
  the server can state (27 words) is `api-description.json`.
* A client that did not report which audio track it plays leaves
  `PlayState.AudioStreamIndex` out (the Games Console's session): which of
  two audio tracks was converted is then not known, and nothing is concluded
  about audio.
* `GET /Devices` and `GET /Library/VirtualFolders` need an administrator (an
  API key counts as one); an ordinary user gets 403. `GET /Sessions` and
  `GET /Items` do not.
* Every spelling of the key works (`Authorization: MediaBrowser Token="..."`,
  `X-Emby-Token`, `X-MediaBrowser-Token`, `?api_key=`); a bad key is 401.
  `Authorization: Bearer` is refused. The daemon sends the first.
* The server keeps no per-device play history. `GET /System/ActivityLog/Entries`
  lists "<user> is playing <item> on <device name>" as free text in the
  server's language; it is not used, because free text in any language cannot be read reliably.

## constructed/

Shapes that cannot be produced without a real player, or that a test needs in
a form the server would not give on request. They are **not** recorded
answers: each carries only the fields the parser reads, in the shape of the
recorded sessions, and is labelled here as unverified against a real server.

| File | What it is for |
|---|---|
| `session-audio-selected.json` | the Games Console's session as a client that reports its audio track would give it: `PlayState.AudioStreamIndex` 2 (the AC-3 track) with `AudioCodecNotSupported` |
| `session-subtitle-burned.json` | a subtitle burned into the picture: `SubtitleStreamIndex` 3 and `SubtitleCodecNotSupported` (seen in a live run, not recorded) |
| `session-level.json` | `VideoLevelNotSupported` for HEVC (seen in a live run, not recorded) |
| `session-no-transcoding-info.json` | `PlayMethod: Transcode` with no `TranscodingInfo` (no reason stated: the conversion is not explained) |
| `session-unknown-reasons.json` | a reason the table does not know, an over-long one, one with control characters, and non-strings among them |
| `session-odd-ids.json` | a device id too long to be a key (as a web client makes up), missing and null fields, a number where a string belongs |
| `session-other-version.json` | the client played another version of the item than the one whose streams the session lists |
| `item-one.json` | `GET /Items?Ids=...&Fields=Path` for one item |
| `libraries-mixed.json` | a library with no collection type (mixed content), one with no id, two libraries sharing a folder, locations that are not text |
