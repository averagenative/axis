# requests Specification (delta)

## ADDED Requirements

### Requirement: A request names a person, a title, a kind and, for a series, seasons
A request SHALL record who asked, one title, its kind (movie or series) and, for a series, a list of season numbers. A person SHALL choose a series' seasons as the whole series, the first season, the latest season or picked seasons, and the daemon SHALL turn that choice into the season list when the request is made, from the seasons the title's record has then; season 0 (the specials) SHALL be in the list only when it is picked by number. A request SHALL NOT name an episode, and SHALL carry no text that the person typed. A request SHALL be made only for a kind whose module is switched on.

#### Scenario: A movie
- **WHEN** the requester `robin` asks for the movie "The Lantern Orchard" (2019)
- **THEN** one request exists for that title, of kind movie, asked by `robin`, with no seasons, in the state waiting

#### Scenario: The whole series becomes a season list
- **WHEN** `robin` asks for the whole of the series "Harbour of Glass", whose record has seasons 0, 1, 2 and 3
- **THEN** the request lists seasons 1, 2 and 3, and not season 0

#### Scenario: The first and the latest season
- **WHEN** one person asks for the first season of that series and another for the latest
- **THEN** the first request lists season 1 and the second lists season 3

#### Scenario: No episodes and nothing typed
- **WHEN** a request is posted with an episode number, with a note, or with a season the title's record does not have
- **THEN** it is refused and no request exists

#### Scenario: A module that is off
- **WHEN** the TV module is switched off and a series is asked for
- **THEN** it is refused and the answer says that series cannot be requested

### Requirement: Who may ask
An administrator SHALL create every account that may ask; there SHALL be no way to register. A requester and an ordinary user SHALL be able to make requests. In the engine's interface, an ordinary user SHALL be offered Request on a search result, a Discover card and a title's page where an administrator is offered Add, and SHALL still be refused every action that adds or changes a title directly.

#### Scenario: An ordinary user asks from Discover
- **WHEN** an ordinary user presses Request on a Discover card
- **THEN** a waiting request for that title exists in their name, and no movie was added

#### Scenario: An ordinary user still cannot add
- **WHEN** an ordinary user posts the form that adds a movie
- **THEN** the response is 403 and nothing is added

#### Scenario: A forged request post
- **WHEN** a Request post arrives without the session's CSRF token
- **THEN** the response is 403 and no request exists

