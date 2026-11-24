# Tasks: a request portal

Every scenario in the spec deltas has a task for its test, named by the test's path and file. Where it can be done, see the test fail with its check switched off before the slice is merged. The four gates pass on the final code of each slice.

Test programs: `tests/test-requests.c` (the model, with a database), `tests/test-web-requests.c` (the engine's pages), `tests/test-portal-api.c` (the portal API, with a database), `tests/test-portal.c` (the portal's pages against a stand-in daemon), `tests/test-portal-process.c` (the real `axis-portal` against a real daemon), `tests/check-portal-link.sh`, and the portal's part of `tests/check-container.sh` and `tests/check-examples.sh`.

## 1. P1: requests inside the engine

Build:

- [ ] 1.1 Migration `0080_requests.sql`: `request`, `request_season`, `request_follower`, `request_closed_title`, the partial unique indexes for one open request, the two switches and two limits on `app_user`. Test it in `tests/test-db-migrate.c` on a database with accounts.
- [ ] 1.2 `src/requests/`: the model, the season expansion, the duplicate and follower rules, the quota count from rows, the decision transaction, the derived state in one query for a page.
- [ ] 1.3 The approval path: add or find the title, monitor the request's seasons, record, queue the search; "Try again" for an approval that could not add.
- [ ] 1.4 Settings: `requests.window`, `requests.movies_per_window`, `requests.seasons_per_window`, `requests.auto_max_seasons`, `requests.show_who_asked`, and the four defaults for placement.
- [ ] 1.5 Pages: `/requests` for administrators and as "My requests", Request on search results, Discover cards and title pages, the state on every card, the Users page's switches and limits.
- [ ] 1.6 The two notices of requests in the attention sources.
- [ ] 1.7 A user guide page, "Let other people ask for titles", and the known limits of this slice in `openspec/project.md`.

Tests, `requests` specification:

- [ ] 1.10 `/requests/model/movie`: A movie.
- [ ] 1.11 `/requests/model/whole-series-is-a-list`: The whole series becomes a season list.
- [ ] 1.12 `/requests/model/first-and-latest`: The first and the latest season.
- [ ] 1.13 `/requests/model/no-episodes-no-text`: No episodes and nothing typed.
- [ ] 1.14 `/requests/model/module-off`: A module that is off.
- [ ] 1.15 `/web/requests/user-asks-from-discover`: An ordinary user asks from Discover.
- [ ] 1.16 `/web/requests/user-cannot-add`: An ordinary user still cannot add.
- [ ] 1.17 `/web/requests/forged-post`: A forged request post.
- [ ] 1.18 `/requests/decide/waiting-fetches-nothing`: Waiting fetches nothing.
- [ ] 1.19 `/requests/decide/approval-adds-and-searches`: Approval adds and searches.
- [ ] 1.20 `/requests/decide/approval-of-seasons`: Approval of seasons.
- [ ] 1.21 `/requests/decide/widens-never-narrows`: Approval widens and never narrows.
- [ ] 1.22 `/requests/decide/decline-has-reason`: A decline has a reason.
- [ ] 1.23 `/requests/decide/two-at-once`: Two decisions at once.
- [ ] 1.24 `/web/requests/decide-not-admin`: Not an administrator.
- [ ] 1.25 `/requests/place/defaults-used`: Defaults are used.
- [ ] 1.26 `/requests/place/request-cannot-choose`: A request cannot choose.
- [ ] 1.27 `/web/requests/no-defaults-yet`: No defaults yet.
- [ ] 1.28 `/requests/auto/off-by-default`: Off by default.
- [ ] 1.29 `/requests/auto/movie`: A movie approved automatically.
- [ ] 1.30 `/requests/auto/one-switch-not-the-other`: One switch does not open the other.
- [ ] 1.31 `/requests/auto/too-many-seasons`: Too many seasons for automatic approval.
- [ ] 1.32 `/requests/auto/defaults-gone`: The defaults are gone.
- [ ] 1.33 `/requests/quota/limit-reached`: The limit is reached.
- [ ] 1.34 `/requests/quota/seasons-count-each`: Seasons count one each.
- [ ] 1.35 `/web/requests/quota-shown`: What is left is shown.
- [ ] 1.36 `/requests/quota/new-window`: A new window (day, week and month, and the two days a year of 23 and 25 hours).
- [ ] 1.37 `/requests/quota/withdraw-gives-back`: Withdrawing gives the count back.
- [ ] 1.38 `/requests/quota/decline-keeps`: A decline keeps the count.
- [ ] 1.39 `/requests/quota/limit-of-none`: A limit of none.
- [ ] 1.40 `/web/requests/states-of-a-search`: The states of a search.
- [ ] 1.41 `/requests/state/seasons-of-a-series`: Seasons of a series.
- [ ] 1.42 `/web/requests/who-asked-shown`: Who asked, shown.
- [ ] 1.43 `/web/requests/who-asked-hidden`: Who asked, hidden.
- [ ] 1.44 `/web/requests/reason-not-for-everyone`: A reason is not for everyone.
- [ ] 1.45 `/requests/follow/movie`: Following a movie request.
- [ ] 1.46 `/requests/follow/seasons-left-out`: Seasons are left out, not refused.
- [ ] 1.47 `/requests/follow/nothing-remains`: Nothing remains.
- [ ] 1.48 `/requests/follow/already-in-library`: Already in the library.
- [ ] 1.49 `/requests/follow/asked-twice`: Asked twice (also two posts at the same moment).
- [ ] 1.50 `/requests/follow/already-looking`: A title Axis is already looking for.
- [ ] 1.51 `/requests/declined/asked-again`: Asked again.
- [ ] 1.52 `/requests/declined/closed`: Closed to requests.
- [ ] 1.53 `/requests/declined/opened-again`: Opened again.
- [ ] 1.54 `/requests/withdraw/while-waiting`: Withdrawn while waiting.
- [ ] 1.55 `/requests/withdraw/not-after-approval`: Not after approval (also a withdrawal that races an approval).
- [ ] 1.56 `/requests/withdraw/not-anothers`: Not another person's.
- [ ] 1.57 `/requests/withdraw/follower-takes-over`: A follower takes it over.
- [ ] 1.58 `/requests/state/downloading`: Downloading.
- [ ] 1.59 `/requests/state/movie-available`: A movie is available.
- [ ] 1.60 `/requests/state/series-partly`: A series is partly available.
- [ ] 1.61 `/requests/state/failure-plain-words`: A failure in plain words.
- [ ] 1.62 `/requests/state/import-needs-person`: An import that needs a person.
- [ ] 1.63 `/requests/state/removed`: Removed.
- [ ] 1.64 `/requests/decide/folder-gone`: The folder is gone.
- [ ] 1.65 `/requests/decide/tried-again`: Tried again.
- [ ] 1.66 `/web/requests/mine`: My requests.
- [ ] 1.67 `/web/requests/anothers-by-identifier`: Another person's request by its identifier.
- [ ] 1.68 `/web/requests/nothing-of-inner-workings`: Nothing of the engine's inner workings.
- [ ] 1.69 `/web/requests/waiting-list`: The waiting list.
- [ ] 1.70 `/web/requests/ordinary-user-page`: An ordinary user.
- [ ] 1.71 `/web/requests/switches-and-limits`: The switches and limits of a person.
- [ ] 1.72 `/requests/account/disabled`: A disabled requester (run with a disabled ordinary user in P1, and again with a requester in P2).
- [ ] 1.73 `/requests/state/one-query-a-page`: the state of 20 titles and of 50 requests is read in a bounded number of queries, on a library of 5,000 movies and 1,500 series.

Tests, `attention` specification:

- [ ] 1.80 `/web/attention/requests-wait`: Requests wait.
- [ ] 1.81 `/web/attention/one-request`: One request.
- [ ] 1.82 `/web/attention/requests-decided`: Decided.
- [ ] 1.83 `/web/attention/request-approved-automatically`: Approved automatically.
- [ ] 1.84 `/web/attention/request-could-not-be-added`: Could not be added.
- [ ] 1.85 `/web/attention/requests-not-admin`: Not an administrator.

Gate:

- [ ] 1.90 Independent review of P1: who may read and change a request, the decision transaction, the quota count. Record the reviewer and the findings in `design.md`; fix each finding with a test.

## 2. P2: the scoped token and the narrow API

Build:

- [ ] 2.1 Confirm first what "Believed and not confirmed" says about a second listener, and record the answer in `design.md`.
- [ ] 2.2 Migration `NNNN_portal_api.sql` (the next free number in `data/migrations/` when it is written; 0083 is P1's): the role `requester` in the check constraint of `app_user`, `portal_token`, `kind` on `web_session` (`web` for every existing row).
- [ ] 2.3 The requester role in the user store, the Users page, the engine's login, the session lookup and the middleware; the password rules for a requester.
- [ ] 2.4 The portal listener (`server.portal_listen_port`, `AXIS_PORTAL_LISTEN_PORT`), with a router that holds the operations only, under the connection limits.
- [ ] 2.5 The portal gate and the token store, beside the worker's; the page "Request portal".
- [ ] 2.6 Portal sessions: the kind, the proof, its expiry and revocation, the Users page's list.
- [ ] 2.7 The eleven operations in `src/api/`, with one bounded reader for their bodies and one writer for their answers.
- [ ] 2.8 The daemon's limits: the hourly login ceiling, the search budget and cache, the portal's daily ceiling, the reserved hashing place, the device value in `login`.
- [ ] 2.9 A fuzz target for the reader of operation bodies, `tests/fuzz/fuzz-portal-body.c`.
- [ ] 2.10 The known limits of this slice in `openspec/project.md`.

Tests, `portal` specification:

- [ ] 2.20 `/portal/api/listener/off-by-default`: Off by default.
- [ ] 2.21 `/portal/api/listener/only-the-api`: Only the API on the portal listener.
- [ ] 2.22 `/portal/api/listener/not-on-main`: Not on the main listener.
- [ ] 2.23 `/portal/api/token/valid`: A valid token.
- [ ] 2.24 `/portal/api/token/revoked`: A revoked token.
- [ ] 2.25 `/web/portal/token-shown-once`: Shown once, stored as a hash.
- [ ] 2.26 `/portal/api/token/replacing`: Replacing a token.
- [ ] 2.27 `/portal/api/token/workers-token`: A worker's token.
- [ ] 2.28 `/portal/api/token/elsewhere`: A portal token elsewhere.
- [ ] 2.29 `/portal/api/token/session-refused`: A session on the portal API.
- [ ] 2.30 `/portal/api/token/in-the-address`: A token in the address.
- [ ] 2.31 `/portal/api/token/wrong-throttled`: Wrong tokens are throttled.
- [ ] 2.32 `/portal/api/scope/not-an-operation`: Not an operation.
- [ ] 2.33 `/portal/api/scope/every-other-route`: Every other route of the daemon (walks the route table on both listeners; fails when a route is added that opens to the token).
- [ ] 2.34 `/portal/api/scope/a-browser`: A browser.
- [ ] 2.35 `/portal/api/scope/methods-and-bodies`: Methods and bodies.
- [ ] 2.36 `/portal/api/scope/member-not-defined`: A member that is not defined (every operation, every member name the engine's forms use).
- [ ] 2.37 `/portal/api/login/requester`: A requester logs in.
- [ ] 2.38 `/portal/api/login/uniform-failures`: Uniform failures (status, body, and one hash each, counted).
- [ ] 2.39 `/portal/api/login/administrator-opens-nothing`: An administrator's password opens nothing here.
- [ ] 2.40 `/web/auth/requester-password-rules`: Password rules for a requester.
- [ ] 2.41 `/portal/api/proof/token-without-proof`: The token without a proof.
- [ ] 2.42 `/portal/api/proof/naming-a-person`: Naming a person does nothing.
- [ ] 2.43 `/portal/api/proof/anothers-request`: Another person's request.
- [ ] 2.44 `/portal/api/proof/made-up`: A made-up proof.
- [ ] 2.45 `/portal/api/proof/disabled-at-once`: Disabled at once.
- [ ] 2.46 `/portal/api/proof/password-or-role-change`: A password or role change ends proofs.
- [ ] 2.47 `/portal/api/proof/logout`: Logout.
- [ ] 2.48 `/portal/api/proof/not-an-engine-session`: Not a session of the engine.
- [ ] 2.49 `/portal/api/proof/stored-as-hash`: Stored as a hash.
- [ ] 2.50 `/portal/api/fields/search-result`: A search result's members (the member names of every operation's answer are compared with a list in the test).
- [ ] 2.51 `/portal/api/fields/nothing-of-inner-workings`: Nothing of the library's inner workings (a library with marked file, release, indexer, client, profile and folder names; no answer holds a mark).
- [ ] 2.52 `/portal/api/fields/who-asked-setting`: Who asked follows the setting.
- [ ] 2.53 `/portal/api/fields/only-the-callers`: Only the caller's requests.
- [ ] 2.54 `/portal/api/art/by-title`: A poster by title.
- [ ] 2.55 `/portal/api/art/no-address`: No address is taken.
- [ ] 2.56 `/portal/api/art/not-held`: Not held.
- [ ] 2.57 `/portal/api/search/a-search`: A search.
- [ ] 2.58 `/portal/api/search/asked-once`: Asked once.
- [ ] 2.59 `/portal/api/search/provider-unreachable`: The provider is unreachable.
- [ ] 2.60 `/portal/api/search/too-long`: Too long.
- [ ] 2.61 `/portal/api/search/browse-asks-nobody`: Browse asks nobody.
- [ ] 2.62 `/portal/api/limits/one-password`: Guessing one password.
- [ ] 2.63 `/portal/api/limits/across-names`: Guessing across names.
- [ ] 2.64 `/portal/api/limits/reported-addresses`: Reported addresses clear nothing.
- [ ] 2.65 `/portal/api/limits/search-budget`: The search budget.
- [ ] 2.66 `/portal/api/limits/daily-ceiling`: The portal's daily ceiling.
- [ ] 2.67 `/portal/api/limits/quota-not-the-clients`: Quota does not depend on the client.
- [ ] 2.68 `/portal/api/device/owner-gets-in` and `/portal/api/device/altered`: the daemon's half of "A remembered browser is not locked out by a stranger".

Tests, `web-auth` specification:

- [ ] 2.70 `/web/auth/requester-at-engine-login`: A requester at the engine's login.
- [ ] 2.71 `/web/auth/requester-created`: Created by an administrator.
- [ ] 2.72 `/web/auth/no-session-for-requester`: No session for a requester (every route of the main listener).
- [ ] 2.73 `/web/auth/role-change-ends-sessions`: A role change ends sessions.
- [ ] 2.74 `/web/auth/last-admin-not-requester`: The last administrator.
- [ ] 2.75 `/portal/api/class/default-deny`: Default deny holds.
- [ ] 2.76 `/portal/api/class/throttled`: Throttled.
- [ ] 2.77 `/portal/api/class/new-route-closed`: A new route is closed to the token.
- [ ] 2.78 `/portal/api/sessions/kinds-do-not-cross`: Kinds do not cross.
- [ ] 2.79 `/web/auth/portal-sessions-ended`: Ended by an administrator.
- [ ] 2.80 `/portal/api/hashing/admin-during-flood`: An administrator logs in during a flood.
- [ ] 2.81 `/portal/api/hashing/queue-bounded`: The portal's queue is bounded.

Gate:

- [ ] 2.90 Independent review of P2: the gate's order, the role's three barriers, the proof, the object-level checks, the answers' members. Record it in `design.md`; fix each finding with a test.

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
