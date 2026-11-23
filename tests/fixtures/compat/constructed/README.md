# Constructed media server fixtures

These files are NOT recorded from a real media server. They were written by
hand from the shape of the one session that was observed
(`../session-converted-audio.json`), because the cases they stand for had
not been seen when the change was written (2026-11-12):

- a session played **entirely directly** (expected: no `TranscodeSession`,
  `Part.decision` of `directplay`);
- a **video stream converted** (`videoDecision` and the stream's `decision`
  `transcode`, with an output codec that differs from the source's);
- a **subtitle conversion** (`subtitleDecision`);
- a **bandwidth-limited conversion** (same codec at a lower resolution over a
  remote link whose `Session.bandwidth` is below the source's bitrate);
- a decision word the daemon does not know;
- the server's root document (`GET /`), which carries its friendly name:
  not part of the survey.

They are unverified against a real server. What they prove is only that the
parser and the classification read the documented shape the way
`src/compat/axis-compat-evidence.h` says; a real server's answer may differ
in details (for example the stream `decision` of a direct play may be absent
or `directplay`; both are read as direct). The parser treats any decision
value it does not know as unknown, so a difference costs evidence, never a
file. Replace a file here with a recording of the real thing as soon as one
is at hand: the tests name each file, and the expectations stay.

| File | Stands for |
|---|---|
| `root.json` | `GET /`: the server's friendly name |
| `session-direct.json` | an HEVC 10-bit HDR 2160p film in Matroska played directly on a local device |
| `session-video-converted.json` | the same device asking the server to convert HEVC to H.264 on the local network |
| `session-subtitle.json` | H.264 video and AAC audio passed through, a subtitle converted |
| `session-bandwidth.json` | a remote session converting H.264 to H.264 at a lower resolution because the bandwidth is below the source's bitrate |
| `session-bandwidth-codec.json` | a remote session whose codec changes while the bandwidth is below the source's bitrate (the limit explains it) |
| `session-unclear.json` | a local session converting audio to the same codec (downmix): cause unclear |
| `session-unknown-decision.json` | a stream decision word the daemon has never seen |
| `session-two-audio.json` | direct play of a file with two audio tracks and no sign of which one plays |