### Requirement: A request waits for an administrator unless the person is approved automatically
A request SHALL wait until an administrator approves or declines it, and while it waits the daemon SHALL add nothing, search for nothing and contact no indexer or download client on its account. Approval SHALL add the title to its module (for a series, with the request's seasons monitored and no others changed) and start its search. A decline SHALL record a reason of at most 300 characters, written by the administrator, and MAY mark the title as closed to requests. A request SHALL be decided once: of two decisions made at the same moment, exactly one SHALL take effect.

#### Scenario: Waiting fetches nothing
- **WHEN** a request waits
- **THEN** no movie or series row exists for its title, no search was queued, and no request went to an indexer or a download client

#### Scenario: Approval adds and searches
- **WHEN** an administrator approves the request for "The Lantern Orchard"
- **THEN** the movie is in Movies with the default library folder and quality profile, a search for it is queued, and the request is approved

#### Scenario: Approval of seasons
- **WHEN** an administrator approves a request for seasons 2 and 3 of a series that Axis does not have
- **THEN** the series is added with seasons 2 and 3 monitored and season 1 not monitored

#### Scenario: Approval widens and never narrows
- **WHEN** Axis has a series with season 1 monitored and a request for season 2 is approved
- **THEN** seasons 1 and 2 are monitored, and the series' library folder and quality profile are unchanged

#### Scenario: A decline has a reason
- **WHEN** an administrator declines a request with the reason "Not for this library"
- **THEN** the request is declined, the person who asked sees that reason, and nothing was added

#### Scenario: Two decisions at once
- **WHEN** one administrator approves and another declines the same request at the same moment
- **THEN** one of the two is refused with a message that the request was already decided, and the request has one outcome

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator posts an approval or a decline
- **THEN** the response is 403 and the request still waits

### Requirement: The administrator's defaults place a title, never the person who asks
The library folder and the quality profile of a title that a request adds SHALL be the ones an administrator set as the defaults for requests of that kind, or the ones the approving administrator chooses on the approval. No field of a request SHALL choose either, and a request post that carries such a field SHALL be refused.

#### Scenario: Defaults are used
- **WHEN** the defaults for movie requests are the folder "Movies" and the profile "Balanced", and a movie request is approved without a change
- **THEN** the movie is added in that folder with that profile

#### Scenario: A request cannot choose
- **WHEN** a request post carries a library folder, a quality profile or a monitoring choice
- **THEN** it is refused and no request exists

#### Scenario: No defaults yet
- **WHEN** no default is set for series requests and an administrator opens a series request to approve it
- **THEN** the approval asks for the folder and the profile, and approves nothing until both are chosen

### Requirement: Automatic approval is two switches for each person, and is bounded
Each person SHALL have two switches, which start as the settings `requests.new_auto_movies` and `requests.new_auto_series` are when the account is created (both off by default; changing a setting changes no existing account): requests for movies are approved automatically, and requests for series are approved automatically. A request of a kind whose switch is on SHALL be approved when it is made, by the same code as an administrator's approval with the defaults, and SHALL be recorded as approved automatically. It SHALL wait instead when the defaults for its kind are not set or no longer exist, or when a series request lists more seasons than `requests.auto_max_seasons` (3 by default); the Requests page SHALL say why it waits.

#### Scenario: Off by default
- **WHEN** an administrator creates a requester and that requester asks for a movie
- **THEN** the request waits

#### Scenario: New accounts start with the installation's choice
- **WHEN** `requests.new_auto_movies` and `requests.new_auto_series` (both off by default) are switched on and an administrator then creates an account
- **THEN** the account starts with both switches on and its requests are approved automatically, within the usual bounds, while an account made before keeps the switches it had

#### Scenario: A movie approved automatically
- **WHEN** `robin` has the movies switch on and asks for a movie
- **THEN** the movie is added with the defaults, its search is queued, and the request says it was approved automatically

#### Scenario: One switch does not open the other
- **WHEN** `robin` has only the movies switch on and asks for a season of a series
- **THEN** the request waits

#### Scenario: Too many seasons for automatic approval
- **WHEN** `robin` has the series switch on and asks for the whole of a series with nine seasons
- **THEN** the request waits, and the Requests page says it lists more seasons than are approved automatically

#### Scenario: The defaults are gone
- **WHEN** the default library folder for movie requests was removed and `robin` asks for a movie
- **THEN** the request waits, and the Requests page says the default folder is missing

### Requirement: Each person has a quota for each kind over a calendar window
Requests SHALL be limited for each person and each kind over a calendar window that an administrator chooses for the installation: a day, a week (from Monday) or a month, in the server's local time. A movie request SHALL count as one, and a series request as one for each season it lists. Each person SHALL have the installation's limits (`requests.movies_per_window` and `requests.seasons_per_window`, 5 each by default) or limits of their own set by an administrator, where 0 means that the person cannot ask for that kind and cannot follow another person's request of that kind either. A request that would exceed what is left SHALL be refused whole, and the answer SHALL say how many are left and when the window ends. A withdrawn request SHALL give its count back; a declined request SHALL keep it. Following another person's request SHALL count nothing. Besides the quota, a person who is not an administrator SHALL make at most 100 requests in any 24 hours, whatever became of them: a withdrawal SHALL NOT give back a place under this ceiling, it SHALL NOT be a setting, and a request beyond it SHALL be refused with a reason of its own. The daemon SHALL count from its own records, whatever a client says, and SHALL show a person what is left in words such as "2 of 5 left this week". An administrator SHALL NOT be limited.

#### Scenario: The limit is reached
- **WHEN** the window is a week, the limit is 5 movies, and `robin` has asked for 5 movies since Monday
- **THEN** a sixth movie request is refused, saying none is left until Monday, and no request exists for it

#### Scenario: Seasons count one each
- **WHEN** `robin` has 3 seasons left and asks for a series' 4 seasons
- **THEN** the request is refused whole, saying 3 seasons are left, and no request exists

#### Scenario: What is left is shown
- **WHEN** `robin` has asked for 3 of 5 movies this week
- **THEN** the page shows "2 of 5 left this week" for movies

#### Scenario: A new window
- **WHEN** the next window begins
- **THEN** `robin` can ask again up to the limit

#### Scenario: Withdrawing gives the count back
- **WHEN** `robin` withdraws a waiting movie request made in this window
- **THEN** one more movie is left in this window

#### Scenario: A decline keeps the count
- **WHEN** one of `robin`'s requests of this window is declined
- **THEN** what is left is unchanged

#### Scenario: A limit of none
- **WHEN** an administrator sets `sam`'s limit for series to 0
- **THEN** `sam` is offered no series request, and one posted anyway is refused, also for seasons that another person asked for already

#### Scenario: Asking and withdrawing again and again
- **WHEN** `robin` has made 100 requests in the last 24 hours and withdrawn every one of them
- **THEN** the next request is refused, saying that 100 is the most in 24 hours, although the quota has room


### Requirement: Every title shows its state to everyone who can ask
For every title in a search result, on a Discover card and on a title's page, a person who can ask SHALL be shown one state: available, partly available, downloading, approved, requested, declined, closed to requests, or can be requested; and for a series, the same for each season. The state SHALL NOT be hidden from anyone who can ask. The name of the person who asked SHALL be shown beside a requested, approved or declined title while `requests.show_who_asked` is on (the default) and SHALL be shown to administrators always; while it is off, a person who is not an administrator SHALL be shown no other person's name. A decline's reason SHALL be shown only to the person who asked, to the request's followers and to administrators.

#### Scenario: The states of a search
- **WHEN** a search returns a movie in the library, a movie being downloaded, a movie that waits for approval, a declined movie and a movie nobody asked for
- **THEN** their cards read available, downloading, requested, declined and can be requested, for an administrator, an ordinary user and a requester alike

#### Scenario: Seasons of a series
- **WHEN** a series has season 1 with every aired episode on disk, season 2 with half of them, and season 3 requested and waiting
- **THEN** its page shows season 1 available, season 2 partly available and season 3 requested, and the series as partly available

#### Scenario: Who asked, shown
- **WHEN** `requests.show_who_asked` is on and `sam` looks at a title that `robin` asked for
- **THEN** the card says that `robin` asked for it

#### Scenario: Who asked, hidden
- **WHEN** `requests.show_who_asked` is off and `sam` looks at the same title
- **THEN** the card says requested, and nothing `sam` is sent names `robin`

#### Scenario: A reason is not for everyone
- **WHEN** `robin`'s request was declined with a reason and `sam`, who does not follow it, looks at the title
- **THEN** `sam` sees declined and not the reason

#### Scenario: Who asked, hidden, for another season
- **WHEN** `requests.show_who_asked` is off, `robin` has a request for season 1 of a series, and `sam` has one for season 2
- **THEN** nothing `sam` is sent about the series names `robin`

### Requirement: A second person follows the existing request
When a person asks for a movie that has an open request (waiting or approved), or for seasons that open requests already list, the daemon SHALL make them a follower of each such request, SHALL NOT refuse them and SHALL count nothing against their quota for what they follow. Seasons that are already available or already listed by an open request SHALL be left out of a new series request, and a request SHALL be made for the remaining seasons only; when no season remains, no request SHALL be made. A title or a season that is available SHALL be answered as available, with no request. A follower SHALL see the request's state among their own requests. The same person asking twice SHALL change nothing.

#### Scenario: Following a movie request
- **WHEN** `robin` has a waiting request for a movie and `sam` asks for the same movie
- **THEN** there is still one request, `sam` follows it, `sam`'s quota is unchanged, and the request is in `sam`'s list with its state

#### Scenario: Seasons are left out, not refused
- **WHEN** season 1 of a series is available, `robin` has an open request for season 2, and `sam` asks for the whole series of three seasons
- **THEN** `sam` has a new request that lists season 3 only, follows `robin`'s request, and is counted one season

#### Scenario: Nothing remains
- **WHEN** every season `sam` asks for is available or already requested
- **THEN** no request is made, `sam` follows the open requests, and the answer says what is available and what is already requested

#### Scenario: Already in the library
- **WHEN** a person asks for a movie that has a file in the library
- **THEN** the answer is that it is available, and no request exists

#### Scenario: Asked twice
- **WHEN** `robin` posts the same request twice
- **THEN** one request exists and it is counted once

#### Scenario: A title Axis is already looking for
- **WHEN** an administrator added a movie that has no file yet, and `robin` asks for it
- **THEN** `robin` has a request that is approved without waiting, because approving it would fetch nothing more, and it is counted against `robin`'s quota once

### Requirement: A declined title can be asked for again unless it is closed
A person SHALL be able to ask again for a title or seasons that were declined; the new request SHALL take the declined one's place among open requests, SHALL count against the quota like any request, and the Requests page SHALL say how often the title was declined before. An administrator SHALL be able to mark a title as closed to requests, on a decline or later, and to open it again. A request for a closed title SHALL be refused, with the state shown as closed to requests, and SHALL count nothing.

#### Scenario: Asked again
- **WHEN** `robin`'s request for a movie was declined and `robin` asks for it again
- **THEN** a new waiting request exists, the title's state is requested, and the Requests page says it was declined once before

#### Scenario: Closed to requests
- **WHEN** an administrator declines a request and ticks "Do not ask again", and `sam` then asks for that title
- **THEN** the request is refused, the title reads closed to requests, and `sam`'s quota is unchanged

#### Scenario: Opened again
- **WHEN** an administrator opens the title again
- **THEN** it can be requested

### Requirement: A person may withdraw their own waiting request, and change nothing else
A person SHALL be able to withdraw a request they made while it waits. A withdrawn request SHALL fetch nothing and SHALL give its count back. When the request has followers, the earliest follower SHALL become the person who asked, with the count taken from their quota only if they have it left; otherwise the request SHALL be withdrawn for all. A follower SHALL be able to stop following. Nothing else about a request SHALL be changeable by the person who asked, and a request that is no longer waiting SHALL NOT be withdrawn.

#### Scenario: Withdrawn while waiting
- **WHEN** `robin` withdraws a waiting request
- **THEN** it is withdrawn, the title can be requested again, and an administrator can no longer approve it

#### Scenario: Not after approval
- **WHEN** `robin` posts a withdrawal of a request that was approved
- **THEN** it is refused and the request is unchanged

#### Scenario: Not another person's
- **WHEN** `sam` posts a withdrawal of `robin`'s request
- **THEN** the answer is the same as for a request that does not exist, and the request is unchanged

#### Scenario: A follower takes it over
- **WHEN** `robin` withdraws a request that `sam` follows and `sam` has quota left
- **THEN** the request still waits, in `sam`'s name, and is counted against `sam`

### Requirement: A request's state is worked out from the engine's own records
The daemon SHALL store only what a person decided about a request (made, approved, declined, withdrawn, replaced by a newer one), and that the title an approved request added was removed from Axis since, and SHALL work out the rest when the request is read, from the records the engine already keeps of its title: downloading while a grab of the title or of one of the request's seasons is in flight; available when a movie has a file, or when every aired episode of the request's seasons has a file; partly available when some of those episodes have files and not all; failed otherwise where the engine's records say so. A failed request SHALL carry one cause from a fixed list, shown in plain words: the title could not be added; a download failed and Axis is looking again; a download finished and needs the administrator. No cause SHALL name a release, a file, an indexer, a download client or a path. Available SHALL mean that the file is in the library folder; it SHALL NOT wait for a media server's scan. A request whose title was added and then removed from Axis SHALL be closed: it SHALL read "Removed from Axis" to the person who asked, it SHALL no longer stand in the way of a new request for the same title or seasons, and it SHALL keep its count.

#### Scenario: Downloading
- **WHEN** an approved movie request's movie has a grab in flight
- **THEN** the request reads downloading

#### Scenario: A movie is available
- **WHEN** that download is imported
- **THEN** the request reads available, with no write to the request itself

#### Scenario: A series is partly available
- **WHEN** a request lists seasons 1 and 2, with 16 aired episodes, and 7 of them have files
- **THEN** the request reads partly available, 7 of 16 episodes

#### Scenario: A failure in plain words
- **WHEN** an approved movie's download failed and no other grab is in flight
- **THEN** the request reads failed with the cause "A download failed. Axis is looking again.", and nothing the person is sent names the release or its source

#### Scenario: An import that needs a person
- **WHEN** the movie's download finished and its import is blocked
- **THEN** the request reads failed with the cause that the download needs the administrator

#### Scenario: Removed
- **WHEN** an administrator removes the movie of an approved request before it had a file
- **THEN** the request reads "Removed from Axis", the movie can be requested again, and the quota the request used stays used

### Requirement: An approval that cannot add says so on both sides
When an approved request's title cannot be added (its library folder is gone, its module is off, the provider cannot be asked for the series' seasons), the request SHALL stay approved, SHALL read failed with the cause "could not be added" to the person who asked, and SHALL show the engine's own reason to administrators on the Requests page with a control to try again. An approval SHALL never remove what it added: a series approval that fails part of the way SHALL leave the series with the seasons monitored so far, and trying again SHALL complete it. Trying again SHALL be refused for a request whose title was added and is still in Axis. Two placings of the same title SHALL NOT run at the same time.

