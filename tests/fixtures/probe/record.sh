#!/bin/sh
# record.sh - generates small media files with ffmpeg's test sources and
# records what the probe's two ffprobe calls print for them.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   sh tests/fixtures/probe/record.sh OUTDIR WORKDIR
#
# OUTDIR gets NAME.json (the container-and-streams call) and, for video,
# NAME.frames.json (the one-frame call), plus VERSION holding the first line
# of `ffprobe -version`.  WORKDIR holds the generated media and should be a
# new directory.  The two calls below must be kept the same as
# axis_probe_build_argv() in src/probe/axis-probe.c; the file is given as
# descriptor 3, as the probe gives it.
#
# What this cannot make: Dolby Vision, HDR10+, image subtitles (PGS), DTS-HD
# Master Audio.  Those fixtures are the constructed-*.json files, written by
# hand from ffprobe's documented output, and are marked as such (see
# README.md).

set -eu

out=${1:?output directory}
work=${2:?work directory}
ffmpeg=${FFMPEG:-ffmpeg}
ffprobe=${FFPROBE:-ffprobe}

mkdir -p "$out" "$work"
"$ffprobe" -version | head -n 1 > "$out/VERSION"

WL="matroska,webm,mov,mp4,m4a,3gp,3g2,mj2,avi,mpegts,mpegtsraw,mpeg,mpegvideo,asf,asf_o,flv,ogg,flac,mp3,wav,w64,aiff,aac,ac3,eac3,ac4,dts,dtshd,truehd,mlp,ape,wv,tta,tak,caf,amr"

V="-f lavfi -i testsrc2=size=320x240:rate=24:duration=1"
A="-f lavfi -i sine=frequency=440:duration=1"

gen() {
	name=$1
	shift
	"$ffmpeg" -v error -y "$@" "$work/$name"
}

# The container and streams.
record() {
	"$ffprobe" -v error -hide_banner -protocol_whitelist fd -format_whitelist "$WL" -fd 3 -threads 1 \
		-print_format json -show_error -show_format -show_streams -show_chapters -i fd: \
		3< "$work/$1" > "$out/$2.json" || true
}

# One frame of stream $3.
record_frames() {
	"$ffprobe" -v error -hide_banner -protocol_whitelist fd -format_whitelist "$WL" -fd 3 -threads 1 \
		-print_format json -show_error -select_streams "$3" -show_frames -read_intervals '%+#1' \
		-show_entries frame=pix_fmt,color_space,color_primaries,color_transfer,side_data_list -i fd: \
		3< "$work/$1" > "$out/$2.frames.json" || true
}

printf '1\n00:00:00,000 --> 00:00:01,000\nHello\n' > "$work/en.srt"
printf '1\n00:00:00,000 --> 00:00:01,000\nBonjour\n' > "$work/fr.srt"

# 1. H.264 8-bit SDR, two audio tracks, two text subtitle tracks, in
# Matroska: no stream states its bitrate.
gen h264-sdr.mkv $V $A $A -i "$work/en.srt" -i "$work/fr.srt" -map 0 -map 1 -map 2 -map 3 -map 4 \
	-c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 64k -c:s srt \
	-metadata:s:a:0 language=eng -metadata:s:a:1 language=fra \
	-metadata:s:s:0 language=eng -metadata:s:s:1 language=fra -disposition:s:1 forced
record h264-sdr.mkv h264-sdr
record_frames h264-sdr.mkv h264-sdr 0

# 2. HEVC 10-bit with HDR10 signalling and static metadata (in the
# bitstream, so only the frame call sees it).
HDR10="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400:hdr10=1:repeat-headers=1:log-level=error"
gen hevc-hdr10.mkv $V -vf format=yuv420p10le -c:v libx265 -x265-params "$HDR10"
record hevc-hdr10.mkv hevc-hdr10
record_frames hevc-hdr10.mkv hevc-hdr10 0

# 3. HLG.
gen hevc-hlg.mkv $V -vf format=yuv420p10le -c:v libx265 \
	-x265-params "colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:repeat-headers=1:log-level=error"
record hevc-hlg.mkv hevc-hlg
record_frames hevc-hlg.mkv hevc-hlg 0

# 4. PQ without mastering metadata: not classified.
gen hevc-pq-bare.mkv $V -vf format=yuv420p10le -c:v libx265 \
	-x265-params "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:repeat-headers=1:log-level=error"
record hevc-pq-bare.mkv hevc-pq-bare
record_frames hevc-pq-bare.mkv hevc-pq-bare 0

# 5. 10-bit with no colour signalling at all: not classified.
gen hevc-10bit-unsignalled.mkv $V -vf format=yuv420p10le -c:v libx265 -x265-params "log-level=error"
record hevc-10bit-unsignalled.mkv hevc-10bit-unsignalled
record_frames hevc-10bit-unsignalled.mkv hevc-10bit-unsignalled 0

# 6. 10-bit with BT.709 signalling: SDR.
gen hevc-10bit-bt709.mkv $V -vf format=yuv420p10le -c:v libx265 \
	-x265-params "colorprim=bt709:transfer=bt709:colormatrix=bt709:log-level=error"
record hevc-10bit-bt709.mkv hevc-10bit-bt709
record_frames hevc-10bit-bt709.mkv hevc-10bit-bt709 0

# 7. Interlaced H.264 (top field first).
gen h264-interlaced.mkv $V -vf "tinterlace=interleave_top" -c:v libx264 -flags +ilme+ildct \
	-x264opts tff=1
