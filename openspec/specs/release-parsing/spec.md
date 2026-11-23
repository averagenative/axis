# release-parsing Specification

## Purpose
Reading a release name as indexers and download clients give it into the fields the rest of the daemon decides on: which title it is, which episodes, and what quality. Everything that searches, ranks, follows or imports a release depends on this reading, so it never fails and says what it could not read.
## Requirements
### Requirement: A release name is read into fields
The daemon SHALL read a release name into its title, year, season and episode numbers, resolution, source, video codec, HDR kind, audio, edition, languages, revision marks and release group. Reading SHALL never fail: a part that cannot be read is reported as not found.

#### Scenario: A film
- **WHEN** `Glass.Harbour.Part.Two.2024.2160p.UHD.BluRay.x265.10bit.HDR.DTS-HD.MA.5.1-GROUP` is read
- **THEN** the title is "Glass Harbour Part Two", the year 2024, the resolution 2160p, the source BluRay, the codec H.265 and the group "GROUP"

#### Scenario: An episode
- **WHEN** `Show.Name.S02E05.1080p.WEB-DL.DDP5.1.H.264-GROUP` is read
- **THEN** the title is "Show Name", the season 2, the episode 5, and the source WEB-DL

#### Scenario: Several episodes and season packs
- **WHEN** a name carries `S01E02E03`, or `S01` alone
- **THEN** it is read as episodes 2 and 3 of season 1, or as the whole of season 1

#### Scenario: An ordinary word that is also a quality word
- **WHEN** `Lantern.Web.2024.1080p.BluRay.x264-GROUP` is read
- **THEN** the title is "Lantern Web" and the source is BluRay

#### Scenario: A title that is a year
- **WHEN** `1958.2019.1080p.BluRay.x264-GROUP` is read
- **THEN** the title is "1958" and the year 2019

### Requirement: Nothing is inferred
A field SHALL be reported only when the name states it. The parts of a name that were not understood SHALL be reported.

#### Scenario: No resolution in the name
- **WHEN** a name states a source and no resolution
- **THEN** the resolution is not found

### Requirement: Marks that should stop a grab are reported
The reader SHALL report samples, password-protected releases, cinema recordings, names that carry no readable title, and Usenet post subjects for one part of an upload.

#### Scenario: Cinema recording
- **WHEN** a name carries a cam or telesync mark
- **THEN** the source says so

#### Scenario: A post's subject
- **WHEN** a name is a Usenet post's subject for one part of an upload, with a part counter such as `(101/171)` and a file's name in quotes
- **THEN** it is reported as a post's subject, which no search takes

### Requirement: Reading is bounded
Reading SHALL take time proportional to the name's length, SHALL accept only valid text up to a fixed length, and SHALL NOT fail on any input.

#### Scenario: Hostile input
- **WHEN** a name is invalid text, empty or longer than the limit
- **THEN** nothing is found and the call returns

### Requirement: A name is matched to a title without guessing
A read name SHALL be matched to a known title by its cleaned title, original title or other names, and its year, with the year allowed to differ by one. A title of that name whose known year differs by more SHALL NOT fit, and neither a title of that name with no year known nor one a year off SHALL stand in the way of the one title whose year is the name's; two titles of the name's own year SHALL be ambiguous. The result SHALL be a match, no match or ambiguous, ambiguous SHALL NOT be treated as a match, and the reason SHALL name each title with its year, or say it has none.

#### Scenario: Another name
- **WHEN** a release uses a title's other name from the metadata store
- **THEN** it matches that title

#### Scenario: Two candidates
- **WHEN** a name without a year fits two known titles
- **THEN** the result is ambiguous, and its reason names both with their years

#### Scenario: Same-named titles of other years
- **WHEN** a name states a year, and of several titles of that name one has that year while the others have other years or no year known
- **THEN** it matches the one with that year

#### Scenario: A same-named title with no year, a year off
- **WHEN** a name states a year one off the only known year of its name, and another title of that name has no year known
- **THEN** the result is ambiguous

### Requirement: A quality word can be a word of the title
A word the reader knows as a quality word (a source, an audio or HDR word) SHALL be read as a word of the title, not as quality, when it is a word of letters outside brackets with a year ahead of it before any other quality word, and either the title would be nothing (or only an article) without it, or it is written as a title word, not in capitals, and the name ends with the year in brackets, as a folder is named "Title (Year)". A quality word after the year SHALL be read as quality as before, and so SHALL a quality word before a bracketed year that more of the name follows. An edition word SHALL be read as an edition as before.

