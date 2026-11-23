# indexers Specification

## Purpose
How Axis finds releases: searching configured indexer sources, keeping what they return for a short time, and fetching a release only from the source that offered it.
## Requirements
### Requirement: Releases are found through configured sources
The daemon SHALL search configured indexer sources for releases of a film or an episode, using identifiers where the source supports them and text otherwise, and SHALL return each release with its name, size, protocol, indexer and age.

#### Scenario: Search
- **WHEN** a film is searched for with a source configured
- **THEN** its releases are returned with name, size, protocol and indexer

#### Scenario: Some indexers fail
- **WHEN** some of a source's indexers do not answer
- **THEN** the releases from the others are returned and the failures are stated

### Requirement: A release is fetched only from its source
A release's file or magnet link SHALL be fetched only from the configured address of the source that returned it.

#### Scenario: Address elsewhere
- **WHEN** a result names a fetch address that is not the source's own
- **THEN** that release is not offered

#### Scenario: Redirect elsewhere
- **WHEN** fetching a release is redirected to another web address
- **THEN** the fetch fails and nothing is sent there

#### Scenario: Not a release file
- **WHEN** the fetched content is neither an NZB nor a torrent
- **THEN** it is refused

### Requirement: Source keys stay with their source
A source's key SHALL be sent only to that source's address and SHALL NOT appear in any page, log line or stored job, including inside a release's fetch address.

#### Scenario: Key in a fetch address
- **WHEN** releases are listed on a page
- **THEN** no fetch address is shown

### Requirement: Searches are limited and reused
Searches SHALL be limited per source, and a repeated search within the cache life SHALL be answered from the stored result.

#### Scenario: Repeat
- **WHEN** the same search is made twice within the cache life
- **THEN** the source is asked once

### Requirement: Release names are never markup
Text from an indexer SHALL be shown as text.

#### Scenario: Markup in a name
- **WHEN** a release name contains markup
- **THEN** it is displayed literally

### Requirement: Indexers page
The daemon SHALL provide a page for administrators showing each source, its state, the indexers it reports and a test search.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page
- **THEN** the response is 403

### Requirement: A release offered as a link is kept
A result whose only fetch address is a link on the source's own address SHALL be kept, and fetching it SHALL yield the magnet link the source redirects to, without following it.

#### Scenario: Link address
- **WHEN** a torrent result gives a link on the source's own address and no file address
- **THEN** the release is offered, and fetching it gives its magnet link

#### Scenario: Wrong torrent
- **WHEN** the magnet the source redirects to names a different torrent than the result stated
- **THEN** the fetch is refused

### Requirement: An indexer's download redirect is followed once, with nothing of Axis's, unless switched off
When a source redirects a release's download to the indexer's own address, the daemon SHALL follow that redirect unless the administrator has switched following off for the source, only once, only to a public address over TLS, and without sending any secret, cookie or header of the source. The redirect address SHALL NOT be stored or logged.

#### Scenario: Followed
- **WHEN** following is on, as it is by default, and the source redirects a download to the indexer
- **THEN** the release file is fetched from the indexer and checked like any other

#### Scenario: Switched off
- **WHEN** following has been switched off for the source
- **THEN** the fetch fails saying the indexer redirects downloads and how to resolve it, and nothing is sent to the indexer

#### Scenario: Inside the network
- **WHEN** a redirect points at a loopback, private or link-local address
- **THEN** it is refused

### Requirement: Indexers that redirect are shown
The Indexers page SHALL show, for each indexer of a source, whether its downloads come through the source or redirect to the indexer.

#### Scenario: Redirecting indexer
- **WHEN** an enabled indexer redirects downloads and following is switched off
- **THEN** the source's page says so

### Requirement: An identifier search does not lose indexers that only take text
When a request carries identifiers and some enabled indexer does not accept them, the daemon SHALL also search those indexers by title and merge the answers.

