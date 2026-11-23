# web-live Specification

## Purpose
How pages update without reloading: the event stream hub, who may subscribe to what, and the limits that keep it bounded.
## Requirements
### Requirement: Event streams require a session
The daemon SHALL serve event streams only to authenticated sessions and SHALL close a session's streams when that session ends.

#### Scenario: Unauthenticated stream refused
- **WHEN** a client without a session requests the event stream
- **THEN** the response is 401 and no stream is opened

#### Scenario: Logout closes streams
- **WHEN** a session with an open stream logs out or is revoked
- **THEN** the stream is closed by the daemon

### Requirement: Topic broadcast
The daemon SHALL deliver each published event to every stream subscribed to its topic and to no other stream.

#### Scenario: Subscribers receive their topic
- **WHEN** an event is published on a topic with two subscribed streams and one stream subscribed elsewhere
- **THEN** the two subscribers receive it and the third does not

### Requirement: Bounded streams
The daemon SHALL limit open streams per session, per account over all its sessions, and in total, SHALL keep a part of the total that only administrators may use, SHALL drop a stream whose client stops reading, and SHALL send periodic keep-alives. The cap per account SHALL be small enough that no account, nor five of them, can fill the part of the total that the others may use. At the per-account cap a new stream SHALL be refused, no stream of the account being closed for it: the refused request SHALL be answered as a stream that says, in the page's status line, that live updates are off in that tab, asks for a retry after a minute, and ends. A page that opens a stream SHALL carry that status line, hidden, and a stream that is served SHALL hide it.

#### Scenario: Per-session cap
- **WHEN** a session opens more streams than the per-session limit
- **THEN** its oldest stream is closed and the newest is served

#### Scenario: Per-account cap
- **WHEN** an account with two sessions holds as many streams as the per-account limit and opens one more
- **THEN** the new one is told that live updates are off in its tab and to retry after a minute, and ends; none of the account's streams is closed, nor any other account's; once one of the account's streams ends, the next is served and hides the line

#### Scenario: The status line
- **WHEN** a page that opens a stream is served
- **THEN** it carries the hidden status line the refusal fills in

#### Scenario: Part of the total is kept for administrators
- **WHEN** ordinary accounts hold every stream they may use and another ordinary account asks for one
- **THEN** that request is answered 503 with a retry time, and an administrator's request is still served until the whole total is used

#### Scenario: The defaults
- **WHEN** the hub uses its default limits
- **THEN** five accounts at the per-account cap hold less than the part of the total ordinary accounts may use, and that part is less than the total

#### Scenario: Stalled client is dropped
- **WHEN** a client stops reading its stream while events continue
- **THEN** the daemon closes that stream and memory use does not keep growing

#### Scenario: Keep-alive
- **WHEN** a stream has no events for longer than the keep-alive interval
- **THEN** the daemon sends a comment line on it

### Requirement: Administrator-only topics
The daemon SHALL refuse a subscription to a topic whose name ends in `:admin` unless the session belongs to an administrator at the time of the request, and SHALL close every stream of an account whose role changes, so that a stream opened again is judged by the role the account has then.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests a stream that includes a topic ending in `:admin`
- **THEN** the response is 403 and no stream is opened

#### Scenario: Administrator served
- **WHEN** an administrator requests a stream for a topic ending in `:admin`
- **THEN** the stream is opened

#### Scenario: A demoted administrator
- **WHEN** an administrator with a stream for a topic ending in `:admin` and another stream is made an ordinary user
- **THEN** both streams are closed, nothing published on the `:admin` topic reaches the account, a new request for it is answered 403, and a stream for another topic is served

#### Scenario: A promoted user
- **WHEN** an ordinary user with an open stream is made an administrator
- **THEN** that stream is closed, a new one may include a topic ending in `:admin`, and other accounts' streams stay open

### Requirement: Pages follow the work they show
When a job with a de-duplication key changes, the shell SHALL publish the event "activity" on the topic of its key's family ("act:" and the key up to its first number: "act:tv.search:12" for "tv.search:12:season:1"); when a download changes, on its title's topic ("act:title:7"); and for either on "act:all". It SHALL publish only to topics someone follows, at most once a topic per batch of changes (at most twice a second), so a page with many rows costs one request per change, never one per row. A page that shows that work SHALL follow those topics and fetch its own status lines when told.

#### Scenario: Another series' page
- **WHEN** a job keyed "tv.search:7:season:1" starts
- **THEN** a stream following "act:tv.search:7" and one following "act:all" hear "activity", and one following "act:tv.search:8" hears nothing

### Requirement: Pages are told when the notices change
When a reading of the notices finds that a notice came, went, or changed its level or its number, and when a user dismisses a notice or shows one again, the shell SHALL publish the event "attention" on the topic "attention:admin", only if someone follows it, and at most once for a reading. Only an administrator SHALL be able to follow the topic: while every notice is an administrator's, even that something changed is theirs to know. The event SHALL carry no notice, count, name or path: what a user sees depends on who they are, so each page SHALL ask for its own attention control when told, and the page "Needs attention" SHALL ask for its own list. A page that shows the control SHALL follow the topic on the one stream it already opens, and SHALL be complete without the stream.

#### Scenario: A new notice
- **WHEN** an import is blocked while an administrator has a page of Movies open
- **THEN** within the time of a reading the page hears "attention", asks for its control, and the control shows one more

#### Scenario: Nothing changed
- **WHEN** a reading finds the same notices as the one before
- **THEN** nothing is published

#### Scenario: Nothing in the event
- **WHEN** the event is published
- **THEN** its data names no disk, title, file or path

#### Scenario: An ordinary user asks for the topic
- **WHEN** a user who is not an administrator asks to follow "attention:admin"
- **THEN** the stream is refused (403), and they hear nothing when the notices change

#### Scenario: Another tab
- **WHEN** an administrator dismisses a notice in one tab
- **THEN** their other open pages show the lower count without being loaded again

#### Scenario: Without the stream
- **WHEN** a page is loaded in a browser that runs no script
- **THEN** the control shows the count as it was when the page was drawn, and its link opens the page "Needs attention"