#### Scenario: A title that starts with a quality word
- **WHEN** `Atmos (2018)` or `Hdr Ghosts (2019)` is read
- **THEN** the title is "Atmos" or "Hdr Ghosts", with the year

#### Scenario: A quality word inside a title
- **WHEN** `The Remux Brothers (2010) Remux-1080p.mkv` is read
- **THEN** the title is "The Remux Brothers", the year 2010, and the source Remux

#### Scenario: A folder's name with a quality word in its title
- **WHEN** `Dark Remux Harbour (2011)` is read
- **THEN** the title is "Dark Remux Harbour", with the year

#### Scenario: A tag in capitals
- **WHEN** a name gives a title, then a quality word in capitals, then the year
- **THEN** the quality word is read as quality, as before

#### Scenario: Quality after the bracketed year
- **WHEN** `Copper Sky Remux (2012) 1080p BluRay` is read
- **THEN** the title is "Copper Sky" and the source Remux, as before

#### Scenario: An edition before a bracketed year
- **WHEN** `Copper Sky Unrated (2012) 1080p` is read
- **THEN** the title is "Copper Sky", the year 2012 and the edition Unrated, as before

### Requirement: Letters that do not decompose are compared as the plain letters
Titles SHALL be compared with the letters that do not decompose written as the plain letters they stand for: "ø" as "o", "đ" and "ð" as "d", "ł" as "l", "æ" as "ae", "œ" as "oe", "þ" as "th", "ß" as "ss", "ħ" as "h", "ı" as "i" and "ŧ" as "t", in either case. The comparison form of a comparison form SHALL be itself.

#### Scenario: A name written without them
- **WHEN** a folder named "Sorens Winter" is compared with the title "Søren's Winter"
- **THEN** they are the same title