record h264-interlaced.mkv h264-interlaced
record_frames h264-interlaced.mkv h264-interlaced 0

# 8. Several audio codecs beside one video stream.
gen multi-audio.mkv $V $A $A $A $A $A $A -map 0 -map 1 -map 2 -map 3 -map 4 -map 5 -map 6 \
	-c:v libx264 -pix_fmt yuv420p \
	-c:a:0 aac -c:a:1 ac3 -c:a:2 eac3 -c:a:3 flac -c:a:4 truehd -c:a:5 libopus -strict -2 \
	-disposition:a:1 default
record multi-audio.mkv multi-audio
record_frames multi-audio.mkv multi-audio 0

# 9. FLAC with tags, ReplayGain, MusicBrainz identifiers and cover art.
"$ffmpeg" -v error -y -f lavfi -i testsrc2=size=64x64:rate=1 -frames:v 1 "$work/cover.png"
gen album-track.flac $A -i "$work/cover.png" -map 0 -map 1 -c:a flac -c:v copy -disposition:v attached_pic \
	-metadata title="Track One" -metadata artist="The Band" -metadata album="The Album" \
	-metadata track=1/10 -metadata disc=1/2 \
	-metadata REPLAYGAIN_TRACK_GAIN="-6.5 dB" -metadata MUSICBRAINZ_TRACKID=00000000-0000-0000-0000-000000000001
record album-track.flac album-track

# 10. MP3 with ID3 tags and cover art.
gen album-track.mp3 $A -i "$work/cover.png" -map 0 -map 1 -c:a libmp3lame -b:a 192k -c:v copy \
	-id3v2_version 3 -disposition:v attached_pic \
	-metadata title="Track Two" -metadata artist="The Band" -metadata album="The Album" -metadata track=2/10
record album-track.mp3 album-track-mp3

# 11. Opus in Ogg.
gen voice.opus $A -c:a libopus -b:a 32k -metadata title="A Voice" -metadata artist="Someone"
record voice.opus voice-opus

# 12. An audiobook: AAC in M4B with chapters.
printf ';FFMETADATA1\ntitle=The Book\nartist=An Author\nalbum=The Book\n' > "$work/book.txt"
for n in 1 2 3; do
	start=$(( (n - 1) * 300 ))
	end=$(( n * 300 ))
	printf '[CHAPTER]\nTIMEBASE=1/1000\nSTART=%s\nEND=%s\ntitle=Chapter %s\n' "$start" "$end" "$n" >> "$work/book.txt"
done
gen book.m4b $A -i "$work/book.txt" -map 0:a -map_chapters 1 -map_metadata 1 -c:a aac -b:a 48k -f ipod
record book.m4b book-m4b

# 13. MP4 with a text subtitle track.
gen h264-mp4-subs.mp4 $V -i "$work/en.srt" -map 0 -map 1 -c:v libx264 -pix_fmt yuv420p -c:s mov_text \
	-metadata:s:s:0 language=eng
record h264-mp4-subs.mp4 h264-mp4-subs
record_frames h264-mp4-subs.mp4 h264-mp4-subs 0

# 14. 24-bit PCM, whose bitrate follows from its parameters.
gen pcm24.wav $A -c:a pcm_s24le -ar 48000
record pcm24.wav pcm24-wav

# 15. A variable frame rate (in MP4, whose average rate is counted from the
# frames; Matroska only repeats the rate it was told).
gen h264-vfr.mp4 $V -vf "setpts='PTS*(1+0.5*mod(N,2))+N*0.0001/TB'" -fps_mode passthrough -c:v libx264 \
	-pix_fmt yuv420p
record h264-vfr.mp4 h264-vfr
record_frames h264-vfr.mp4 h264-vfr 0

# 17. HDR10 whose static metadata is in the container too: ffmpeg 7.1 and
# later pass a decoder's mastering display and content light data on to the
# encoder and the muxer, so re-encoding file 2 writes them into the
# Matroska Colour element (as floats, which ffprobe prints as "17/25") as
# well as into the bitstream.
gen hevc-hdr10-reencoded.mkv -i "$work/hevc-hdr10.mkv" -c:v libx265 -x265-params log-level=error
record hevc-hdr10-reencoded.mkv hevc-hdr10-reencoded
record_frames hevc-hdr10-reencoded.mkv hevc-hdr10-reencoded 0

# 18. HDR10 with mastering display metadata and no content light levels
# (x265 writes zeros for them unless told not to: cll=0).
HDR10_NOCLL="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50):hdr10=1:cll=0:repeat-headers=1:log-level=error"
gen hevc-hdr10-nocll.mkv $V -vf format=yuv420p10le -c:v libx265 -x265-params "$HDR10_NOCLL"
record hevc-hdr10-nocll.mkv hevc-hdr10-nocll
record_frames hevc-hdr10-nocll.mkv hevc-hdr10-nocll 0

# 16. What ffprobe says about files that are not media or are cut short.
head -c 3000 /dev/urandom > "$work/random.bin" 2>/dev/null || dd if=/dev/zero of="$work/random.bin" bs=3000 count=1 2>/dev/null
record random.bin corrupt-random
size=$(wc -c < "$work/h264-mp4-subs.mp4")
head -c $(( size / 2 )) "$work/h264-mp4-subs.mp4" > "$work/truncated.mp4"
record truncated.mp4 corrupt-truncated-mp4
: > "$work/empty.mkv"
record empty.mkv corrupt-empty

echo "recorded in $out"