#### Scenario: The folder is gone
- **WHEN** a request is approved and the chosen library folder no longer exists at that moment
- **THEN** no title is added, the Requests page shows the reason and "Try again", and the person who asked sees that it could not be added

#### Scenario: Tried again
- **WHEN** the administrator chooses another folder and tries again
- **THEN** the title is added and its search is queued

#### Scenario: Two placings of one series
- **WHEN** two approved requests for different seasons of a series that Axis does not have are placed at the same moment, and one of them lists a season the series no longer has
- **THEN** the series is added once and stays, the other request's season is monitored, the failed request says why, and trying it again once the season exists monitors it on the same series

### Requirement: A person sees their own requests and no one else's list
The page "My requests" SHALL list the requests a person made and the ones they follow, the newest first, each with its title, its seasons, its state and, when declined, its reason. A person who is not an administrator SHALL NOT be able to read or change a request that they neither made nor follow, by any identifier, and such an attempt SHALL be answered as a request that does not exist. Requests SHALL be named outside the daemon by identifiers that cannot be guessed or counted.

#### Scenario: My requests
- **WHEN** `robin` made two requests and follows one of `sam`'s
- **THEN** `robin`'s list has those three and none of `sam`'s others

#### Scenario: Another person's request by its identifier
- **WHEN** `robin` asks for one of `sam`'s requests by its identifier
- **THEN** the answer is 404, the same as for an identifier that was never issued