#### Scenario: Mixed indexers
- **WHEN** one indexer accepts the identifier and another accepts only text
- **THEN** releases from both are returned, each once

### Requirement: Indexers can be used directly
The daemon SHALL let an administrator add Newznab and Torznab indexers with their own address and key and SHALL search and fetch from them without any other program.

#### Scenario: Usenet indexer
- **WHEN** a Newznab indexer is added and a movie is searched for
- **THEN** its releases are returned, searched by identifier when the indexer accepts one

#### Scenario: Without Prowlarr
- **WHEN** only native indexers are configured
- **THEN** movies and TV searches work

### Requirement: Prowlarr remains an optional source
A Prowlarr source SHALL keep working beside native indexers, and an indexer reachable both ways SHALL be asked once.

#### Scenario: Both
- **WHEN** an indexer is configured natively and also in a connected Prowlarr
- **THEN** it is searched once

### Requirement: A search asks every indexer under its own limits
A search SHALL ask every enabled indexer and source concurrently, each within its own rate and allowance, and one indexer failing or being slow SHALL NOT fail or delay the others beyond its deadline.

#### Scenario: One indexer down
- **WHEN** one indexer does not answer
- **THEN** the releases of the others are returned and the failure is stated

#### Scenario: Allowance
- **WHEN** an indexer reports that its daily allowance is nearly used
- **THEN** automatic searches stop using it and the page says so

### Requirement: Indexer answers are read defensively
An indexer's answer SHALL be read with entity definitions, external references and oversized or over-deep documents refused.

#### Scenario: Entity definitions
- **WHEN** an answer defines entities
- **THEN** it is refused

### Requirement: An indexer's key and fetch addresses are never shown
An indexer's key, and any address that contains it, SHALL NOT appear in any page, log line, stored job or message.

#### Scenario: Failure message
- **WHEN** a request to an indexer fails
- **THEN** the message names the indexer and not its address's key

### Requirement: Sites with their own API are indexers
The daemon SHALL provide native indexers for YTS, EZTV and The Pirate Bay, each with a configurable address, returning releases as magnet links built from the listed info-hash. The Pirate Bay SHALL be asked for a movie by its IMDb identifier when the request has one, and by its title and year when that finds nothing; for an episode or a season by words in its TV categories; and for typed words in its video categories. Each Pirate Bay result's category SHALL be mapped to movies or TV, a result in a category that is not video SHALL be left out, and a VIP or trusted uploader SHALL be shown as a flag.

#### Scenario: Movie from YTS
- **WHEN** a movie is searched for with YTS enabled
- **THEN** its YTS releases are returned with their info-hashes

#### Scenario: Site moved
- **WHEN** a site's address stops answering
- **THEN** that indexer is shown as failing and the other indexers are unaffected

#### Scenario: A movie at The Pirate Bay by its identifier
- **WHEN** a movie with an IMDb identifier is searched for at The Pirate Bay
- **THEN** the site is asked for the identifier in its movie categories, and for the title and year only when that finds nothing

#### Scenario: A VIP upload
- **WHEN** The Pirate Bay lists a result whose uploader is VIP
- **THEN** the release carries the flag "VIP" and the movie category it was listed in

### Requirement: A Newznab or Torznab search reads further pages
The daemon SHALL read further pages of a Newznab or Torznab search, stepping the `offset` by the results each page held, while the indexer's stated total is not reached (or, without one, while a page is full), up to five pages and the result cap, listing a result on two pages once and respecting the indexer's limiter between pages.

#### Scenario: More results than a page
- **WHEN** an indexer says it has more results than its page holds
- **THEN** the next pages are read and their results listed once each

#### Scenario: A later page fails
- **WHEN** a page after the first cannot be read
- **THEN** what was read is kept and the result says it was cut

### Requirement: An API under a path is reached through the base path
The daemon SHALL ask a Newznab or Torznab indexer at its base path followed by `/api`, so that Jackett's feeds and an API at `/api/v1/api` work, and SHALL say so with those examples where the path is typed.

