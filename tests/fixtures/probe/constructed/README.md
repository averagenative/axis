# Constructed fixtures

These files are NOT recorded from real media. They were written by hand
from ffprobe's documented output shape (the field names and value
formats `-print_format json -show_format -show_streams -show_chapters`
and `-show_frames` print), because the formats they stand for cannot be
generated on the development machine: its ffmpeg has no Dolby Vision or
HDR10+ writer, no image-subtitle encoder, and no DTS-HD Master Audio
encoder. They are unverified against real files of those kinds.

What they therefore prove is only that the mapper reads that documented
shape and classifies it the way `src/probe/axis-probe-hdr.c` says. They do
not prove that a real Dolby Vision file looks like this to the ffprobe on
the machine that will run the probe. Files that look like Dolby Vision or
dynamic HDR metadata but do not match the expected shape exactly are
classified "uncertain" on purpose, so that a difference between this
shape and a real file costs a conversion that does not happen, never a
file.

Each file starts with a `"_constructed"` member (which the mapper
ignores) saying what it stands for. Replace a file here with a recording
of a real one as soon as one is at hand: the tests name each file, and
the expectations stay.

| File | Stands for |
|---|---|
| `constructed-dv-p5.json` | Dolby Vision profile 5 (no usable base layer), 10-bit HEVC with no colour signalling |
| `constructed-dv-p81.json` | Dolby Vision profile 8.1, HDR10-compatible base layer |
| `constructed-dv-p82.json` | Dolby Vision profile 8.2, SDR-compatible base layer |
| `constructed-dv-p84.json` | Dolby Vision profile 8.4, HLG-compatible base layer |
| `constructed-dv-p7.json` | Dolby Vision profile 7 (dual layer), as stored in one Matroska track |
| `constructed-dv-bad-record.json` | A Dolby Vision record that is not in the expected shape |
| `constructed-dv-rpu-only.json`, `constructed-dv-rpu-only.frames.json` | Dolby Vision frame metadata with no configuration record |
| `constructed-hdr10plus.json`, `constructed-hdr10plus.frames.json` | HDR10+ (dynamic metadata on the first frame) |
| `constructed-dv-and-hdr10plus.json`, `.frames.json` | Both signalled at once |
| `constructed-dynamic-other.json`, `.frames.json` | Dynamic HDR metadata of a kind the probe does not know |
| `constructed-hdr10-container.json` | HDR10 whose static metadata is in the container (stream side data) |
| `constructed-remux.json` | A Blu-ray remux: HEVC, DTS-HD MA, TrueHD with Atmos, PGS subtitles |