#### Scenario: Nothing of the engine's inner workings
- **WHEN** an ordinary user or a requester reads their requests
- **THEN** nothing they are sent names a file, a path, a release, an indexer, a download client, a quality profile or a library folder

### Requirement: The Requests page
The engine's interface SHALL have a page "Requests" (`/requests`). To an administrator it SHALL list every request, the waiting ones first and the oldest of them first, each with who asked, how many follow it, its title and seasons, how many aired episodes an approval would look for, why it waits when it would otherwise have been approved automatically, and how often it was declined before; with controls to approve (with the folder and the profile, prefilled with the defaults), to decline with a reason, to close a title to requests and to open it again. To an ordinary user it SHALL be "My requests". A page of the Requests module for administrators, "People who ask" (`/requests/people`), linked from the Users page, SHALL show, for each person, the two switches and the limits.

#### Scenario: The waiting list
- **WHEN** three requests wait and ten were decided
- **THEN** the page lists the three first, the oldest at the top, each saying who asked and what an approval would look for

#### Scenario: An ordinary user
- **WHEN** an ordinary user opens `/requests`
- **THEN** they see their own requests and no control to approve or decline

#### Scenario: The switches and limits of a person
- **WHEN** an administrator follows the link on the Users page to `/requests/people` and sets `robin`'s movies switch on and the series limit to 2
- **THEN** both are stored and shown, and a change without the CSRF token is refused

