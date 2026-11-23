# metadata Specification

## Purpose
How Axis learns what films and series exist: metadata providers behind one interface, a local store that pages and jobs read, refresh within each provider's limits, and artwork fetched once and served by Axis itself.
## Requirements
### Requirement: Titles come from providers through one interface
The daemon SHALL look up films and series through metadata providers behind one interface offering search by name, a title's details, a series' seasons and episodes, and lookup by an external identifier. A title SHALL be identified by its external identifiers, so that the same title found in different ways is one record.

#### Scenario: One record
- **WHEN** a title is found by search and later by its identifier
- **THEN** one stored title exists

#### Scenario: Fetched again
- **WHEN** a stored title is fetched again
- **THEN** its record is updated in place

### Requirement: Everything used is stored, and reads come from the store
Every provider answer the daemon uses SHALL be stored. Pages and jobs SHALL read stored data, which SHALL state its age when older than its freshness limit, and SHALL keep working when a provider is unreachable.

#### Scenario: Provider unreachable
- **WHEN** a provider does not answer and a stored answer exists
- **THEN** the stored answer is shown with the time it was fetched

#### Scenario: Search while the provider is down
- **WHEN** a search is made that was made before and the provider does not answer
- **THEN** the saved results are shown with a notice, not an error

#### Scenario: Malformed answer
- **WHEN** a provider returns something the daemon cannot read
- **THEN** the request fails and stored data is unchanged

### Requirement: Refresh within limits
Stored data SHALL be refreshed on a schedule according to how likely it is to change, only for titles something refers to, and within each provider's rate limit. A provider that asks the daemon to wait SHALL be left alone for that time without delaying other providers.

#### Scenario: Rate limit
- **WHEN** many lookups are requested at once
- **THEN** requests to a provider do not exceed its configured rate

#### Scenario: Told to wait
- **WHEN** a provider answers that the daemon is sending too much
- **THEN** no request is sent to that provider until the stated time, and other providers are unaffected

#### Scenario: Unreferenced titles
- **WHEN** a stored title is not wanted, not in the library and not recently viewed
- **THEN** the schedule does not refresh it

### Requirement: Provider keys are secret and stay with their provider
A provider's key SHALL be a secret setting, SHALL be sent only to that provider's own address, and SHALL NOT appear in any page, log line or stored job.

#### Scenario: Key stays put
- **WHEN** a provider is configured and used, including when requests fail or are redirected
- **THEN** its key reaches no other address and appears in no page or log

### Requirement: Only the lookup leaves the machine
A request to a provider SHALL carry only the search text or a title identifier, the key and a fixed user agent.

#### Scenario: What is sent
- **WHEN** lookups are made
- **THEN** no request contains a user name, a file name or anything about the library

### Requirement: Artwork is fetched once and served by Axis
Artwork SHALL be fetched from the provider's declared image address, accepted only if it is a JPEG, PNG or WebP image within the size and dimension limits, stored on disk, and served by the daemon to logged-in users. Pages SHALL NOT load images from third parties.

#### Scenario: Served locally
- **WHEN** a page shows a poster
- **THEN** the image address is the daemon's own

#### Scenario: Not an image
- **WHEN** the fetched file is not one of the accepted image types, whatever its declared type
- **THEN** it is discarded

#### Scenario: Session required
- **WHEN** a client without a session requests an artwork address
- **THEN** it is refused

#### Scenario: Cache bounded
- **WHEN** stored artwork exceeds its total limit
- **THEN** the least recently used is removed until it fits

### Requirement: Attribution
Each provider's required notice SHALL be shown wherever its data is displayed.

#### Scenario: Notice shown
- **WHEN** search results from a provider are displayed
- **THEN** that provider's notice is on the page

### Requirement: Metadata page
The daemon SHALL provide a page for administrators, in System and named "Movie and TV details", showing each details provider, whether its key or token is set, its state and limit, the size of stored data and artwork, and a test lookup.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page
- **THEN** the response is 403

#### Scenario: The page's name
- **WHEN** an administrator opens System
- **THEN** its row lists the page as "Movie and TV details", and the page has that heading and browser title

### Requirement: TMDb provider
The daemon SHALL provide TMDb as a metadata provider for films and series: search, details with alternative titles and external identifiers, seasons and episodes, lookup by IMDb or TVDB identifier, and posters and backdrops. It SHALL contact only TMDb's data and image addresses, which SHALL be fixed and never taken from a response.

