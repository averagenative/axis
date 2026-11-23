# Download a video from YouTube

This page shows you how to switch the YouTube downloader on, give it a
library folder, and download one video. The example is a video called "The
Lost Spoon" from a channel called "Moon Kettle Studio". Allow about 10
minutes of your time. The download itself takes as long as YouTube takes.

Axis downloads one video at a time in this version. Playlists, channels, and
following a channel for new uploads come later.

## Before you begin

You need:

- An administrator account. Only administrators can switch YouTube on, look
  a video up, or download it.
- An online disk that Axis can write to
  ([Add disks and library folders](02-add-disks-and-library-folders.md)).
- **Read-only storage** switched off. Open **System**, then **Settings**, and
  find **Read-only storage** in the **Disks** card. While it is on, Axis
  refuses the download and says "Storage is read-only".
- The Axis container image, which includes `yt-dlp`, the program that does
  the downloading. If you run Axis another way, install yt-dlp yourself and
  set **yt-dlp program** in **Settings**.

Downloading from YouTube may be against YouTube's terms of service, and
against the law where you live. Axis does not decide that for you. You are
responsible for what you download.

## Add a YouTube library folder

Videos from YouTube go into a library folder of their own, apart from Movies
and TV.

1. Open **Media management**, then **Storage**, and open the disk.
2. In the form that adds a library folder, type the folder's path, such as
   `YouTube`, and choose the kind **YouTube**.
3. Select the button that adds the folder.

Axis does not add the folder to Plex or Jellyfin. Add it there yourself, as a
library that reads folder and file names rather than an online source: a TV
library with a personal-media agent in Plex, or a Shows library with its
metadata sources switched off in Jellyfin.

## Switch YouTube on

1. Open **System**, then **YouTube downloader**.
2. Read the notice, tick **I have read this, and what I download is mine to
   answer for**, and select **Switch YouTube on**.

The page now says who switched it on and when, which yt-dlp runs, and its
version. Until you do this, Axis asks nothing of YouTube and does not run
yt-dlp.

## Download a video

1. Open **Media**, then **YouTube**.
2. In **Address of the video**, paste the video's address from your
   browser, such as `https://www.youtube.com/watch?v=...`, and select **Look
   it up**. Addresses at `youtube.com` and `youtu.be` work. If the address
   also names a playlist, Axis takes the one video.
3. Read the **This video** card: the title, the channel, the upload date, the
   length, what Axis would take (for example "1080p, avc1 with mp4a"), and
   about how large it is. Nothing is downloaded yet.
4. Select **Download**. If you have more than one YouTube library folder,
   select the button that names the folder you want.

The video appears in the **Videos** table as **Queued**, then
**Downloading**, then **In the library**. The **Downloads** page lists it
too, with its progress, under the download client **YouTube (yt-dlp)**.

## Find the file

Axis saves the video under its channel and the year it was uploaded:

```
YouTube/
  Moon Kettle Studio [<channel id>]/
    Season 2021/
      Moon Kettle Studio - 2021-03-15 - The Lost Spoon [<video id>].mkv
```

Axis takes the video as YouTube serves it and does not re-encode it. When
YouTube serves the picture and the sound separately, yt-dlp joins them into
one Matroska file (`.mkv`) without changing either. When YouTube serves the
video as one file, Axis keeps the file as it is, so its name ends in `.mp4`
or `.webm`.

Axis never replaces a file. If a file with that name is already in the
folder, the download is refused and the file stays as it was.

Axis keeps free space on the disk. A download needs room for twice the
video's size while yt-dlp puts it together, plus the free space that imports
keep (the setting **Free space to keep when copying**,
`import.free_space_floor`). If yt-dlp can't say how large the video is, Axis
allows for twice **Largest video file**. Axis checks the free space when you
select **Download**, again when the download starts, and about once a minute
while it runs.

## Stop a download

1. Open **Activity**, then **Downloads**.
2. In the video's row, select **Remove**, tick the box, and select **Remove
   this download**.

Axis stops yt-dlp and deletes what was downloaded so far. A video that is
already in the library stays there.

Three other things stop a download that is running, and each one says so on
the video and on the **Downloads** page:

- **Switching YouTube off.** The download that is running and the ones that
  wait are stopped. The **YouTube downloader** page says how many.
