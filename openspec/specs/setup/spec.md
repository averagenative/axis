# setup Specification

## Purpose
The optional setup guide: a resumable path through what Axis needs before it can download and import, using the real settings forms and the real checks.
## Requirements
### Requirement: An optional guide walks a new administrator through setup
The daemon SHALL offer an optional guide that takes an administrator through what Axis needs before it can download and import: a TMDb token, disks and library folders, an indexer source, a download client with its path mapping, a default quality profile, and the decision to allow writing. The guide SHALL be skippable and resumable, and every setting it touches SHALL remain editable in its usual place.

#### Scenario: Leave and return
- **WHEN** an administrator leaves the guide part way and opens it again later
- **THEN** finished steps are shown as done and the guide resumes at the first unfinished one

#### Scenario: Not used
- **WHEN** an administrator dismisses a step
- **THEN** the guide and the dashboard stop asking for it

### Requirement: A step is done only when its check passes
A step SHALL be shown as done only when the corresponding check of the real configuration passes.

#### Scenario: Wrong key
- **WHEN** an indexer key is entered that the indexer refuses
- **THEN** the step is not done and says the key was refused

### Requirement: Each step explains itself and links to what is needed
Each step SHALL say in plain words what the thing is and why Axis needs it, and SHALL link to where the required key or address is found.

#### Scenario: TMDb
- **WHEN** the titles step is opened
- **THEN** it says what TMDb is used for, which of TMDb's two values to paste, and links to TMDb's page for it

### Requirement: The guide shows the real forms and comes back only to itself
Each step SHALL show the form of the page the thing is configured on, built by that page's own code and posting to that page's own handler, so that every value has one place. After a successful post from the guide the handler SHALL send the browser back to the step; the address it goes back to SHALL be one of the guide's own, from a fixed list, and any other value SHALL be ignored.

#### Scenario: Both ways
- **WHEN** a value is set in the guide, or on its own page
- **THEN** the other shows the same value

#### Scenario: A foreign return address
- **WHEN** a form is posted with a return address that is not one of the guide's own (another site, another page of Axis, a path with "..", a query)
- **THEN** the handler goes where it always goes, and nothing of the request reaches the redirect

### Requirement: Administrators only, and no secret shown
The guide SHALL be for administrators only, and SHALL never put a stored key, token or password in a page. Links out of Axis SHALL open in a new tab without a referrer or an opener; a link to a service an administrator configured SHALL be built from its configured address and base path and a fixed path of Axis's only.

#### Scenario: Someone else
- **WHEN** a user who is not an administrator asks for the guide or one of its actions
- **THEN** the answer is 403, and their dashboard does not link to it

