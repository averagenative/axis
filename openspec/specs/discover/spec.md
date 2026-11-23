# discover Specification

## Purpose
Define the Discover section for movies and series: lists and recommendations kept as a snapshot by a job, shown with the state of each title in the library, with filters, and with a way to hide titles you are not interested in.
## Requirements
### Requirement: Discover lists are kept and refreshed by a job
Discover SHALL keep, for movies and for series, a snapshot of each list the metadata provider offers (popular, trending this week, now, coming soon, top rated), up to `discover.pages` pages of 20 titles, and SHALL refresh a snapshot with the `discover.refresh` job only once it is older than `discover.refresh_after`. A snapshot SHALL be replaced only when every page asked for arrived; a failure SHALL keep the snapshot shown and record the problem and when it happened.

#### Scenario: A list is fetched once
- **WHEN** the job runs twice within `discover.refresh_after`
- **THEN** the provider is asked for each list's pages at the first run only

#### Scenario: A stale list is fetched again
- **WHEN** the job runs after a snapshot has become older than `discover.refresh_after`
- **THEN** that list's pages are asked for again and the snapshot replaced

#### Scenario: Fetching now
- **WHEN** an administrator chooses "Fetch the lists now"
- **THEN** a refresh is queued that asks for every list again, however recently it was fetched

#### Scenario: A failure keeps what was there
- **WHEN** the provider answers a list's second page with an error
- **THEN** the list keeps its earlier snapshot, records the problem and the time, and the page shows both

#### Scenario: A provider that stops answering
- **WHEN** the provider is unavailable or rate limited during a run
- **THEN** the run asks it nothing more and leaves the rest for the next run

### Requirement: Pages never ask the provider
A Discover page SHALL be drawn from the stored snapshots only. When no snapshot of a kind has ever been stored and a provider can answer, a page view SHALL queue the refresh job (one at a time) and say that the lists are being fetched.

#### Scenario: Viewing a page
- **WHEN** someone opens a Discover page any number of times
- **THEN** the provider receives no request from those views

#### Scenario: The first view
- **WHEN** the first Discover page is opened with a TMDb token set and nothing stored
- **THEN** one refresh job is queued, and the page says the lists are being fetched

### Requirement: Recommendations come from what you have
The Recommended list SHALL be made from the provider's recommendations for the titles some module refers to (managed movies and series, titles identified on disk), fetched by the job for at most `discover.recommend_per_run` of them a run (never fetched first, then the oldest) and kept a week; it SHALL rank each recommended title by how many of those titles recommend it, SHALL never include a title you have or one hidden from Discover, and SHALL say on each card which of your titles recommend it. Recommendations of a title you no longer have SHALL be dropped.

#### Scenario: Ranked by agreement
- **WHEN** two of your movies both recommend one title and one recommends another
- **THEN** the first is listed before the second and its card says it is because you have both

#### Scenario: What you have is not recommended
- **WHEN** a recommended title is one of your managed movies
- **THEN** it is not in the Recommended list

#### Scenario: Turned off
- **WHEN** `discover.recommend_per_run` is 0
- **THEN** the job asks for no recommendations

### Requirement: Every card shows its state here
A Discover card SHALL show the title's poster, year, rating (or that it has none yet) and overview, and the state the Movies or TV module gives it (in the library, wanted, searching, downloading and the rest) through the card actions.

#### Scenario: A wanted movie
- **WHEN** a movie on the Popular list is on the wanted list
- **THEN** its card shows the Movies module's Wanted mark and no Add button

### Requirement: Filters and paging over the snapshot
A Discover page SHALL filter the stored entries of the chosen list by genre, a year range, a minimum rating, and "Hide what I have or want", and SHALL show 20 titles a page with a count of those that match that names what they are, the snapshot's size stated.

#### Scenario: By genre and rating
- **WHEN** a list is filtered by a genre and a rating of 7 and up
- **THEN** only stored entries with that genre number and a rating of at least 7.0 are shown, in the list's order

#### Scenario: Hiding what you have
- **WHEN** "Hide what I have or want" is ticked
- **THEN** no title that a module refers to is shown

#### Scenario: Adding with the hide ticked
- **WHEN** a card's Add is pressed while "Hide what I have or want" is ticked
- **THEN** the page comes back at the next card (the block of the cards when it was the last), and the notice there names the title added instead of pointing at its card

#### Scenario: Paging
- **WHEN** a list of movies holds 45 matching titles
- **THEN** the page shows 20, says "Showing 1 to 20 of 45 movies.", and links to the next page with "Next page"

### Requirement: Not interested is remembered
An administrator SHALL be able to hide a title from Discover with the "Hide from Discover" button on its card; it SHALL then be left out of every Discover list, recommendations included, and listed on the page "Hidden from Discover" (in the navigation, Not interested), where an administrator can show it again. Hiding SHALL be kept when two records of the title merge.

#### Scenario: Hiding a title
- **WHEN** an administrator presses "Hide from Discover" on a card
- **THEN** it is in no Discover list, and the page "Hidden from Discover" lists it with who hid it and when

#### Scenario: Showing it again
- **WHEN** an administrator chooses "Show in Discover again" for it
- **THEN** it is back in the lists that hold it

#### Scenario: Not for everyone
- **WHEN** a user who is not an administrator posts the hiding of a title
- **THEN** the answer is 403 and nothing changes

### Requirement: Without a key Discover says so
When no details provider that offers lists has its token, a Discover page SHALL say that Discover needs a TMDb token and where an administrator sets it, SHALL show any snapshot stored before with its date, and the job SHALL ask nothing.

#### Scenario: No key
- **WHEN** a Discover page is opened and no token is set
- **THEN** it says a TMDb token is needed, links an administrator to Settings with "Add a TMDb token in Settings", and no request is made