#### Scenario: Search
- **WHEN** a film is searched for by name with a token configured
- **THEN** results are returned with title, year, overview and a poster

#### Scenario: One record across lookups
- **WHEN** a title is found by search and then by its IMDb identifier
- **THEN** one stored title exists, carrying its TMDb and IMDb identifiers

#### Scenario: Address in a response
- **WHEN** a response names a different address for images
- **THEN** it is ignored

#### Scenario: Unreadable fields
- **WHEN** a response has missing or wrong-typed fields
- **THEN** those fields are left empty and the rest is used

### Requirement: The TMDb token travels in a header to TMDb's data address only
The token SHALL be sent only as a request header to TMDb's data address, SHALL NOT appear in any requested URL, and SHALL NOT be sent with image requests.

#### Scenario: Not in a URL
- **WHEN** any TMDb request is made
- **THEN** the requested URL does not contain the token

#### Scenario: Images
- **WHEN** an image is fetched
- **THEN** the request carries no token

### Requirement: TMDb notice
Wherever TMDb data is displayed the daemon SHALL show the notice that it uses the TMDb API and is not endorsed or certified by TMDb.

#### Scenario: Notice
- **WHEN** TMDb search results are displayed
- **THEN** the notice is on the page

### Requirement: A provider may offer lists of titles
A metadata provider MAY offer lists of titles for a kind (popular, trending this week, now, coming soon, top rated, and the recommendations for one title) and the names of its genres. A list SHALL be asked for through the metadata layer like every other question (the provider's own origin, the key in its header, its rate limit and back-off), each title of it SHALL be checked and stored as a brief record, and each entry SHALL carry the provider's rating in tenths of a point out of ten (none for a title without votes), its vote count and at most 20 genre numbers. An answer that cannot be read, has more than 50 entries or holds a title that fails the checks SHALL be refused whole, keeping nothing of it.

#### Scenario: A list stored
- **WHEN** a page of a list is asked for
- **THEN** every title in it is stored as a brief record and each entry carries the stored title's id, its rating, votes and genre numbers

#### Scenario: A list not offered
- **WHEN** a list the provider does not offer for a kind is asked for
- **THEN** the question fails as invalid and nothing is sent

#### Scenario: An unreadable answer
- **WHEN** a list's answer is not an object or lacks its list of results
- **THEN** the question fails as unreadable and nothing is stored

### Requirement: TMDb lists
The TMDb provider SHALL offer, for movies, `/3/movie/popular`, `/3/trending/movie/week`, `/3/movie/now_playing`, `/3/movie/upcoming`, `/3/movie/top_rated` and `/3/movie/{id}/recommendations`, and for series `/3/tv/popular`, `/3/trending/tv/week`, `/3/tv/on_the_air`, `/3/tv/top_rated` and `/3/tv/{id}/recommendations`, with the language setting and the page, and the region setting on the movie lists other than trending and recommendations; and genre names from `/3/genre/movie/list` and `/3/genre/tv/list`. Adult titles SHALL be dropped unless the adult setting is on; an entry of the other kind (a trending answer's `media_type`) SHALL be dropped.

#### Scenario: Coming soon in the region
- **WHEN** the Coming soon list of movies is asked for with the region GB
- **THEN** the request is `/3/movie/upcoming` with `region=GB`, the language and the page

#### Scenario: No vote, no rating
- **WHEN** an entry has a vote count of 0
- **THEN** it has no rating

### Requirement: A search's answer does not replace a full record's year and dates
When a search's answer holds a title whose full record is stored, the stored year, release date and first air date SHALL stay the full record's. A title with no full record stored SHALL take them from the search's answer.

#### Scenario: Another year in a search
- **WHEN** a film's full record gives 2014 and a later search's answer gives 2015 for it
- **THEN** the stored film keeps 2014 and its release date

#### Scenario: No full record yet
- **WHEN** a search's answer holds a film never fetched in full
- **THEN** the film is stored with the search's year

### Requirement: The second page of a search
The metadata layer SHALL offer the second page of a provider's answer to a search, asked for as a question of its own, through the provider's own origin, key, rate limit and back-off like every other question, and kept apart from the first page. No page after the second SHALL be asked for.

#### Scenario: The second page
- **WHEN** the second page of a search is asked for
- **THEN** the provider is asked for page 2 of that search, and the answer is stored apart from the first page's

#### Scenario: A third page
- **WHEN** a third page is asked for
- **THEN** the question fails as invalid and nothing is sent

### Requirement: A film's year in the region is its theatrical one
A film's full record SHALL keep, besides the year of its first release, the year it came out in cinemas in the region setting (a theatrical or limited theatrical release) when that year is within two years after the first release's year and differs from it. A release on disc, on television or by streaming SHALL NOT give a year. The stored release date SHALL stay the earliest release in the region, as before.

#### Scenario: A disc decades later
- **WHEN** a film first released in 1975 came out in the region only on disc, in 2009
- **THEN** it keeps no year in the region, and 1975 is its year

#### Scenario: In cinemas a year later
- **WHEN** a film first released in 1975 came out in cinemas in the region in 1976
- **THEN** 1976 is kept as its year in the region

#### Scenario: In cinemas three years later
- **WHEN** a film came out in cinemas in the region three years after its first release
- **THEN** it keeps no year in the region

### Requirement: The comparison form of stored names is made again at start
When an upgrade empties the comparison form of stored titles' names (because the way names are compared changed), the daemon SHALL make it again for every such title when it starts, after the migrations and before identification or a search reads it.

#### Scenario: Emptied by an upgrade
- **WHEN** the daemon starts after a migration emptied the comparison form of some titles' names
- **THEN** every one of them has it again, in the new form, before the first identification run

### Requirement: A poster a card waits for is fetched when it asks
A placeholder that asks for its poster (`/search/poster`) SHALL, on its first ask, have the image fetched when it is neither kept nor refused and a stored title shows it as its poster or backdrop, through the same fetch as any artwork (the provider's image address, its rate limit, the size and dimension limits, one fetch however many ask), without waiting for it; its later asks SHALL only look. An address no stored title shows SHALL never be fetched. Once the provider has refused the image, the answer SHALL be the titled placeholder with no further request.

#### Scenario: A candidate's poster on a review card
- **WHEN** the Identify page shows a candidate whose poster is not kept, and its placeholder asks for it
- **THEN** the image is fetched from the provider's image address once, kept, and the next ask answers the image served by Axis

#### Scenario: The provider does not have it
- **WHEN** the image address answers that there is no such image
- **THEN** the next ask answers the placeholder with the title set in it and nothing that asks again, and the image host is asked once

#### Scenario: An address no title shows
- **WHEN** a request asks for an image no stored title shows
- **THEN** nothing is fetched, and the placeholder is answered

### Requirement: TMDb's movie search gives the first release's year
The TMDb provider SHALL search for movies without the region setting, so that each answer's release date, and the year taken from it, is the film's primary release date and not its date in the region. The region SHALL still be sent where its dates are wanted: a film's full record (its earliest release in the region and its theatrical year there) and the movie lists that are regional. After an upgrade from a build that searched with the region, every stored answer of a TMDb movie search SHALL be dropped, so that the next search of a name asks TMDb again and its answer's year replaces the region's on every title with no full record stored.

#### Scenario: A film first released abroad
- **WHEN** a film was first released abroad in 1988 and in the region in 2005, and it is searched for by name
- **THEN** the search sent has no region, the answer's film is of 1988, and its full record keeps 2005 as its release date in the region

#### Scenario: The region setting
- **WHEN** the region setting is GB and a film is searched for
- **THEN** the search sent has no region, and the film's full record still takes its release date in GB

#### Scenario: Stored answers from before
- **WHEN** the store holds TMDb's answers of movie searches made with the region, and the build that sends none starts
- **THEN** those answers are dropped, TMDb's series searches and finds and every other provider's lookups are kept, and the next search of each name is asked again

### Requirement: TheTVDB provider
The daemon SHALL provide TheTVDB as a secondary metadata provider for series only. It SHALL never be the provider a kind is searched or identified by, and SHALL be asked only where a caller names it. It SHALL read a series' full record, with its other names, its name and overview in the language setting (`metadata.tvdb_language`), its IMDb and TMDb identifiers and the episode orders it has, and every episode of one order. Its answers SHALL add to a series Axis already holds and SHALL NOT make a title of its own: an answer about a series no stored title holds SHALL NOT be kept. It SHALL contact only TheTVDB's API address and its one image address, both fixed and never taken from a response, at a rate the setting `metadata.tvdb_rate` limits, and SHALL pause when TheTVDB says to wait. It SHALL be offered only while its key is set.

#### Scenario: Every episode of an order
- **WHEN** the aired order of a series is asked for and TheTVDB answers it in three pages
- **THEN** all three pages are read and stored as that order's seasons and episodes, and no stored TMDb season of the series changes

#### Scenario: A series Axis does not hold
- **WHEN** TheTVDB answers about a series that no stored title holds
- **THEN** nothing of the answer is stored

#### Scenario: A film
- **WHEN** a film is looked up through TheTVDB
- **THEN** the question fails as invalid and nothing is sent

#### Scenario: An image on another host
- **WHEN** an answer gives an image address on a host other than TheTVDB's image address
- **THEN** the image is not kept and nothing is fetched from that host

#### Scenario: No key
- **WHEN** TheTVDB has no key
- **THEN** no page offers it as a source, and no request is sent to it

### Requirement: TheTVDB's key is exchanged for a token held in memory
The daemon SHALL send TheTVDB's key, and its PIN when one is set, only in the body of a login request to TheTVDB's API address. The key and the PIN SHALL be kept as secret settings. The token the login returns SHALL be kept in memory only, sent only as a bearer token in a request header to that address, renewed after 25 days, and SHALL NOT be stored, logged or shown. A request refused as unauthorised SHALL lead to one new login and one more try. A refused login SHALL mark the key refused and send nothing more to TheTVDB until the key or PIN changes.

#### Scenario: Nothing kept
- **WHEN** series are fetched from TheTVDB
- **THEN** neither the key nor the token appears in the database, a log line, a page, a stored job or any requested URL

#### Scenario: Expired token
- **WHEN** a request is answered 401 with a token that worked before
- **THEN** Axis logs in once more and asks again once, and the answer is used

#### Scenario: Key refused
- **WHEN** the login is refused
- **THEN** the Metadata page shows TheTVDB's key as refused, and no request is sent to TheTVDB until the key or PIN is changed

#### Scenario: Testing the key
- **WHEN** an administrator uses "Test TheTVDB key" on the Metadata page
- **THEN** Axis logs in and asks for the list of episode orders, says what came back, and sends nothing about any title or the library

### Requirement: Seasons and episodes are kept per source
The store SHALL keep each source's seasons and episodes of a series apart: TMDb's, and TheTVDB's aired order. A title SHALL stay one record with the identifiers of both providers. A title's name, overview, year and artwork SHALL come from the provider that made its record, and another provider's answer SHALL add identifiers, other names and its own source's seasons without changing them. A provider's statement about another provider's identifier SHALL NOT merge two stored titles that each hold their own provider's record.

#### Scenario: One title, two sources
- **WHEN** TMDb's record of a series names TheTVDB id 81001 and TheTVDB's record of 81001 is fetched
- **THEN** one stored series holds both identifiers, with TMDb's seasons and TheTVDB's aired order stored apart

#### Scenario: The other provider's answer
- **WHEN** TheTVDB answers for a series whose record TMDb made
- **THEN** the series keeps TMDb's name, overview, year and poster

#### Scenario: Identifiers that disagree
- **WHEN** TheTVDB's record names a TMDb series whose own record names a different TheTVDB series
- **THEN** no stored titles are merged, and the series' settings card shows the disagreement

### Requirement: TheTVDB notice
While TheTVDB has a key, or any series follows its aired order, every page's footer SHALL show "Metadata provided by TheTVDB. Please consider adding missing information or subscribing." with a direct link to TheTVDB, beside TMDb's notice. The Metadata page's TheTVDB card, the page that changes a series' numbering, and every page showing TheTVDB's seasons or episodes SHALL show it beside that data. The notice SHALL be text and a link, with no logo.

#### Scenario: Every page
- **WHEN** TheTVDB has a key and any page is opened
- **THEN** the page shows TheTVDB's notice with its link

#### Scenario: Beside the data
- **WHEN** the page of a series that follows TheTVDB's aired order is opened
- **THEN** TheTVDB's notice and link are shown beside its seasons

#### Scenario: No key, no notice
- **WHEN** TheTVDB has no key and no stored series follows it
- **THEN** no page shows TheTVDB's notice