#### Scenario: Jackett feed
- **WHEN** the base path is `/api/v2.0/indexers/NAME/results/torznab`
- **THEN** requests go to that path and `/api` on the indexer's own origin

### Requirement: Counts are written with thousands separators
The Indexers pages SHALL write counts of releases, indexers, files and calls with a comma between thousands, through one shared formatter, without localisation.

#### Scenario: A large count
- **WHEN** a page shows 1234 releases
- **THEN** it reads "1,234 releases"

### Requirement: What a person adds is an indexer
Everywhere a person reads it (pages, settings, readiness check, setup guide and the errors they show), what a person adds, whether an indexer, a site or Prowlarr, SHALL be called an indexer.

#### Scenario: Nothing set up
- **WHEN** no indexer is added
- **THEN** the readiness check says "No indexer is set up"

### Requirement: A typed text search does not blame an indexer that takes no words
A search of all indexers by words as typed SHALL NOT show a problem for an indexer that cannot be asked by words; nothing is asked of it, and it is shown as not asked ("it cannot be searched by words"), not as one that answered, and that indexer's own test search says why.

#### Scenario: EZTV and words
- **WHEN** words are searched for among several indexers including EZTV
- **THEN** EZTV shows no problem and no releases, and is shown as not asked

### Requirement: The test search shows the identifiers on request
The test-search form SHALL show the title or words and what to look for, and put the identifiers, year, season and episode in a group that is closed unless one holds a value, without script.

#### Scenario: A fresh form
- **WHEN** the Indexers page is opened
- **THEN** the identifiers are in a closed group named "Identifiers and episode (optional)"

### Requirement: An indexer with nothing for a search is shown as not asked
An indexer that has nothing for the kind of search made (a TV site for a movie, a movie site for an episode, a definition covering neither) SHALL NOT be asked, SHALL be shown in a search of every indexer as "Not asked: it has no movies" or "Not asked: it has no TV" with no "Searched by" and no count, SHALL NOT be counted among the indexers that answered, and SHALL leave nothing stored for the search. The movie and TV pages' last-search line SHALL say it was not asked, not that it found no releases. An indexer that was asked and found nothing SHALL still be shown as answered with none found.

#### Scenario: A movie search and a TV site
- **WHEN** a movie is searched among several indexers including EZTV
- **THEN** EZTV is shown as "Not asked: it has no movies", with no request made to it and no count

#### Scenario: Asked and nothing found
- **WHEN** an indexer is asked and finds no release
- **THEN** it is shown as answered, with none found

### Requirement: An indexer added both natively and through Prowlarr is asked once, directly
A search SHALL ask an indexer once when it is added natively and Prowlarr lists it too: the native one is asked, Prowlarr is asked without that indexer, and the page SHALL say "<name> is asked directly, not through <source>". Prowlarr's indexer is the native one when its host (where Prowlarr gives an address) is the native indexer's host, else when its name, made plain, is the native indexer's own name made plain or the name its site goes by (`newsowl` for `api.newsowl.example`). An indexer of another name that Prowlarr gives no address for SHALL NOT be taken for the native one.

#### Scenario: Prowlarr gives no address
- **WHEN** NewsOwl and DozyNewt are added natively and Prowlarr lists indexers of those names with no address
- **THEN** both are asked directly, Prowlarr is asked for its other indexers only, and the page says each is asked directly

#### Scenario: Another name
- **WHEN** a native indexer has a name Prowlarr's indexers do not and Prowlarr gives no addresses
- **THEN** Prowlarr is asked for all of its indexers, as before

### Requirement: A stored address is kept when a kind's default changes
An indexer SHALL keep the address it was stored with when the built-in default address of its kind later changes; the default is used only when no address is typed.