- **A pause.** If YouTube refuses this server while a video downloads, Axis
  stops that download too.
- **Axis stopping or restarting.** The video says "Axis stopped while it was
  downloading; download it again." The same happens when Axis is killed: the
  next time Axis starts, it marks the video as failed and removes what yt-dlp
  had written.

In each case, look the video up and download it again when you are ready.
Axis does not start it again by itself.

## Change what is taken

Open **System**, then **Settings**, and find the **YouTube** card:

| Setting | What it does |
| --- | --- |
| **Largest picture height** | The tallest picture Axis takes: 480, 720, 1080 or 2160 lines. The default is 1080. |
| **Largest video file** | A video larger than this is not downloaded. The default is 8 GiB. |
| **yt-dlp program** | The yt-dlp that Axis runs: a name on its PATH, or an absolute path to a copy of your own. |
| **Proxy for YouTube** | A proxy that yt-dlp uses, such as `socks5h://vpn:1080`. |
| **All of Axis's network goes through the VPN** | Confirms that Axis runs in a VPN container's network. |

For a SOCKS proxy, the scheme decides where names are looked up. With
`socks5://`, this machine looks up the names of YouTube's servers with its
own DNS, and only the connections go through the proxy. With `socks5h://`,
the proxy looks the names up too. Use `socks5h://` when the proxy is a VPN,
so that your DNS provider doesn't see the lookups.

Axis passes the proxy to yt-dlp in its environment, not on its command line,
so other users of the machine can't read it in the process list.

## If something goes wrong

| What you see | What it means | What to do |
| --- | --- | --- |
| "YouTube is switched off" | Nobody has switched it on yet. | Follow [Switch YouTube on](#switch-youtube-on). |
| "That is a playlist" or "That is a channel" | This version downloads one video at a time. | Paste the address of one video. |
| "There is no YouTube library folder yet" | Axis has nowhere to save the video. | Follow [Add a YouTube library folder](#add-a-youtube-library-folder), then look the video up again. |
| "Storage is read-only" | The setting **Read-only storage** is on. | Switch it off in **Settings**. |
| "YouTube refused this server", and then "YouTube work is paused until" a time | YouTube answered with too many requests or a bot check. Axis waits an hour, and twice as long each time it happens again, up to a day. | Wait. Axis does not try again by itself, so download the video again after the time shown. |
| "This video needs a YouTube account" | The video is private, for members, or age restricted. | Axis does not sign in to YouTube. It cannot download this video. |
| "This is a live stream that is still running" | Axis does not record live streams. | Download it after the stream has ended. |
| "Axis's connections are bound to a network interface" | You set `outbound.bind`, and yt-dlp makes its own connections outside it. | Set **Proxy for YouTube**, or, if Axis runs in the VPN container's network, switch on **All of Axis's network goes through the VPN**. |
| "The disk ... has ... free, and this download needs ..." | The disk doesn't have room for the video and the free space Axis keeps. | Free some space, or lower **Largest video file** if yt-dlp gave no size. Then download the video again. |
| "The disk ... has ... free, less than the ... Axis keeps free" | The disk filled up while the video was downloading. Axis stopped the download and removed what it had written. | Free some space, then download the video again. |
| "Axis stopped while it was downloading; download it again." | Axis stopped, restarted, or was killed during the download. | Look the video up and download it again. |
| "Stopped: YouTube was switched off." | An administrator switched YouTube off while the video was downloading or waiting. | Switch YouTube on, then download the video again. |
| "The folder Axis gives yt-dlp as its home ... is not empty" | yt-dlp loads plugins from its home folder. Axis keeps that folder empty and doesn't run yt-dlp while something is in it. | Look at what is in the folder that the message names, in Axis's data directory under `youtube`. If you didn't put it there, treat it as suspicious. Remove it, then try again. |
| A download fails with "yt-dlp said:" and yt-dlp's own words | yt-dlp could not download the video. Often YouTube has changed and your yt-dlp is too old. | Pull or build a newer Axis image, which takes the newest yt-dlp, or point **yt-dlp program** at a newer copy. Then look the video up and download it again. |

Axis never updates yt-dlp by itself. The **YouTube downloader** page shows
the version that runs. An image takes the newest yt-dlp on the day it is
built, so the amd64 and arm64 images of one Axis release can hold different
yt-dlp versions if they were built on different days.
