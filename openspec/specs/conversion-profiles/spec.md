# conversion-profiles Specification

## Purpose
What a conversion aims for and who decided it: versioned, strictly validated profiles that can be copied, adjusted and assigned to libraries, the safety rules no profile can change, and the lock that keeps a definition from bulk use until it has been proven.
## Requirements
### Requirement: Profiles
The daemon SHALL provide conversion profiles: named definitions of a video or audio target with a quality rule, encoder preference, audio and subtitle handling, container, and the adjustable part of eligibility. Built-in profiles for each family SHALL always exist and SHALL NOT be editable; a user SHALL be able to copy any profile and edit the copy.

#### Scenario: Built-ins present
- **WHEN** the daemon starts on a new database
- **THEN** the built-in video and audio profiles exist

#### Scenario: Built-in cannot be edited
- **WHEN** a request tries to change a built-in profile
- **THEN** it is refused

#### Scenario: Copy
- **WHEN** an administrator copies a profile
- **THEN** a new editable profile exists with the same definition and its estimates equal the original's

### Requirement: Strict definitions
A profile definition SHALL be accepted only if every member is known, of the right type and within its bounds. A stored definition that fails validation SHALL make that revision unusable and SHALL be shown as such, never repaired silently.

#### Scenario: Out of bounds
- **WHEN** an administrator saves a quality level outside the bounds for the codec
- **THEN** the save is refused with the error beside that field

#### Scenario: Unknown member
- **WHEN** a definition contains a member the daemon does not define
- **THEN** it is refused

#### Scenario: Stored definition no longer valid
- **WHEN** a stored revision fails validation after an upgrade
- **THEN** it cannot be used for estimates or conversion and its page says why

### Requirement: Rules no profile can change
Whatever its definition, a profile SHALL NOT make eligible a file that is pinned, is Dolby Vision, has uncertain or unclassified HDR, is HDR10+, is or may be interlaced, is an HDR source for an SDR target, has facts missing or inconsistent, or is not estimated strictly smaller. The minimum saving SHALL NOT be set below the floor.

#### Scenario: No definition overrides the rules
- **WHEN** any valid definition is evaluated against a file one of these rules covers
- **THEN** the file is not eligible, with that rule's reason

#### Scenario: Minimum saving floor
- **WHEN** an administrator sets the minimum saving below the floor
- **THEN** the save is refused

### Requirement: Quality rule
A profile's quality level SHALL be chosen by an ordered list of conditions on resolution class and file size, first match first, with a required default.

#### Scenario: Large 4K
- **WHEN** the built-in Space saver profile is applied to a 2160p file of 60 GB
- **THEN** the quality level is the one for large 4K files

#### Scenario: Default
- **WHEN** no condition matches a file
- **THEN** the default quality level is used

### Requirement: Immutable revisions
Every change to a profile SHALL create a new revision and SHALL NOT alter an earlier one. Saving without a change SHALL create nothing. A revision that something refers to SHALL NOT be deleted; a profile SHALL be archivable.

#### Scenario: Edit makes a revision
- **WHEN** an administrator changes a profile
- **THEN** a new revision exists and the earlier revision is unchanged

#### Scenario: No change
- **WHEN** an administrator saves a profile without changing it
- **THEN** no revision is added

#### Scenario: Archive
- **WHEN** a profile is archived
- **THEN** it is no longer offered for assignment and its record remains

### Requirement: Locked until proven
A profile revision SHALL be locked for bulk use until it has been proven. A change that can alter the output SHALL produce a locked revision; a change that cannot SHALL carry the earlier proof forward. The page SHALL say, before saving, which kind a change is.

#### Scenario: Output-affecting change locks
- **WHEN** the quality rule of an unlocked profile is changed
- **THEN** the new revision is locked

#### Scenario: Other change keeps the proof
- **WHEN** only the name or the minimum saving of an unlocked profile is changed
- **THEN** the new revision is unlocked

#### Scenario: New profiles start locked
- **WHEN** a profile is created
- **THEN** its revision is locked and the page says nothing can be bulk-converted with it yet

### Requirement: Assignment to library roots
An administrator SHALL be able to assign at most one video profile and one audio profile to a library root. The assignment page SHALL show the estimated saving and the compatibility verdict for the pairing and SHALL require a second confirmation when the verdict is risky or unknown. Assigning SHALL NOT convert anything.

#### Scenario: Assign
- **WHEN** an administrator assigns a profile to a root
- **THEN** the root shows that profile, its estimated saving and its verdict, and no file is changed

#### Scenario: Risky or unknown
- **WHEN** the verdict for the pairing is risky or unknown
- **THEN** the assignment needs a second confirmation that states the verdict

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator tries to change a profile or an assignment
- **THEN** the response is 403 and nothing changes

### Requirement: Proven by hand
A revision SHALL be unlocked also by a proof of the method "trials", made only by an administrator's confirmed request (see conversion-execution) and recording who, when and which trials; the database SHALL refuse such a proof that names no trials, or trials that are not done, passed trials of that revision. Such a proof SHALL be carried forward by an edit that cannot change the output, as a canary proof is.

#### Scenario: Carried
- **WHEN** only the minimum saving of a revision proven by hand is changed
- **THEN** the new revision is unlocked