#### Scenario: An old default
- **WHEN** an indexer was stored at a kind's earlier default address and the default is now another
- **THEN** its stored address is unchanged when it is read, listed or saved again

### Requirement: Built-in site definitions
The daemon SHALL ship site definitions it maintains and tests for TorrentProject, LimeTorrents, TheRARBG and Torrents-CSV, compiled into it, with ids beginning with `axis-`. Each SHALL be offered on the Indexers page with one sentence saying what the site is and what it can be searched by, and SHALL be added with no file to install and no key; nothing SHALL be asked of a site until an administrator adds it. At every start each built-in definition's stored copy SHALL be made the one this daemon ships.

#### Scenario: Offered
- **WHEN** the Indexers page is opened on a fresh installation
- **THEN** TorrentProject, LimeTorrents, TheRARBG and Torrents-CSV are offered to add, and no request is made to any of them

#### Scenario: An older copy after a restore
- **WHEN** the daemon starts on a database whose built-in definition is an older copy
- **THEN** the stored copy is replaced by the one this daemon ships, and the indexers that run it run that one

#### Scenario: The RARBG mirror
- **WHEN** an indexer runs TheRARBG at `rarbg.torrentsbay.org` and the site answers with a browser challenge
- **THEN** the indexer is shown as failing with the words that the site answers with a browser challenge Axis cannot pass

#### Scenario: A movie known only by its identifier
- **WHEN** a movie with an IMDb identifier and no title is searched for at a definition-driven indexer that is not searched by that identifier
- **THEN** nothing is asked of the site, and the search says it is searched by words and this search has none

### Requirement: A site that asks Axis to wait is waited for
When a definition-driven indexer answers 429, or 503 with Retry-After, the daemon SHALL hold it until the time asked, in words that say the site asked Axis to wait. Searches, tests and checks SHALL NOT ask it before then, and a test or a check SHALL say until when; a passing test or check, or a new version of its definition, SHALL lift only the holds Axis set for its pages or a browser challenge, never the site's. The Indexers page SHALL word each hold by its cause.

#### Scenario: Asked to wait
- **WHEN** a built-in site answers a search with 429 and Retry-After: 600, and an administrator then tests and checks it
- **THEN** neither asks the site, each says it asked Axis to wait and until when, and the hold stays

### Requirement: A browser challenge is told by what only a challenge has
A browser challenge SHALL be recognised by the challenge's own marks (Cloudflare's challenge options object, its orchestration script, the challenge page's title, the `cf-mitigated: challenge` header), never by a script or a link that ordinary pages also carry.

#### Scenario: The detection script
- **WHEN** a site's ordinary page carries Cloudflare's detection script, in either of its two forms
- **THEN** the page is read as the page it is