### Requirement: An account that goes takes its waiting requests with it
When an account is disabled or deleted, the requests it made that still wait SHALL be withdrawn, or handed to their earliest follower as for a withdrawal, and its part in requests it follows SHALL end. Requests that were approved SHALL stay, and the titles they added SHALL be unchanged.

#### Scenario: A disabled requester
- **WHEN** an administrator disables `robin`, who has one waiting request and one approved
- **THEN** the waiting request is withdrawn and the approved one, with its movie, is unchanged

### Requirement: Two records of a title that merge keep one open request for each thing
When the metadata store merges two records of one title, the requests of the record that goes SHALL move to the record that is kept. Where both records have an open request for the movie or for the same season, one of the two SHALL stay for it: the approved one over one that waits, whichever record it is on, and otherwise the kept record's. The other SHALL lose that movie or season, and the person who made it and its followers SHALL follow the one that stays. A series request that loses some of its seasons SHALL stay open with the others; a request that loses everything SHALL be closed. Nobody SHALL lose sight of what they asked for.

#### Scenario: Two waiting requests for a movie
- **WHEN** `robin` and `sam` each have a waiting request for a movie on two records that merge, and `kim` follows `robin`'s
- **THEN** one request stays, and `robin` and `kim` follow it and see it among their requests

#### Scenario: The approved one stays
- **WHEN** the request on the record that goes is approved and the one on the record kept waits
- **THEN** the approved request stays, and the person who made the other follows it

#### Scenario: Seasons that are not shared
- **WHEN** `robin` waits for seasons 1, 2 and 3 on the record that goes, and the record kept has open requests for seasons 1 and 2
- **THEN** `robin`'s request stays open for season 3, and `robin` follows the requests that hold seasons 1 and 2
