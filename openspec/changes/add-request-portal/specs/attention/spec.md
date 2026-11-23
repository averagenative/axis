# attention Specification (delta)

## ADDED Requirements

### Requirement: Requests have notices
Requests that wait for an administrator SHALL be one notice under "Waiting for you", with their number and since when the oldest waits, that links to the Requests page; it SHALL be a notice at once and SHALL be gone when none waits. An approved request whose title could not be added SHALL be a notice under "Waiting for you" that links to the Requests page. While the portal API's daily ceiling for requests, or its hourly ceiling for failed logins, has been reached within the last 24 hours, there SHALL be one notice under "Worth a look" that says which limit was reached and how often, and that links to the page "Request portal"; it SHALL name no account. Like every notice, these SHALL be an administrator's: a person who is not an administrator SHALL see none of them.

#### Scenario: Requests wait
- **WHEN** three requests wait and the oldest was made two hours ago
- **THEN** one notice under "Waiting for you" says that 3 requests wait for approval, the oldest for 2 hours, and links to the Requests page

#### Scenario: One request
- **WHEN** one request waits
- **THEN** the notice reads "1 request waits for approval"

#### Scenario: Decided
- **WHEN** the administrator approves or declines every waiting request
- **THEN** the notice is gone without anyone touching it

#### Scenario: Approved automatically
- **WHEN** a request is approved automatically
- **THEN** it was never part of the notice

#### Scenario: Could not be added
- **WHEN** an approved request's title could not be added
- **THEN** a notice under "Waiting for you" says that 1 approved request could not be added, and links to the Requests page

#### Scenario: The portal reached a limit
- **WHEN** the portal API's ceiling for failed logins was reached twice today
- **THEN** a notice under "Worth a look" says so and links to the page "Request portal", and it names no username

#### Scenario: Not an administrator
- **WHEN** requests wait and an ordinary user opens any page
- **THEN** they are shown no attention control and no count