### Requirement: A built-in definition can be overridden by a newer one
An administrator SHALL be able to override a built-in definition with a newer version of it, pasted on its page, uploaded as a definition file, or placed in the data directory as `definitions/<id>.yml`. An override SHALL be accepted only when it is a usable definition with the built-in one's id, a version higher than the built-in one's, and requests that can be built; a file in the data directory SHALL be taken at start only when its version is also higher than the stored override's (or the stored one cannot be run), and SHALL be refused by its size before it is read. The definition's page SHALL say which version is in use, built in or overridden, and where it came from (the data directory's file or the page); an override no newer than the built-in copy SHALL be ignored and the page SHALL say so. Removing the override SHALL return to the built-in copy.

#### Scenario: A newer override
- **WHEN** an administrator pastes a usable definition with the built-in id and a higher version
- **THEN** the indexers that run it run the override, and the page says the override is in use

#### Scenario: Not newer
- **WHEN** the pasted definition's version is not higher than the built-in one's
- **THEN** it is refused, saying which version is built in

#### Scenario: Ignored after an upgrade
- **WHEN** the daemon is upgraded to one whose built-in copy is as new as the stored override or newer
- **THEN** the built-in copy is used and the page says the override is ignored

#### Scenario: An older file in the data directory
- **WHEN** the daemon starts with a file in the data directory whose version is lower than the override pasted on the page
- **THEN** the pasted override stays in use, and the log says why the file was not taken

### Requirement: Pages that no longer look as expected are not an empty answer
When a definition names the part every answer of its site has, the daemon SHALL tell an answer with no results apart from one whose pages no longer look as expected: the part is missing, the answer is not of the kind the definition reads, or more than half of at least two results lack what every result must have. Such an answer, and a browser challenge, SHALL fail the search with a plain sentence naming the indexer, set its state to failing with that sentence, and keep searches from asking it for an hour; a test or a check that passes SHALL end that. An indexer that is not working SHALL be a notice under "Needs attention", as the attention capability defines.

#### Scenario: Changed layout
- **WHEN** a built-in site answers a page without the part every answer has
- **THEN** the indexer is failing with "its pages no longer look as expected", is not searched for an hour, and is listed on the dashboard

#### Scenario: Nothing found
- **WHEN** the site's answer has that part and no results
- **THEN** the search is answered with no releases and the indexer is working

### Requirement: Results that are not video are left out
A result whose site category maps only to software, music, books or games SHALL be left out of every search.

#### Scenario: Music in a feed
- **WHEN** a site's answer lists a result in its Music category
- **THEN** that result is not among the releases

### Requirement: A definition-driven indexer can be checked step by step
The daemon SHALL give administrators, for each indexer that runs a definition, a check that makes one search with the words typed, through the indexer's own address and limits, and shows the request made, the status and size of each answer, how many rows the row selector matched, and for the first rows what each field's selector read or that it matched nothing, and why a row was left out. Nothing of the answer SHALL be shown raw unless an excerpt is asked for, which SHALL be bounded; everything shown SHALL be text.

#### Scenario: A field that matches nothing
- **WHEN** an administrator checks an indexer whose seeders selector matches nothing
- **THEN** the page shows, for the first rows, that the seeders field matched nothing

#### Scenario: Markup in an answer
- **WHEN** a checked answer contains markup in a release name
- **THEN** the check page shows it as text

### Requirement: A definition can be checked from the command line
`axisd --check-definition` SHALL read a definition file (or a built-in definition by its id) and print the same step-by-step account against a saved page, asking nothing, or against the live site with the words given, one search, with no database and no daemon running.

#### Scenario: A saved page
- **WHEN** it is run with a definition and a saved page
- **THEN** it prints the rows matched and each field's reading, and makes no request

### Requirement: Sites without an API can be indexers through installed definitions
The daemon SHALL be able to search sites described by installed indexer definition files in the community format, and SHALL use a definition only when every feature it needs is supported.

#### Scenario: Supported definition
- **WHEN** a supported definition is installed and enabled
- **THEN** searching returns that site's releases

#### Scenario: Unsupported feature
- **WHEN** a definition needs a feature the engine lacks
- **THEN** the indexer is shown as not usable, naming the feature

#### Scenario: A site that needs a login
- **WHEN** a definition describes a login
- **THEN** it can be installed, and it is shown as not usable because logins are not supported

### Requirement: A definition cannot reach outside its site
A definition-driven indexer SHALL request only the addresses its definition names for the site.

#### Scenario: Elsewhere
- **WHEN** a definition or a page would lead to another address
- **THEN** the request is not made

### Requirement: Definitions are installed by the administrator
The daemon SHALL ship only its built-in site definitions, the small set it maintains and tests, and SHALL install or update any other definition only when an administrator asks. Each installed definition SHALL be validated when it is loaded, and one that does not validate SHALL NOT be used.

#### Scenario: Installed from a file
- **WHEN** an administrator uploads a definition file
- **THEN** it is validated and listed with whether it can be used, and no indexer is added or searched until the administrator adds one from it

#### Scenario: A file that is not a definition
- **WHEN** an administrator uploads a file that does not validate
- **THEN** it is refused with the reason, and the definitions installed before are unchanged

### Requirement: An administrator can list Jackett's site definitions
The daemon SHALL let an administrator ask for the list of the site definition files in Jackett's repository, and SHALL ask only then: no request for it SHALL be made at start, by a job, on a schedule or by opening a page. The list SHALL be read at one commit of the repository and kept with that commit and the time it was read, in place of the list before. It SHALL show each file's name, whether a definition of that id is installed and whether the installed copy differs, and SHALL be searchable by name. Only a name of lower-case letters, digits and hyphens followed by `.yml` SHALL be taken; any other entry SHALL be left out and counted. When the service refuses for its rate limit, the page SHALL say so in a plain sentence and the list before SHALL be kept. The page SHALL name the licence Jackett is published under and link to it. Every action here SHALL be an administrator's, checked for the form's token.

#### Scenario: The list is asked for
- **WHEN** an administrator selects See what Jackett has
- **THEN** the daemon asks for the branch's commit and for the folder's files at that commit, keeps the list with that commit and the time, shows the files, and fetches no file

#### Scenario: Nothing at start
- **WHEN** the daemon starts and an administrator opens the Indexers page and the fetch page
- **THEN** no request has been made to the service

#### Scenario: Rate limited
- **WHEN** the service answers that too many requests were made
- **THEN** the page says so plainly with when to try again, and the list kept is unchanged

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks for the list
- **THEN** it is refused and no request is made to the service

### Requirement: Fetched definitions are the listed files, judged like any other
The daemon SHALL fetch only the files an administrator ticked, at most 50 for one request and each at most 256 KiB, each at the commit of the list kept. A file's name SHALL be taken only in the accepted form and only when it is in the list kept; it SHALL never be used as a path. A file whose bytes do not have the git blob id the list gave SHALL NOT be installed. Each file SHALL be judged by the same loader, with the same limits, as a definition installed by hand, and one that fails SHALL NOT be installed and SHALL be shown with its reason. A definition whose id is not its file's name, or whose id is a built-in definition's, SHALL be refused. An installed file SHALL be stored where definitions installed by hand are, with its origin recorded: Jackett, the file's name, the commit, the file's hash and the time. Installing SHALL NOT add, switch on or search an indexer. Requests SHALL go only to the service's two fixed hosts over https, and SHALL NOT follow a redirect to another host.

#### Scenario: Fetched at the listed commit
- **WHEN** the repository's branch has moved on since the list was read and an administrator fetches a ticked file
- **THEN** the file installed is the one of the listed commit, and its origin names that commit

#### Scenario: Changed since the listing
- **WHEN** the bytes that arrive for a file are not the listed file's
- **THEN** nothing is installed for it, and the page says the file is not the one that was listed

#### Scenario: A name that is not a file name
- **WHEN** a form names a file with a path separator, an upper-case letter, another extension, or a name that is not in the list kept
- **THEN** that name is refused with a reason, and no request is made for it

#### Scenario: Not a definition
- **WHEN** a fetched file is not a definition the loader takes
- **THEN** it is not installed, and the page gives the loader's reason

#### Scenario: Nothing is switched on
- **WHEN** a definition has been fetched and installed
- **THEN** no indexer exists for it until an administrator adds one from its page

### Requirement: Fetched definitions are updated only when asked, and an update can be undone
The daemon SHALL NOT update a fetched definition by itself. An administrator SHALL be able to check for newer versions, which reads the list again and marks each fetched definition whose file changed there, and to update one definition or the ticked ones, with the same checks as a fetch. The file an update replaces SHALL be kept, and an administrator SHALL be able to put it back once, with its origin. A definition installed by hand, or changed by hand since it was fetched, SHALL NOT be replaced by a fetch unless the administrator ticks that it may be, and SHALL be said to have been left as it is otherwise.

#### Scenario: A newer version
- **WHEN** a fetched definition's file has changed at Jackett and an administrator checks for newer versions
- **THEN** the definition is marked as newer at Jackett, and its file on this server is unchanged

#### Scenario: Update and put back
- **WHEN** an administrator updates that definition and then puts the earlier version back
- **THEN** the newer file is in use after the update, the earlier file with its own commit is in use after it is put back, and there is nothing further to put back

#### Scenario: Changed by hand
- **WHEN** a file was installed by hand over a fetched definition and an administrator updates it without the tick
- **THEN** the definition is left as it is and the page says why; with the tick it is replaced, and the file changed by hand can be put back

### Requirement: An episode can be asked for by its absolute number, as words
A search for an episode SHALL be able to ask for the episode by its absolute number. Besides the request by season and episode, the indexers SHALL then be asked by the words of the series' title followed by the number, written with two digits, or with three when any episode of the series has an absolute number of 100 or more (a number of four digits is written whole). Where the series has a second name to ask by, the same words SHALL be asked with that name too, and with no more names than two. Each set of words SHALL be a text request of its own, made after the request by season and episode and only when that request could be made, so that it is limited, held, kept in the search cache and passed over near an indexer's allowance exactly as any request is. The answers SHALL be read as one list in the order asked, and a release that several requests return SHALL be listed once. An indexer that takes no words SHALL be shown as not asked for that request, and a words request that no indexer takes SHALL be said in the search's record as not asked. A words request that fails SHALL be counted among the requests that did not answer, and what the other requests found SHALL stand. When a search reads at most a fixed number of releases, each of its requests SHALL have an equal share of that number first, and what the shares leave SHALL go to the requests in their order. A search that carries no absolute number SHALL make no request of words.

#### Scenario: The words
- **WHEN** an episode of *Tenkuu Rangers* is searched for with season 6, episode 12 and the absolute number 137
- **THEN** the indexers are asked for season 6 episode 12, and each indexer that can be searched by words is also asked for the words `Tenkuu Rangers 137`

#### Scenario: Padded
- **WHEN** the absolute number is 5 and an episode of the series has the absolute number 100 or more
- **THEN** the words end in `005`

#### Scenario: Found twice
- **WHEN** the request by season and the request by words both return the same release
- **THEN** it is listed once

#### Scenario: No absolute number
- **WHEN** an episode is searched for without an absolute number
- **THEN** only the request by season and episode is sent, and no words are asked

### Requirement: An anime search includes the anime category
A request marked as being for an anime series, words included, SHALL ask in the anime category (5070) as well as in the categories it would ask in for TV: a Newznab or Torznab indexer in its own TV categories with 5070 added once after them, unless no room is left for another category; Prowlarr in 5000 and 5070; and a site run by a definition in the site's categories mapped under 5000, which take in those under 5070. The Pirate Bay, which has no anime category, SHALL be asked as for any series, and for such words in its video categories. EZTV, which is searched by identifier only, and YTS, which has no TV, SHALL NOT be asked for such words. A request for a film SHALL ask in a film's categories whether it is marked or not. A marked request SHALL have a cache key of its own, so that it is never answered from what was kept for the same request unmarked. A request that is not marked SHALL ask in the categories it asks in when no series is marked, and SHALL keep the cache key it has then.

#### Scenario: A Newznab indexer
- **WHEN** an anime search is sent to a Newznab indexer whose TV categories are 5000
- **THEN** the request names 5000 and 5070

#### Scenario: A standard series
- **WHEN** a search for a series that is not anime is sent
- **THEN** its categories are the indexer's TV categories alone, and its cache key is the one it has when no series is marked

#### Scenario: Kept apart in the cache
- **WHEN** an episode is asked for as an anime series' request after the same episode was asked for unmarked
- **THEN** the anime request is sent to the indexers, and is not answered from what was kept of the unmarked one

