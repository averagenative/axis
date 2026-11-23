# quality-profiles Specification

## Purpose
How Axis decides which releases are acceptable, which is best, and whether one is worth replacing the file on disk: a fixed quality ladder, user profiles over it, and decisions that state their reasons.
## Requirements
### Requirement: Qualities form a fixed ladder
The daemon SHALL place every release and file on a fixed, ordered ladder of qualities derived from its source and resolution. A release whose source or resolution is not known SHALL be of unknown quality.

#### Scenario: Order
- **WHEN** a WEB-DL 1080p release and a BluRay 1080p release are compared on the ladder
- **THEN** the BluRay is the higher

#### Scenario: Unknown
- **WHEN** a release name states no resolution
- **THEN** its quality is unknown

### Requirement: A profile says what is acceptable and what is preferred
A quality profile SHALL state the allowed qualities in order of preference, the quality at which upgrading stops, size limits, and preference rules that require, reject, prefer or avoid releases by their parsed fields.

#### Scenario: Not allowed
- **WHEN** a release's quality is not among the profile's allowed qualities
- **THEN** it is rejected

#### Scenario: Required field
- **WHEN** a profile requires a video codec and a release has another
- **THEN** it is rejected

### Requirement: Every decision states its reasons
For each release the daemon SHALL say whether it is accepted or rejected and SHALL state every reason for a rejection in plain words.

#### Scenario: Several reasons
- **WHEN** a release is both too large and of a rejected codec
- **THEN** both reasons are stated

### Requirement: Some releases are never accepted
Samples, password-protected releases, releases with no readable title and releases that do not match the title asked for SHALL be rejected whatever the profile says. Cinema recordings and the like SHALL be accepted only when a profile's separate switch allows them. No search SHALL grab a release rejected by such a fixed rule. An administrator's grab by force, and nothing else, SHALL be able to take a release whose only fixed rules are about its name: it does not match the title asked for, it could be more than one title, its year differs, its indexer states another title's identifier, no title can be read from it, or no episode of the series' data can be read from it. A sample, a password-protected release, a name that ends in an archive's or a program's extension and a Usenet subject line for one part of an upload SHALL NOT be taken by anyone.

#### Scenario: Ambiguous match
- **WHEN** a release's match to the title is ambiguous
- **THEN** it is rejected

#### Scenario: Cinema recording
- **WHEN** a profile lists every quality as allowed but has not switched on unwanted sources
- **THEN** a cam release is rejected

#### Scenario: A rule about the name
- **WHEN** a stored release is rejected only because it could be more than one title
- **THEN** it may be grabbed by force by an administrator, and by no search

#### Scenario: A rule that protects the system
- **WHEN** a stored release is password-protected and also could be more than one title
- **THEN** it may not be grabbed by force

### Requirement: Accepted releases are ranked
Accepted releases SHALL be ranked by quality preference, then preference score, then revision, then the remaining tie-breakers, and the ranking SHALL be explainable.

#### Scenario: Quality first
- **WHEN** one accepted release has a higher quality and another a higher preference score
- **THEN** the higher quality ranks first

### Requirement: Upgrades stop at the cutoff
A release SHALL be an upgrade only when it is better than the file on disk by the profile's rules, and no upgrade SHALL be sought once the file on disk is at or above the cutoff.

#### Scenario: At the cutoff
- **WHEN** the file on disk is at the profile's cutoff
- **THEN** no release is an upgrade

#### Scenario: A file Axis converted
- **WHEN** the file on disk was converted by Axis from a Remux
- **THEN** it counts as a Remux, and a BluRay encode is not an upgrade

### Requirement: Profiles are edited and tried on a page
The daemon SHALL provide a page listing quality profiles, where administrators copy and edit them, and where a release name can be tried against a quality profile to see how it was read and decided. The trial SHALL be offered as "Try a release name".

#### Scenario: Try a release name
- **WHEN** a release name is tried against a quality profile
- **THEN** the page shows what the release name says and the decision with its reasons

#### Scenario: Built-in
- **WHEN** an administrator tries to change a built-in profile
- **THEN** it is refused, and copying it is offered