### Requirement: A folder's name always has its title
A name that is exactly "<something> (<year>)" (its file extension and trackers' tags taken off, a space before the bracket, nothing after it, no other bracket, and a year believed) SHALL be reported as a folder's form, and SHALL always have a title when the something holds a letter or a digit: when its first word is a quality word ("4K", "x264", "1080p"), everything before the year SHALL be the title. An edition word or a tag after a title SHALL be read as before. A release name with more after the year, or a year not in brackets, SHALL be read as before.

#### Scenario: A quality word alone
- **WHEN** `4K (2015)` or `x264 (2015).mkv` is read
- **THEN** the title is "4K" or "x264", with the year, and no resolution or codec is read from it

#### Scenario: A quality word first
- **WHEN** `4K Ghosts (2015)` is read
- **THEN** the title is "4K Ghosts", with the year

#### Scenario: An edition after the title
- **WHEN** `Copper Sky Extended (2012)` is read
- **THEN** the title is "Copper Sky", the year 2012 and the edition Extended, as before

#### Scenario: Not a folder's form
- **WHEN** `4K (2015) 2160p` or `Lantern.Cove.(2010)` is read
- **THEN** it is not reported as a folder's form, and it is read as before

#### Scenario: A release named by a number
- **WHEN** `412.2011.1080p.BluRay.x264-GROUP` is read
- **THEN** the title is "412" and the year 2011, as before

### Requirement: A name can be read after a known title
Besides reading a name by itself, the daemon SHALL be able to read a name against the names of one known series: when the name, after a leading group in brackets and site tags, begins with one of those names at a word boundary, the longest that fits being taken, what follows SHALL be read as an absolute episode number or a range of them when it is exactly one whole number of one to four digits with an optional version (`v2`), or one range (`01-12`, `01 ~ 12`), with or without a lone hyphen before it, followed by the end of the name, a bracket, a word the parser's tables know or a site's tag. Anything else SHALL be reported as not read. A four-digit number that can be a year, a number with a decimal part, two numbers in a row, 0, a single digit with no hyphen before it (a sequel or a season far more often than an episode), a range that runs backwards or holds more than 200 numbers, and a word in the number's place that names a special, an extra or a film (such as OVA or NCOP) SHALL be not read, each with a code of its own that says why. A number that stands alone in a bracket after the number (`10 [05]`, `10 (05)`, `10 [1080p][05]`) SHALL count as a second number, unless it can be a year or is a picture's height (`[1080]`), and so SHALL a number joined to it by `&`, `+` or `and`. A word right after the number that says the number is not one episode (`Complete`, `Batch`, `BD`, `Vol`, `Volume`, a special's word such as `Movie` or `PV`, `Preview`, `Part 2`, `of 12`) SHALL make the name not read, with a code of its own, although reading a name by itself knows some of these words. The name SHALL be compared with the series' names by title key, exactly as a title is matched, and SHALL end at the end of a word: no part of a word and nothing merely alike is the series' name. This reading SHALL be bounded and SHALL NOT fail on any input, as reading a name by itself is, and it SHALL NOT change what reading a name by itself reports for any name. Which series it is used for is the caller's rule.

#### Scenario: A bare number
- **WHEN** `[Tsuki] Tenkuu Rangers 137 [1080p]` is read against the series named "Tenkuu Rangers"
- **THEN** the absolute number is 137

#### Scenario: A range with no brackets
- **WHEN** `Tenkuu Rangers 001-052 [BD 1080p]` is read against "Tenkuu Rangers"
- **THEN** the absolute numbers are 1 to 52

#### Scenario: The title ends in a number
- **WHEN** `[Tsuki] Tenkuu Rangers 2 - 05 [1080p]` is read against a series whose names are "Tenkuu Rangers 2"
- **THEN** the absolute number is 5

#### Scenario: A number that is not the title's
- **WHEN** the same name is read against a series whose only name is "Tenkuu Rangers"
- **THEN** it is not read, because two numbers follow the title

#### Scenario: Half an episode
- **WHEN** `[NekoFansub] Kumo Academy - 12.5 [720p]` is read against "Kumo Academy"
- **THEN** it is not read

#### Scenario: A year
- **WHEN** `Kumo Academy 2023 [1080p]` is read against "Kumo Academy"
- **THEN** it is not read

#### Scenario: Another series
- **WHEN** a name does not begin with any of the series' names
- **THEN** it is not read

#### Scenario: One more letter
- **WHEN** `[Tsuki] Tenkuu Rangersz 137 [1080p]` is read against "Tenkuu Rangers"
- **THEN** it is not read, because the name must end at a word

#### Scenario: A single digit with no hyphen
- **WHEN** `Tenkuu Rangers 2 [1080p]` is read against "Tenkuu Rangers"
- **THEN** it is not read, while `Tenkuu Rangers - 2 [1080p]` and `Tenkuu Rangers 02 [1080p]` are read as 2

#### Scenario: A special by its word
- **WHEN** `[Tsuki] Tenkuu Rangers - NCOP 01 [1080p]` is read against "Tenkuu Rangers"
- **THEN** it is not read, and the code says it names a special

#### Scenario: Words after the number
- **WHEN** `Tenkuu Rangers 05 The Long Night [720p]` is read against "Tenkuu Rangers"
- **THEN** it is not read, because words that are not understood follow the number

#### Scenario: A number in the bracket after the number
- **WHEN** `Tenkuu Rangers 10 [05][1080p]` is read against "Tenkuu Rangers"
- **THEN** it is not read, because two numbers follow the title: the title may be "Tenkuu Rangers 10"

#### Scenario: A year or a picture's height in the bracket
- **WHEN** `Tenkuu Rangers 05 (2023) [1080p]` and `[Tsuki] Tenkuu Rangers 05 [1080][AAC]` are read against "Tenkuu Rangers"
- **THEN** each is read as 5

#### Scenario: A word that counts something larger
- **WHEN** `[Tsuki] Tenkuu Rangers 02 Complete [BD]` and `Tenkuu Rangers 03 BD 1080p` are read against "Tenkuu Rangers"
- **THEN** neither is read

#### Scenario: A range that runs backwards
- **WHEN** `Kumo Academy 12-01 [720p]` is read against "Kumo Academy"
- **THEN** it is not read, and the code says the range runs backwards

#### Scenario: Reading a name by itself is unchanged
- **WHEN** every name of the release corpora is read by itself, then read after a title, then read by itself again
- **THEN** both readings by itself report the same fields

