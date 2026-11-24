# Tasks: a request portal

Every scenario in the spec deltas has a task for its test, named by the test's path and file. Where it can be done, see the test fail with its check switched off before the slice is merged. The four gates pass on the final code of each slice.

Test programs: `tests/test-requests.c` (the model, with a database), `tests/test-web-requests.c` (the engine's pages), `tests/test-portal-api.c` (the portal API, with a database), `tests/test-portal.c` (the portal's pages against a stand-in daemon), `tests/test-portal-process.c` (the real `axis-portal` against a real daemon), `tests/check-portal-link.sh`, and the portal's part of `tests/check-container.sh` and `tests/check-examples.sh`.

## 1. P1: requests inside the engine

Build:

- [x] 1.1 Migration `0083_requests.sql` (0080 to 0082 were taken by then): `request`, `request_season`, `request_follower`, `request_closed_title`, the partial unique indexes for one open request, the two switches and two limits on `app_user`; after the first review also `made_by` with its index (the daily ceiling), the decision `removed` and the trigger that sets it, and a merge trigger that keeps people and seasons. Tested in `tests/test-db-migrate.c` on a database with accounts (`/db/migrate/requests-on-accounts`).
- [x] 1.2 `src/requests/`: the model, the season expansion, the duplicate and follower rules, the quota count from rows, the decision transaction, the derived state in a bounded number of statements for a page (8 for 20 titles, 5 for 50 requests, measured by the test; not one statement: the movies' and the series' facts are read by their own modules' statements).
- [x] 1.3 The approval path: add or find the title, monitor the request's seasons, record, queue the search; "Try again" for an approval that could not add. The decision is one transaction; the placing of the title comes after its commit, because `axis_movies_add()` and `axis_tv_add()` use connections of their own. Two placings of one title wait for each other, and a placing never removes what it added; "Try again" is refused once the title is added and in Axis (see `design.md`, "As built in P1").
- [x] 1.4 Settings: `requests.window`, `requests.movies_per_window`, `requests.seasons_per_window`, `requests.auto_max_seasons`, `requests.show_who_asked`, and the four defaults for placement (set on the Requests page, where the folders and profiles are listed by name).
- [x] 1.5 Pages: `/requests` for administrators and as "My requests", `/requests/<identifier>`, Request on search results, Discover cards and title pages, the state on every card. Two things differ from this line. The people's switches and limits are on a page of the Requests module, `/requests/people`, which the Users page links to; they are not on the Users page itself. And the Request piece on a Discover card is drawn by the same code as on a search result's card, but no test opens a Discover page as a user (see 1.15).
- [x] 1.6 The two notices of requests in the attention sources.
- [x] 1.7 A user guide page, "Let other people ask for titles" (`docs/20-let-other-people-ask-for-titles.md`), and the known limits of this slice in `openspec/project.md`.

The test paths below are the plan's. Where a scenario's test has another path in the tree, the task names it. `tests/test-requests.c` has 41 cases and `tests/test-web-requests.c` has 12; one case often covers several scenarios.

Tests, `requests` specification:

- [x] 1.10 `/requests/model/movie`: A movie.
- [x] 1.11 `/requests/model/whole-series-is-a-list`: The whole series becomes a season list.
- [x] 1.12 `/requests/model/first-and-latest`: The first and the latest season (the first and the latest in the case of 1.11, on a series of two seasons; the latest of the series with seasons 0 to 3, and the specials only when picked, in this one).
- [x] 1.13 `/requests/model/no-episodes-no-text`: No episodes and nothing typed. The model's part is this case (a season the record does not have, a season picked twice, nothing picked, a title of another kind). A post with an episode number or a note is refused by the form's rule, tested in `/web/requests/user-asks`.
- [x] 1.14 `/requests/model/module-off`: A module that is off (a layer without the TV module), and `/web/requests/module-off` (the TV module registered and switched off).
- [ ] 1.15 `/web/requests/user-asks-from-discover`: An ordinary user asks from Discover. Not tested as named: `/web/requests/user-asks` asks from a title's page and reads the Request form on a search result's card. The Discover card's piece is the same branch of the same function, but Discover shows the lists of a provider that offers them, and the requests' fixture provider offers none (the Discover tests use a TMDb fake, which the requests' fixture does not set up), so no test opens a Discover page as a user.
- [x] 1.16 `/web/requests/user-cannot-add`: An ordinary user still cannot add (in `/web/requests/user-asks`).
- [x] 1.17 `/web/requests/forged-post`: A forged request post (in `/web/requests/user-asks`; also without a session).
- [x] 1.18 `/requests/decide/waiting-fetches-nothing`: Waiting fetches nothing.
- [x] 1.19 `/requests/decide/approval-adds-and-searches`: Approval adds and searches.
- [x] 1.20 `/requests/decide/approval-of-seasons`: Approval of seasons.
- [x] 1.21 `/requests/decide/widens-never-narrows`: Approval widens and never narrows.
- [x] 1.22 `/requests/decide/decline-has-reason`: A decline has a reason.
- [x] 1.23 `/requests/decide/two-at-once`: Two decisions at once (forced to meet; see the header of `race()` in the test).
- [x] 1.24 `/web/requests/decide-not-admin`: Not an administrator (in `/web/requests/decide`).
- [x] 1.25 `/requests/place/defaults-used`: Defaults are used (in `/requests/decide/approval-adds-and-searches`).
- [x] 1.26 `/requests/place/request-cannot-choose`: A request cannot choose (in `/web/requests/user-asks`: twelve fields, each refused with 400 and no request made; the model's own call has no such member).
- [x] 1.27 `/web/requests/no-defaults-yet`: No defaults yet (in `/web/requests/decide`).
- [x] 1.28 `/requests/auto/off-by-default`: Off by default (in `/requests/decide/waiting-fetches-nothing`, with the defaults set).
- [x] 1.29 `/requests/auto/movie`: A movie approved automatically.
- [x] 1.30 `/requests/auto/one-switch-not-the-other`: One switch does not open the other (in `/requests/auto/movie`).
- [x] 1.31 `/requests/auto/too-many-seasons`: Too many seasons for automatic approval (the reason is stored and read; the sentence on the Requests page is not asserted by a test).
- [x] 1.32 `/requests/auto/defaults-gone`: The defaults are gone (the stored reason; the page's sentence for "no defaults" is asserted in `/web/requests/switches-and-limits`, the one for "missing" is not).
- [x] 1.33 `/requests/quota/limit-reached`: The limit is reached.
- [x] 1.34 `/requests/quota/seasons-count-each`: Seasons count one each.
- [x] 1.35 `/web/requests/quota-shown`: What is left is shown (in `/web/requests/mine`; the words themselves in `/requests/quota/limit-reached`).
- [x] 1.36 `/requests/quota/new-window`: A new window (day and month in this case, week in `/requests/quota/limit-reached`; the two days a year of 23 and 25 hours in `/requests/quota/window-bounds`, a pure test that skips them where the system has no data for the time zone it uses).
- [x] 1.37 `/requests/quota/withdraw-gives-back`: Withdrawing gives the count back (in `/requests/quota/limit-reached`).
- [x] 1.38 `/requests/quota/decline-keeps`: A decline keeps the count (in `/requests/quota/limit-reached`).
- [x] 1.39 `/requests/quota/limit-of-none`: A limit of none (refused: in `/requests/quota/seasons-count-each`; no following either: in `/requests/follow/movie`; not offered: in `/web/requests/series-page`).
- [x] 1.40 `/web/requests/states-of-a-search`: The states of a search (an administrator and two ordinary users; a requester in P2).
- [x] 1.41 `/requests/state/seasons-of-a-series`: Seasons of a series (in `/requests/follow/seasons-left-out` and `/web/requests/series-page`).
- [x] 1.42 `/web/requests/who-asked-shown`: Who asked, shown (in `/web/requests/who-asked`).
- [x] 1.43 `/web/requests/who-asked-hidden`: Who asked, hidden (in `/web/requests/who-asked`).
- [x] 1.44 `/web/requests/reason-not-for-everyone`: A reason is not for everyone (in `/web/requests/who-asked` and `/requests/read/lists`).
- [x] 1.45 `/requests/follow/movie`: Following a movie request.
- [x] 1.46 `/requests/follow/seasons-left-out`: Seasons are left out, not refused.
- [x] 1.47 `/requests/follow/nothing-remains`: Nothing remains (in `/requests/follow/seasons-left-out`).
- [x] 1.48 `/requests/follow/already-in-library`: Already in the library.
- [x] 1.49 `/requests/follow/asked-twice`: Asked twice (in `/requests/follow/movie`; two posts at the same moment, and two people at the same moment, in this case, forced to meet).
- [x] 1.50 `/requests/follow/already-looking`: A title Axis is already looking for (in `/requests/follow/already-in-library`).
- [x] 1.51 `/requests/declined/asked-again`: Asked again.
- [x] 1.52 `/requests/declined/closed`: Closed to requests (in `/requests/declined/asked-again` and `/web/requests/decide`).
- [x] 1.53 `/requests/declined/opened-again`: Opened again (the same two).
- [x] 1.54 `/requests/withdraw/while-waiting`: Withdrawn while waiting.
- [x] 1.55 `/requests/withdraw/not-after-approval`: Not after approval (in `/requests/withdraw/while-waiting`; a withdrawal that races an approval in this case, forced to meet).
- [x] 1.56 `/requests/withdraw/not-anothers`: Not another person's (in `/requests/withdraw/while-waiting` and `/web/requests/mine`).
- [x] 1.57 `/requests/withdraw/follower-takes-over`: A follower takes it over, and one without quota left does not.
- [x] 1.58 `/requests/state/downloading`: Downloading (in `/requests/state/movie`).
- [x] 1.59 `/requests/state/movie-available`: A movie is available (in `/requests/state/movie`).
- [x] 1.60 `/requests/state/series-partly`: A series is partly available.
- [x] 1.61 `/requests/state/failure-plain-words`: A failure in plain words (in `/requests/state/movie` and `/web/requests/mine`).
- [x] 1.62 `/requests/state/import-needs-person`: An import that needs a person (in `/requests/state/movie`).
- [x] 1.63 `/requests/state/removed`: Removed (in `/requests/state/movie`; a series in `/requests/state/series-removed`; on the pages in `/web/requests/try-again`).
- [x] 1.64 `/requests/decide/folder-gone`: The folder is gone (and `/web/requests/try-again`; a folder of the other kind is refused in the same case).
- [x] 1.65 `/requests/decide/tried-again`: Tried again (in `/requests/decide/folder-gone` and `/web/requests/try-again`, with a second "Try again" refused once the title is in Axis).
- [x] 1.66 `/web/requests/mine`: My requests.
- [x] 1.67 `/web/requests/anothers-by-identifier`: Another person's request by its identifier (in `/web/requests/mine`: the same status and the same body as for an identifier never issued, to read, to withdraw and to stop following).
- [x] 1.68 `/web/requests/nothing-of-inner-workings`: Nothing of the engine's inner workings (in `/web/requests/mine`: a marked folder name, the profile's name, a release name, a file name and a protocol; no indexer or download client is set up in the fixture, so none could appear).
- [x] 1.69 `/web/requests/waiting-list`: The waiting list (in `/web/requests/decide`, with three waiting and one decided, not ten).
- [x] 1.70 `/web/requests/ordinary-user-page`: An ordinary user (in `/web/requests/mine`).
- [x] 1.71 `/web/requests/switches-and-limits`: The switches and limits of a person (on `/requests/people`; see 1.5).
- [x] 1.72 `/requests/account/disabled`: A disabled requester, run with a disabled and with a deleted ordinary user; and `/web/requests/account-goes`, through the Users page with accounts in the database and a real login. Again with a requester in P2.
- [x] 1.73 `/requests/state/one-query-a-page`: the state of 20 titles and of 50 requests is read in a bounded number of queries, on a library of 5,000 movies and 1,500 series.
- [x] 1.74 `/requests/quota/daily-ceiling`: Asking and withdrawing again and again (also a request handed over to a follower, which stays counted for its maker, and an administrator, who is not limited).
- [x] 1.75 `/requests/decide/two-placings`: Two placings of one series (forced to meet).
- [x] 1.76 `/requests/read/who-asked-series`: Who asked, hidden, for another season.
- [x] 1.77 `/requests/model/title-merge`: Two waiting requests for a movie.
- [x] 1.78 `/requests/model/merge-approved-wins`: The approved one stays (on the record that goes, and on the record kept).
- [x] 1.79 `/requests/model/merge-seasons`: Seasons that are not shared (with an approved request on each side).

Tests beyond the plan: `/requests/quota/race` (with room for one, of six requests at the same moment exactly one is made), `/requests/decide/stopped-before-placing` (a daemon that stopped between a decision and the placing), `/web/requests/nothing-typed-is-logged`.

Tests, `attention` specification:

All six are one case, `/web/attention/requests` in `tests/test-web-requests.c`.

- [x] 1.80 `/web/attention/requests-wait`: Requests wait.
- [x] 1.81 `/web/attention/one-request`: One request.
- [x] 1.82 `/web/attention/requests-decided`: Decided.
- [x] 1.83 `/web/attention/request-approved-automatically`: Approved automatically.
- [x] 1.84 `/web/attention/request-could-not-be-added`: Could not be added.
- [x] 1.85 `/web/attention/requests-not-admin`: Not an administrator.

Gate:

- [ ] 1.90 Independent review of P1: who may read and change a request, the decision transaction, the quota count. Record the reviewer and the findings in `design.md`; fix each finding with a test. A first review was made and its findings were fixed or recorded as known limits (`design.md`, "Independent review"); the fixes wait for review. Not tested by a case: the account row read `FOR SHARE` (a disable that meets a request) and the title that is removed between the facts' read and the request's transaction. The four gates (`make check`, `make fuzz`, `make check-container`, `make check-examples`) were not run by the author either; only the test programs the slice touches were, under both sanitizers.

## 2. P2: the scoped token and the narrow API

Build:

- [x] 2.0 Before the slice (recorded in `design.md`, "As built in P1"): the adult mark is stored. Migration `0084_title_adult.sql` (`meta_title.adult`), set wherever a provider's answer carries it; the engine's ask path refuses a marked title for anyone but an administrator, with the answer of a title that does not exist. Tests: `/requests/model/adult-title` (the mark, the refusal, no following, the mark not cleared), `/web/requests/adult-title` (the engine's own Request route), and the `title`, `art`, `request` and `search` operations in `/portal/api/fields/answers`.
- [x] 2.1 Confirm first what "Believed and not confirmed" says about a second listener, and record the answer in `design.md`. Confirmed for two servers on one main loop; the local port of a request is not needed. Recorded under "Believed and not confirmed".
- [x] 2.2 Migration `0085_portal_api.sql` (0084 is the adult mark's): the role `requester` in the check constraint of `app_user`, `portal_token`, `kind` on `web_session` (`web` for every existing row). Tested in `tests/test-db-migrate.c`, in `/db/migrate/requests-on-accounts`, on a database with accounts, a title and a session.
- [x] 2.3 The requester role in the user store, the Users page, the engine's login, the session lookup and the middleware; the password rules for a requester.
- [x] 2.4 The portal listener (`server.portal_listen_port`, `AXIS_PORTAL_LISTEN_PORT`), with a router that holds the operations only, under the connection limits. The server, its gate, its router and its limits are tested in `tests/test-portal-api.c`; the daemon's own start of it from the configuration in `tests/test-daemon.c` (2.20).
- [x] 2.5 The portal gate and the token store, beside the worker's; the page "Request portal" (`/admin/portal`, a page of the Requests module, under System).
- [x] 2.6 Portal sessions: the kind, the proof, its expiry and revocation, the Users page's list (a count for each account and "End N portal sessions"; not a row for each session).
- [x] 2.7 The eleven operations in `src/api/`, with one bounded reader for their bodies and one writer for their answers (`axis-portal-proto.c`).
- [x] 2.8 The daemon's limits: the hourly login ceiling, the search budget and cache, the portal's daily ceiling, the reserved hashing place, the device value in `login`.
- [x] 2.9 A fuzz target for the reader of operation bodies, `tests/fuzz/fuzz-portal-body.c` (also the writer of answers). Compiled; `make fuzz` was not run by the author.
- [x] 2.10 The known limits of this slice in `openspec/project.md` ("The portal API").

The test paths below are the plan's. `tests/test-portal-api.c` has 22 cases (17 before the review), and one case covers several scenarios; where a scenario's test has another path, the task names it.

Tests, `portal` specification:

- [x] 2.20 `/portal/api/listener/off-by-default`: Off by default. In `tests/test-daemon.c`, with the real daemon: without the port it listens on its main port only (nothing listens on the other), and the main port answers the portal API's paths 404; with `AXIS_PORTAL_LISTEN_PORT` the second port answers the portal API (401 without a token) and 404 for `/`, `/login`, `/setup`, `/healthz`, a static file and the worker API. That daemon has no portal token, so "with a valid portal token" on the main listener is `/portal/api/listener/only-the-api`'s.
- [x] 2.21 `/portal/api/listener/only-the-api`: Only the API on the portal listener (thirteen paths, GET, POST and HEAD, with the token and with an administrator's session).
- [x] 2.22 `/portal/api/listener/not-on-main`: Not on the main listener (in `/portal/api/listener/only-the-api`: every operation, with the token, with an administrator's session, with both; a login there makes no session).
- [x] 2.23 `/portal/api/token/valid`: A valid token (in `/portal/api/token/gate`).
- [x] 2.24 `/portal/api/token/revoked`: A revoked token (in `/portal/api/token/gate` and `/web/portal/token-shown-once`).
- [x] 2.25 `/web/portal/token-shown-once`: Shown once, stored as a hash (also: administrators only, four at most, revoked on the page, not in the log).
- [x] 2.26 `/portal/api/token/replacing`: Replacing a token (in `/portal/api/token/gate`).
- [x] 2.27 `/portal/api/token/workers-token`: A worker's token (in `/portal/api/token/gate`).
- [x] 2.28 `/portal/api/token/elsewhere`: A portal token elsewhere (in `/portal/api/token/gate` and `/portal/api/scope/every-other-route`).
- [x] 2.29 `/portal/api/token/session-refused`: A session on the portal API (in `/portal/api/token/gate`).
- [x] 2.30 `/portal/api/token/in-the-address`: A token in the address (in `/portal/api/token/gate`).
- [x] 2.31 `/portal/api/token/wrong-throttled`: Wrong tokens are throttled.
- [x] 2.32 `/portal/api/scope/not-an-operation`: Not an operation (in `/portal/api/token/gate`).
- [x] 2.33 `/portal/api/scope/every-other-route`: Every other route of the daemon (walks the route table on both listeners; fails when a route is added that opens to the token). The routes are those the test's daemon registers: the shell's, the accounts', and the modules of the requests' fixture; not every module of `axisd`.
- [x] 2.34 `/portal/api/scope/a-browser`: A browser (in `/portal/api/token/gate`).
- [x] 2.35 `/portal/api/scope/methods-and-bodies`: Methods and bodies (in `/portal/api/token/gate`).
- [x] 2.36 `/portal/api/scope/member-not-defined`: A member that is not defined (every operation, 31 member names, each as a text and as a number; also wrong types, ranges and places).
- [x] 2.37 `/portal/api/login/requester`: A requester logs in (in `/portal/api/login/uniform-failures`).
- [x] 2.38 `/portal/api/login/uniform-failures`: Uniform failures (status, body, and one hash each, counted; also a name that cannot be one).
- [x] 2.39 `/portal/api/login/administrator-opens-nothing`: An administrator's password opens nothing here (in `/portal/api/login/uniform-failures`).
- [x] 2.40 `/web/auth/requester-password-rules`: Password rules for a requester (in `/portal/api/proof/account-changes`: on creating the account and on setting its password).
- [x] 2.41 `/portal/api/proof/token-without-proof`: The token without a proof (in `/portal/api/proof/anothers-request`: nine operations, one body).
- [x] 2.42 `/portal/api/proof/naming-a-person`: Naming a person does nothing (in `/portal/api/proof/anothers-request`).
- [x] 2.43 `/portal/api/proof/anothers-request`: Another person's request (also as its follower).
- [x] 2.44 `/portal/api/proof/made-up`: A made-up proof (in `/portal/api/proof/anothers-request`).
- [x] 2.45 `/portal/api/proof/disabled-at-once`: Disabled at once (in `/portal/api/proof/account-changes`).
- [x] 2.46 `/portal/api/proof/password-or-role-change`: A password or role change ends proofs (in `/portal/api/proof/account-changes`).
- [x] 2.47 `/portal/api/proof/logout`: Logout (in `/portal/api/proof/anothers-request`).
- [x] 2.48 `/portal/api/proof/not-an-engine-session`: Not a session of the engine (in `/portal/api/login/uniform-failures`).
- [x] 2.49 `/portal/api/proof/stored-as-hash`: Stored as a hash (in `/portal/api/login/uniform-failures`).
- [x] 2.50 `/portal/api/fields/search-result`: A search result's members (in `/portal/api/fields/answers`; the member names of the answers of `hello`, `login`, `whoami`, `search`, `browse`, `title`, `request` and of a title, a season, a request and a quota are compared with lists in the tests).
- [x] 2.51 `/portal/api/fields/nothing-of-inner-workings`: Nothing of the library's inner workings (in `/portal/api/fields/answers`: a marked folder name, a marked profile name, a file's name and path, the provider's name; no indexer or download client is set up in the fixture, and the grab's release name is not searched for by name).
- [x] 2.52 `/portal/api/fields/who-asked-setting`: Who asked follows the setting (in `/portal/api/fields/answers`).
- [x] 2.53 `/portal/api/fields/only-the-callers`: Only the caller's requests (in `/portal/api/proof/anothers-request`).
- [x] 2.54 `/portal/api/art/by-title`: A poster by title (in `/portal/api/fields/answers`).
- [x] 2.55 `/portal/api/art/no-address`: No address is taken (in `/portal/api/scope/member-not-defined`).
- [x] 2.56 `/portal/api/art/not-held`: Not held (in `/portal/api/fields/answers`).
- [x] 2.57 `/portal/api/search/a-search`: A search (in `/portal/api/fields/answers`).
- [x] 2.58 `/portal/api/search/asked-once`: Asked once (in `/portal/api/fields/answers`).
- [x] 2.59 `/portal/api/search/provider-unreachable`: The provider is unreachable.
- [x] 2.60 `/portal/api/search/too-long`: Too long (in `/portal/api/fields/answers`).
- [x] 2.61 `/portal/api/search/browse-asks-nobody`: Browse asks nobody (in `/portal/api/fields/answers`; the fixture's provider offers no lists, so the answer holds no title).
- [x] 2.62 `/portal/api/limits/one-password`: Guessing one password.
- [x] 2.63 `/portal/api/limits/across-names`: Guessing across names.
- [x] 2.64 `/portal/api/limits/reported-addresses`: Reported addresses clear nothing (in `/portal/api/limits/one-password`).
- [x] 2.65 `/portal/api/limits/search-budget`: The search budget.
- [x] 2.66 `/portal/api/limits/daily-ceiling`: The portal's daily ceiling (and `/requests/quota/portal-ceiling` in `tests/test-requests.c`).
- [x] 2.67 `/portal/api/limits/quota-not-the-clients`: Quota does not depend on the client (in `/portal/api/limits/daily-ceiling`).
- [x] 2.68 `/portal/api/device/owner-gets-in` and `/portal/api/device/altered`: the daemon's half of "A remembered browser is not locked out by a stranger" (in `/portal/api/limits/one-password`).

Tests, `web-auth` specification:

- [x] 2.70 `/web/auth/requester-at-engine-login`: A requester at the engine's login (in `/portal/api/proof/account-changes`).
- [x] 2.71 `/web/auth/requester-created`: Created by an administrator (in `/portal/api/proof/account-changes`).
- [x] 2.72 `/web/auth/no-session-for-requester`: No session for a requester (every route of the test's main listener). The session lookup refuses it and ends it, so in the suite the other two barriers are not reached; by hand, with the lookup's rule switched off the case's walk still passed, with the identity check off too it still passed, and with the middleware's and the route check's off as well it failed. Doing so found that the third barrier let a requester's name onto a public page, which is fixed: the middleware now forgets a requester's session on every path.
- [x] 2.73 `/web/auth/role-change-ends-sessions`: A role change ends sessions (in `/portal/api/proof/account-changes`).
- [x] 2.74 `/web/auth/last-admin-not-requester`: The last administrator (in `/portal/api/proof/account-changes`).
- [x] 2.75 `/portal/api/class/default-deny`: Default deny holds (in `/portal/api/token/gate`).
- [x] 2.76 `/portal/api/class/throttled`: Throttled (`/portal/api/token/wrong-throttled`).
- [x] 2.77 `/portal/api/class/new-route-closed`: A new route is closed to the token (in `/portal/api/scope/every-other-route`).
- [x] 2.78 `/portal/api/sessions/kinds-do-not-cross`: Kinds do not cross (in `/portal/api/login/uniform-failures`).
- [x] 2.79 `/web/auth/portal-sessions-ended`: Ended by an administrator (in `/portal/api/proof/account-changes`).
- [ ] 2.80 `/portal/api/hashing/admin-during-flood`: An administrator logs in during a flood. In part, in `/portal/api/hashing/queue-bounded`: the login succeeds while portal logins are queued; that it did not wait behind them follows from the one hashing place the portal has, and is not measured.
- [x] 2.81 `/portal/api/hashing/queue-bounded`: The portal's queue is bounded.

After the first review (`design.md`, "As built in P2", "After the review"):

- [x] 2.82 S1, M9: a requester takes another role only with a new password, held to the new role's rule and not the one it has. `/portal/api/proof/account-changes`: refused without one, with an empty one, a short one and the old one; then the old password fails at the engine's login and the new one opens it.
- [x] 2.83 S2, M5: the adult mark has three values (migration 0084 edited in place: NULL is "not known"); every provider answer read sets it (TheTVDB's false); for anyone who is not an administrator NULL is treated as marked. `/portal/api/fields/mark-not-known` (title, art, request of both kinds, a search from memory; visible once a search's answer marks it false), `/requests/model/adult-mark-not-known` (the ask path, a closed title, no following, an administrator asks; askable after a lookup), `/db/migrate/requests-on-accounts` (stored titles are NULL after 0084).
- [x] 2.84 S3: a budget for every operation, 600 a minute for a token and 120 for a person, in bounded memory, and a thread pool of the API's own (four threads). `/portal/api/limits/operation-budgets`, with the clock stopped: exact counts for both, another person and another token not held to them, a refusal logged once a minute; `/portal/api/login/engine-queue-full` shows an operation answered while the engine's own workers are all busy. The pool's bound of four is not measured.
- [x] 2.85 S4: one person's share of the portal's day, a third of `requests.portal_per_day`, withdrawn requests included. `/portal/api/limits/one-persons-share` (ask and withdraw ten times, the eleventh refused, the others still ask) and `/requests/quota/portal-ceiling` (rewritten for the share: four people, the share's refusal and then the day's).
- [x] 2.86 M1, M8: the token is looked up before the address throttle, and a valid one is exempt (`/portal/api/token/wrong-throttled`); a token's use is written once a minute whatever the address (`/portal/api/token/gate`).
- [x] 2.87 M2, M3, M6, M7: the wrong kind and a closed title for a marked one, `asked_by` of an ordinary user's and an administrator's request, a search's third page (`/portal/api/fields/answers`); `title` and `art` while the TV module is off (`/portal/api/fields/kind-off`).
- [x] 2.88 M10: `server.portal_listen_host` (`AXIS_PORTAL_LISTEN_HOST`), in the example configuration and the guide. `/config/server/portal-listener`. The daemon's own start on that address is not run by a test.
- [x] 2.89 M4 and M11, and what could not be judged: M4 is a known limit; the comments of the tests M11 names say what they assert; a deferred login answered before its handler returns is answered at once (`/portal/api/login/engine-queue-full`); an older `axisd` refuses a newer database, and `meta_search` rows are pruned after 30 days (both recorded in `design.md`).

Gate:

- [ ] 2.90 Independent review of P2: the gate's order, the role's three barriers, the proof, the object-level checks, the answers' members. Record it in `design.md`; fix each finding with a test. A first review was made and its findings were fixed or recorded as known limits (2.82 to 2.89); the fixes wait for review. The four gates (`make check`, `make fuzz`, `make check-container`, `make check-examples`) were not run by the author either; only the test programs the slice touches were, under both sanitizers.

## 3. P3: the axis-portal program

Build:

- [ ] 3.1 Split from `src/web/` what the portal reuses and that must not pull in the database: the security headers, the CSRF and cookie helpers, the throttle, the limits.
- [ ] 3.2 `clients/portal/`: configuration (file and `AXIS_PORTAL_*`, the token from a file, unknown keys refused), the client of the eleven operations with a bounded reader for each answer, the pages, the poster cache, the log, `--healthcheck`.
- [ ] 3.3 `make` builds `axis-portal` without libpq; `tests/check-portal-link.sh` beside the worker's.
- [ ] 3.4 Fuzz targets: `tests/fuzz/fuzz-portal-answer.c` (the daemon's answers as the portal reads them) and `tests/fuzz/fuzz-portal-form.c` (the forms).
- [ ] 3.5 The "not reviewed for the internet" line at start.
- [ ] 3.6 The known limits of this slice in `openspec/project.md`.

Tests, `portal` specification:

- [ ] 3.10 `tests/check-portal-link.sh`: Not linked with the database client.
- [ ] 3.11 `/portal/config/unknown-key`: An unknown key.
- [ ] 3.12 `/portal/config/plain-http-refused`: Plain HTTP refused.
- [ ] 3.13 `/portal/process/nothing-writable`: Nothing writable (the real binary, every directory read-only).
- [ ] 3.14 `/portal/pages/daemon-away`: The daemon is away.
- [ ] 3.15 `/portal/pages/without-session`: Without a session (every route of the portal's table but login and the static files; the stand-in daemon counts no call).
- [ ] 3.16 `/portal/pages/probe-loopback-only`: The probe.
- [ ] 3.17 `/portal/pages/headers`: Headers (compared with the engine interface's own).
- [ ] 3.18 `/portal/pages/state-and-form`: A title's state and the form.
- [ ] 3.19 `/portal/pages/provider-text`: Provider text.
- [ ] 3.20 `/portal/pages/failure-plain-words`: A failure in plain words.
- [ ] 3.21 `/portal/pages/engine-paths`: The engine's paths.
- [ ] 3.22 `/portal/pages/headers-not-passed-on`: Headers are not passed on.
- [ ] 3.23 `/portal/session/cookie`: The cookie.
- [ ] 3.24 `/portal/session/post-without-token`: A post without the token.
- [ ] 3.25 `/portal/session/nothing-secret-in-an-address`: Nothing secret in an address.
- [ ] 3.26 `/portal/session/open-redirect`: An open redirect.
- [ ] 3.27 `/portal/session/plain-http`: Plain HTTP.
- [ ] 3.28 `/portal/session/forwarded-from-elsewhere`: Forwarded headers from elsewhere.
- [ ] 3.29 `/portal/process/restart`: A restart (the real binary and a real daemon).
- [ ] 3.30 `/portal/process/disabled-while-logged-in`: Disabled while logged in.
- [ ] 3.31 `/portal/process/role-change`: A role change.
- [ ] 3.32 `/portal/process/logout`: Logout.
- [ ] 3.33 `/portal/limits/login-flood`: A flood of logins from one address.
- [ ] 3.34 `/portal/limits/search-flood`: A flood of searches.
- [ ] 3.35 `/portal/limits/large-body`: A large body.
- [ ] 3.36 `/portal/process/owner-gets-in`: The owner still gets in (through the portal's cookie).
- [ ] 3.37 `/portal/process/altered-device`: An altered device value.
- [ ] 3.38 `/portal/log/secrets-absent`: Secrets absent from the log.
- [ ] 3.39 `/portal/log/password-as-username`: A password typed as a username.
- [ ] 3.40 `/portal/pages/two-session-cookies`: a request with two session cookies is treated as no session.

Gate:

- [ ] 3.90 Independent review of P3: what the portal passes on, its cookies and CSRF, its readers of the daemon's answers. Record it in `design.md`; fix each finding with a test.

## 4. P4: limits, lockout, hardening and the deployment examples

Build:

- [ ] 4.1 The image: `axis-portal` and its example configuration; the decision on a slim stage (open question 1) recorded and built.
- [ ] 4.2 At start, `axis-portal` sets no-new-privileges and makes itself not dumpable.
- [ ] 4.3 The daemon remembers the addresses that presented a valid portal token and refuses a login at its interface from them.
- [ ] 4.4 The notice for a ceiling that was reached, and the counts on the page "Request portal".
- [ ] 4.5 The optional portal service in `deploy/examples/traefik/`, `deploy/examples/kubernetes/` (a Deployment, a Service, a NetworkPolicy, an Ingress) and `deploy/examples/quadlet/`, each with its network and its README part.
- [ ] 4.6 `deploy/README.md`: "The request portal": what runs where, the token, the proxy, the trusted proxies, what a held portal can and cannot do, how to replace the token, and the "not reviewed" statement.
- [ ] 4.7 The known limits of this slice in `openspec/project.md`: what was not run (Docker, a real certificate, a cluster's NetworkPolicy exercised), and the limits listed in `design.md` under "Where it is hard".

Tests, `deployment` specification:

- [ ] 4.10 `tests/check-container.sh`, portal part: Hardened and empty.
- [ ] 4.11 `tests/check-container.sh`, portal part: Nothing of the daemon's is needed.
- [ ] 4.12 `tests/check-examples.sh`, portal part: The portal cannot reach the database.
- [ ] 4.13 `tests/check-examples.sh`, portal part: A request end to end.
- [ ] 4.14 `tests/check-examples.sh`, portal part: The engine is not behind the portal's proxy.
- [ ] 4.15 `tests/check-examples.sh`: Left out by default.
- [ ] 4.16 The Kubernetes manifests applied to a kind cluster with the portal's: Kubernetes. Record in the known limits whether the NetworkPolicy was exercised.
- [ ] 4.17 `/portal/session/proxy-listed`: The proxy is listed.
- [ ] 4.18 `/portal/session/proxy-not-listed`: The proxy is not listed.
- [ ] 4.19 `tests/tools/check-portal-review.py` in `make check`: Before the review.

Tests, `web-auth` and `attention` specifications:

- [ ] 4.20 `/web/auth/login-from-portal-address`: A login from the portal's address.
- [ ] 4.21 `/web/auth/login-from-another-address`: Another address.
- [ ] 4.22 `/web/attention/portal-reached-a-limit`: The portal reached a limit.

Gate:

- [ ] 4.90 Independent review of P4: the examples' networks as built, the hardening as run, the address rule. Record it in `design.md`; fix each finding with a test.

## 5. P5: the independent security review

- [ ] 5.1 A reviewer who wrote none of P1 to P4 is given the portal's address and a requester account, and tries: the login (guessing, uniform answers, the throttles), every page without a session, CSRF, the cookies, the redirect, the headers, a request for another person's things by every identifier, the quotas and the ceilings, and every path of the engine through the portal.
- [ ] 5.2 The reviewer is then given the portal's container and its token, and tries each line of "If the portal is taken over" in `design.md`, in both directions: that what it says can be done is all that can be done, and that each "cannot" holds. The reviewer also tries the daemon's main listener from the portal's network.
- [ ] 5.3 The reviewer reads the portal API's code for the classes of fault in `openspec/project.md`: trusted input, races (two decisions, a withdrawal and an approval, a login and a disable), a state that never recovers, a leak, and a promise in a specification that has no test.
- [ ] 5.4 Each finding states what happens, the steps that show it, how sure the reviewer is and how bad it is. Each is fixed with a test, or is recorded as a known limit with the owner's agreement.
- [ ] 5.5 `design.md` names the review and its findings. `deploy/README.md`, the examples and the user guide say what the review covered and what it left, and the line at start goes.
- [ ] 5.6 `tests/tools/check-portal-review.py`: After the review.
- [ ] 5.7 Archive the change: `openspec archive add-request-portal`.