### Requirement: Discover sits under Media beside Movies and TV
The Media tab SHALL have a Discover section with the pages Discover movies, Discover TV and Not interested; every page SHALL open with a sentence saying what it is for, and name the provider whose data it shows as its terms require. Anyone logged in may look; acting SHALL be for administrators.

#### Scenario: The section
- **WHEN** a logged-in user opens the Media tab
- **THEN** its sections are Movies, TV and Discover, and Discover's pages are Discover movies, Discover TV and Not interested

### Requirement: Discover is a list on a phone
On a screen at most 40rem wide a Discover page's cards SHALL be rows like the search results (a small poster beside the title, its year and rating, the start of its overview), each with its state or its one-step add, its "Add with options" link and its "Hide from Discover" button in reach; and its filters SHALL take two fields to a line.

#### Scenario: Browsing on a phone
- **WHEN** a Discover list is shown at 390 pixels
- **THEN** each title is one row with its poster, and a title not added shows "Add to Movies" or "Add to TV", "Add with options" and "Hide from Discover", each at least 44 pixels high

### Requirement: Hide unreleased
The Discover movies and Discover TV filters SHALL include a tick box "Hide unreleased". With it ticked, a movie SHALL be left out when the Movies module would call it "Not available yet" (its first release date, plus the wait the movies settings give for a movie that counts as released, is ahead; or it has no date and its status is not released), and a series SHALL be left out when its first air date is ahead, or it has none and is neither continuing nor ended. The box SHALL apply to every list, including the recommendations, SHALL be applied in the statement that reads a page, so that pages stay full and the counts are of what remains, and SHALL be carried by the list tabs, the paging links and the way back after Add or Hide from Discover. It SHALL NOT be remembered between visits. When it empties Coming soon, the page SHALL say that Hide unreleased hides those titles.

#### Scenario: Unreleased movies are left out
- **WHEN** a list holds a movie whose release date is ahead and a movie with no date that is not released, and Hide unreleased is ticked
- **THEN** neither is shown, and the count is of the movies that remain

#### Scenario: A page stays full
- **WHEN** a list holds 25 movies of which five on its first page are unreleased, and Hide unreleased is ticked
- **THEN** the first page shows 20 movies and there is no second page

#### Scenario: Unreleased series are left out
- **WHEN** a series has a first air date ahead, or no date and is not yet airing, and Hide unreleased is ticked
- **THEN** it is not shown

#### Scenario: Add keeps the box
- **WHEN** a movie is added from a page with Hide unreleased ticked
- **THEN** the person comes back to the same place with the box still ticked

#### Scenario: Coming soon
- **WHEN** Hide unreleased is ticked on Coming soon and nothing is left
- **THEN** the empty state says that Hide unreleased hides the titles of this list

### Requirement: More like this on a title's page
A movie's or a series' own page SHALL show a card "More like this" with up to 12 of the titles the provider recommends for it, in the provider's order, drawn as Discover's cards are: each with its state from the Movies or TV module when you have it, or the one-step Add and "Add with options" when you do not. Titles hidden from Discover SHALL be left out; titles you have and unreleased titles SHALL be shown. When the title's recommendations were fetched within the week they are kept, the card SHALL be drawn from the store and the provider SHALL receive no request. Otherwise the page SHALL be drawn without waiting for the provider, and the card SHALL fetch the recommendations of that one title (one request), store them as the refresh job does, and then show them. A failed attempt SHALL be said on the card, SHALL leave what was stored before in place, and SHALL NOT be repeated within the hour. The card SHALL NOT depend on `discover.recommend_per_run`. A title with no recommendations SHALL say so. The provider SHALL be asked with its own id of the title: the record's own, or the one a record of another provider keeps; a title with no such id SHALL have no card. A managed series' episodes page SHALL show the same card, loaded by the card itself. An Add on the card SHALL come back to the page the card is on, at that card.

#### Scenario: Stored recommendations
- **WHEN** a movie's page is opened and its recommendations were fetched within the week
- **THEN** the card shows them, and the provider receives no request for them

#### Scenario: Already in the library
- **WHEN** one of the recommended movies is in the library
- **THEN** its card shows the Movies module's mark, no Add button, and links to its page

#### Scenario: Not fetched yet
- **WHEN** a movie's page is opened and its recommendations are not stored, or are older than a week
- **THEN** the page is drawn at once with a card that says Axis is asking TMDb, the card's own request asks the provider once and shows the titles, and the next view asks nothing

#### Scenario: Nothing recommended
- **WHEN** the provider recommends nothing for the movie
- **THEN** the card says so

#### Scenario: No id of the provider's
- **WHEN** the page is of a title whose record is another provider's and keeps no id of the provider's
- **THEN** there is no card and the provider receives no request

#### Scenario: Another provider's record with the id
- **WHEN** the page is of a series whose record is TheTVDB's and keeps its TMDb id
- **THEN** TMDb is asked with that id and the card shows what it recommends

#### Scenario: A series
- **WHEN** a series' title page or its episodes page is opened
- **THEN** the card shows the series the provider recommends for it, with the TV module's mark on those you have and Add to TV on the others

#### Scenario: Add from a series' episodes page
- **WHEN** an administrator selects Add to TV on a card of the episodes page
- **THEN** the series is added and the browser comes back to the episodes page at that card, where the notice names it

#### Scenario: Recommended for you turned off
- **WHEN** `discover.recommend_per_run` is 0
- **THEN** the card still fetches and shows the title's recommendations

#### Scenario: Add from the card
- **WHEN** an administrator selects Add to Movies on one of the cards
- **THEN** the movie is added and the browser comes back to the same page at that card, where the notice names it

