# web-auth Specification

## Purpose
Who may use the web interface and how that is proved: local accounts, first-run setup, sessions, protection against cross-site requests, login throttling, roles, and the rule that every route is closed unless explicitly public.
## Requirements
### Requirement: Default-deny routes
The daemon SHALL refuse every request without a valid session except requests to `/healthz`, `/login`, `/setup` and static assets.

#### Scenario: Unauthenticated page request
- **WHEN** a client without a session requests a protected page
- **THEN** it is redirected to the login page, with the requested path preserved for after login

#### Scenario: Unauthenticated htmx request
- **WHEN** a client without a session sends an htmx request to a protected path
- **THEN** the response is 401 and instructs htmx to navigate to the login page

#### Scenario: New routes are protected without opting in
- **WHEN** a module registers a route and does nothing about authentication
- **THEN** that route refuses requests without a session

#### Scenario: Open redirect refused
- **WHEN** login succeeds with a `next` value that is an absolute URL or points to another host
- **THEN** the user is sent to the dashboard instead

### Requirement: First-run setup
While no user account exists, the daemon SHALL allow creating the first administrator only to a client presenting the one-time setup token printed in the daemon log, and SHALL disable setup once any account exists.

#### Scenario: Setup with the token
- **WHEN** no user exists and a client submits the setup form with the correct token and a valid username and password
- **THEN** an administrator account is created and the client is logged in

#### Scenario: Setup without the token
- **WHEN** no user exists and a client submits the setup form with a missing or wrong token
- **THEN** no account is created

#### Scenario: Setup closed after first account
- **WHEN** at least one user exists and a client requests the setup page or submits the setup form
- **THEN** the response is 404 and no account is created

#### Scenario: Racing setup requests
- **WHEN** two clients submit valid setup forms at the same moment
- **THEN** exactly one administrator account exists afterwards

### Requirement: Password handling
The daemon SHALL store passwords only as argon2id hashes, SHALL require at least 10 characters, and SHALL never write a password or its hash to a log or a response.

#### Scenario: Stored form
- **WHEN** an account is created
- **THEN** the stored value is an argon2id hash and does not contain the password

#### Scenario: Short password refused
- **WHEN** a password shorter than 10 characters is submitted at setup, user creation or password change
- **THEN** it is refused with a message stating the minimum length

#### Scenario: Passwords absent from logs
- **WHEN** logins succeed and fail with debug logging enabled
- **THEN** no log line contains a submitted password or a stored hash

### Requirement: Login
The daemon SHALL create a session for a correct username and password of an enabled account, and SHALL respond identically to an unknown username, a disabled account and a wrong password.

#### Scenario: Successful login
- **WHEN** a client submits the correct credentials of an enabled account
- **THEN** a session cookie is set and the client is sent to the page it originally requested

#### Scenario: Indistinguishable failures
- **WHEN** a client submits an unknown username, a disabled account's credentials, or a wrong password
- **THEN** each response has the same status and the same message

#### Scenario: Username case
- **WHEN** a user created as `alice` logs in as `Alice`
- **THEN** the login succeeds

### Requirement: Login throttling
The daemon SHALL delay further login attempts after repeated failures for the same username or from the same client address (an IPv6 address by its `/64`), and SHALL say when the client may retry. An attempt whose password is being checked SHALL count as the failure it may be from the moment it is admitted until its check ends, so that no more attempts for one username, or from one address, are checked at once than would be let through one after another: the free failures and the one that starts the delay, then one at a time each time a delay ends. The current password asked for on the account page SHALL be throttled in the same way on the count of the session that asks, not on the username's. When the table of counts is full, it SHALL forget only a count that is neither in flight nor still in its delay.

#### Scenario: Repeated failures are throttled
- **WHEN** a client fails to log in to one username many times in quick succession
- **THEN** further attempts are refused with a retry time, even with the correct password, until the delay passes

#### Scenario: Success clears the counter
- **WHEN** a user logs in successfully after a few failures
- **THEN** later attempts for that username are not delayed

#### Scenario: A burst is no faster than one at a time
- **WHEN** twelve wrong passwords for one username are sent at once, each taking tens of milliseconds to check
- **THEN** at most six of them have their password checked, the others are refused with a retry time, and afterwards the right password is refused until the delay passes

#### Scenario: The current password in a burst
- **WHEN** a logged-in user sends twelve password changes at once, each with a wrong current password
- **THEN** at most six of them have the current password checked and the others are refused with a retry time

#### Scenario: A stranger does not stop a password change
- **WHEN** a stranger fails with a username until it is delayed, and the account's owner, logged in, changes the password with the right current password
- **THEN** the change is made

#### Scenario: A flood of keys does not reset a delay
- **WHEN** a key is in its delay and a hundred other keys fail once each in a table that holds twenty
- **THEN** the delayed key is still refused, and the table grows rather than forget it

### Requirement: Session cookie
The session cookie SHALL be unreadable to scripts, restricted to same-site requests, and marked secure whenever the request arrived over HTTPS or secure cookies are forced by configuration.

#### Scenario: Cookie attributes
- **WHEN** a session cookie is set
- **THEN** it has `HttpOnly`, `SameSite=Lax` and `Path=/`

#### Scenario: Secure over HTTPS
- **WHEN** login happens over HTTPS as reported by a trusted proxy
- **THEN** the cookie also has `Secure`

### Requirement: Persistent, revocable sessions
Sessions SHALL survive a daemon restart, SHALL expire after 30 days without use or 90 days in total, and SHALL stop working immediately when revoked.

#### Scenario: Session survives restart
- **WHEN** a logged-in client makes a request after the daemon restarts
- **THEN** the request is accepted without logging in again

#### Scenario: Logout
- **WHEN** a client logs out and then reuses its old cookie
- **THEN** the request is treated as unauthenticated

#### Scenario: Idle expiry
- **WHEN** a session has not been used for more than 30 days
- **THEN** it is no longer accepted

#### Scenario: Stored session is not a credential
- **WHEN** the session table is read from the database
- **THEN** it contains no value that can be used as a session cookie

#### Scenario: User revokes another session
- **WHEN** a user revokes one of their other sessions from the account page
- **THEN** that session stops working and the current one continues

### Requirement: Password change
A user SHALL be able to change their password by supplying the current one, and doing so SHALL end all of that user's other sessions.

#### Scenario: Change with correct current password
- **WHEN** a user submits their current password and a valid new one
- **THEN** the new password works, the old one does not, and the user's other sessions are ended

#### Scenario: Wrong current password
- **WHEN** a user submits an incorrect current password
- **THEN** the password is unchanged

### Requirement: CSRF protection
The daemon SHALL reject any state-changing request that does not carry the session's CSRF token or that the browser reports as cross-site.

#### Scenario: Missing token
- **WHEN** a logged-in client sends a POST without the CSRF token
- **THEN** the response is 403 and nothing changes

#### Scenario: Token from another session
- **WHEN** a POST carries a CSRF token issued to a different session
- **THEN** the response is 403

#### Scenario: Cross-site request
- **WHEN** a POST carries a valid token but an `Origin` header for another site
- **THEN** the response is 403

#### Scenario: htmx requests carry the token
- **WHEN** a page rendered by the shell issues an htmx POST
- **THEN** the request includes the CSRF token and is accepted

### Requirement: Roles
The daemon SHALL restrict administrative routes to administrators and SHALL always keep at least one enabled administrator.

#### Scenario: Non-admin refused
- **WHEN** a non-administrator requests an administrative route
- **THEN** the response is 403

#### Scenario: Last administrator is protected
- **WHEN** an attempt is made to disable, delete or demote the only enabled administrator
- **THEN** it is refused

#### Scenario: Disabling a user ends their sessions
- **WHEN** an administrator disables a user
- **THEN** that user's sessions stop working immediately

### Requirement: Trusted proxies
The daemon SHALL take the client address and scheme from forwarding headers only when the directly connected peer is a configured trusted proxy.

#### Scenario: Header from an untrusted peer is ignored
- **WHEN** a request from an address not in the trusted list carries `X-Forwarded-For`
- **THEN** the directly connected address is used as the client address

#### Scenario: Header from a trusted proxy is used
- **WHEN** a request from a trusted proxy carries `X-Forwarded-For` and `X-Forwarded-Proto: https`
- **THEN** the forwarded client address is used and the request is treated as HTTPS

### Requirement: A user's theme belongs to their account
A logged-in user SHALL be able to choose a theme on their account page; the choice SHALL persist across sessions and devices and SHALL affect only that user.

#### Scenario: Choice persists
- **WHEN** a user chooses a theme and later logs in from another browser
- **THEN** pages use the chosen theme

#### Scenario: Other users unaffected
- **WHEN** one user changes their theme
- **THEN** another user's pages are unchanged

#### Scenario: Forged request refused
- **WHEN** a theme change arrives without a valid CSRF token
- **THEN** it is refused and the theme is unchanged

### Requirement: Worker API credential class
Routes under the worker API SHALL require a worker token and SHALL NOT accept a session; every other non-public route SHALL continue to require a session and SHALL NOT accept a worker token. Failed worker tokens SHALL be throttled per client address.

#### Scenario: Default deny holds
- **WHEN** a request without any credential reaches the worker API
- **THEN** it is refused with 401 and no redirect

#### Scenario: Throttled
- **WHEN** wrong tokens are presented repeatedly from one address
- **THEN** further attempts from that address are delayed or refused for a time

### Requirement: A trusted proxy may be named
A host name in the trusted proxies SHALL be looked up through the system's resolver, and a request from any address the name has (IPv4 or IPv6, at most 16 of them, the lowest first) SHALL be treated as from a trusted proxy. A lookup SHALL never be waited for by a request: names are looked up asynchronously at start, every 30 seconds, and, at most once every 5 seconds, when a request arrives from an address that is not trusted while the list has a name. The unspecified and multicast addresses SHALL never be trusted.

#### Scenario: A named proxy is believed
- **WHEN** `traefik` resolves to an address and a request from that address carries `X-Forwarded-For` and `X-Forwarded-Proto: https`
- **THEN** the forwarded client address is used and the request is treated as HTTPS

#### Scenario: Not before the first lookup
- **WHEN** a request arrives from the named proxy's address before the first lookup has finished
- **THEN** its headers are ignored

#### Scenario: Both address families
- **WHEN** a name resolves to an IPv4 and an IPv6 address
- **THEN** a request from either is from a trusted proxy, and an IPv4-mapped IPv6 address is its IPv4 address

#### Scenario: A recreated proxy
- **WHEN** the named proxy gets a new address and a request arrives from it
- **THEN** that request's headers are ignored, a lookup starts without the request waiting for it, and the next request from the new address is believed and the old address is not

#### Scenario: Lookups are rate limited
- **WHEN** many requests arrive from untrusted addresses within five seconds
- **THEN** one round of lookups is started

#### Scenario: A lookup is slow
- **WHEN** a lookup has not answered
- **THEN** requests are decided at once, from the addresses known so far, and no second lookup of the name starts over the first

#### Scenario: Too many addresses
- **WHEN** a name resolves to more than 16 addresses
- **THEN** the lowest 16 are trusted (IPv4 before IPv6), the rest are not, and the log and the page say so

### Requirement: A name that does not resolve trusts nothing
A host name whose lookup has not yet worked, or fails, SHALL trust no address, except that when a lookup that worked starts failing the earlier addresses SHALL be kept for 30 seconds from the first failure and then dropped, whether or not another lookup has said so. Each change of a name's state (resolved to other addresses, failing with addresses kept, no longer trusted, resolved again) SHALL be logged once: at info level for an answer, at warning level for a failure.

#### Scenario: Never resolved
- **WHEN** a name has no addresses
- **THEN** no address is trusted for it, the log says so once, and the other entries are unaffected

#### Scenario: A blip
- **WHEN** a lookup fails 10 seconds after one worked
- **THEN** the earlier addresses are still trusted, and the log says they are kept for 30 seconds

#### Scenario: The grace ends
- **WHEN** lookups keep failing for 30 seconds
- **THEN** the earlier addresses are no longer trusted, and the next failed lookup logs that no address is trusted for the name

#### Scenario: It resolves again
- **WHEN** a lookup works after failures
- **THEN** its addresses are trusted at once and the log says what the name resolves to

### Requirement: The Reverse proxies page
Administrators SHALL see, on a System page "Reverse proxies" (`/admin/proxies`), each entry of the trusted proxies: for an address or range, itself; for a host name, its state (looking up, resolved, lookup failing, not resolved), the addresses it stands for now, when it was last looked up, and why it does not resolve; with the cap and the grace said where they apply. The page SHALL change nothing.

#### Scenario: A resolved name
- **WHEN** `edge-proxy` resolves to `127.0.0.1` and an administrator opens the page
- **THEN** it shows the entry as a host name, resolved, with `127.0.0.1` and how long ago it was looked up

#### Scenario: A name that fails
- **WHEN** a name does not resolve
- **THEN** the page says that no address is trusted for it and gives the resolver's reason

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator requests the page
- **THEN** the request is refused

### Requirement: A remembered browser has a count of its own
A successful login SHALL set a device cookie, signed by the daemon, that names the account and the account's epoch (which a password change or a disable advances); it SHALL be unreadable to scripts, sent only on same-site requests, marked secure as the session cookie is, renewed at each successful login and valid for 90 days after it was issued. The daemon SHALL record each account's remembered devices, at most 20 per account (the oldest forgotten first), and SHALL keep that record and the key in the data directory so that a browser stays remembered across restarts; when they cannot be kept there, browsers SHALL be remembered until the daemon stops.

A login attempt that presents a device cookie that is valid, recorded and of the account's current epoch, for an enabled account it names, SHALL be throttled on that device's own count of failures and on a count shared by all of the account's devices (ten free failures), with the same delays as a username, and SHALL NOT be held to the username's or the client address's count; its failures SHALL count against the device and the account's devices only, and its success SHALL clear those counts only. Any other attempt, and one whose device cookie is altered, signed by another key, expired, forgotten, of an earlier epoch or for another account, SHALL be throttled as before.

The trade-off is written here because the delay above stays: a stranger who keeps a known username locked, or who shares the owner's address (every client does behind a proxy that is not trusted), cannot lock the owner out of a browser the owner has logged in with since the last password change; a browser that never did, or whose cookie expired or was forgotten, or after the key was lost, waits like any stranger; and a stolen device cookie gives its holder guesses beside the strangers' count, bounded by the account's devices' shared count and ended by a password change, though never the account (the password is still needed).

#### Scenario: The owner is not locked out by a stranger
- **WHEN** a browser logged in to an account once, and a stranger at the same address then fails six times with that username and once each with fifteen other names, so that both the username and the address are delayed
- **THEN** the stranger is refused with a retry time even with the right password, and the owner logs in from the remembered browser at once; afterwards the stranger is still refused

#### Scenario: A remembered browser is throttled on its own count
- **WHEN** a remembered browser fails six times for its account
- **THEN** it is refused with a retry time even with the right password, and another browser logs in to the account at once

#### Scenario: The account's browsers share a ceiling
- **WHEN** three remembered browsers of one account fail eleven times between them, none more than four times
- **THEN** the next attempt from any of them is refused with a retry time, and a stranger's browser logs in to the account at once

#### Scenario: A password change ends remembered browsers
- **WHEN** a remembered browser's account changes its password, and a stranger then delays the username
- **THEN** the browser, with its old device cookie and the new password, is refused with a retry time like a stranger

#### Scenario: At most twenty per account
- **WHEN** twenty-one browsers log in to one account in turn, and a stranger then delays the username
- **THEN** twenty are recorded, the first is refused with a retry time, and the last logs in at once

#### Scenario: An altered or another account's device cookie
- **WHEN** the username is delayed, and a browser presents a device cookie with its signature changed, or one another account's login set
- **THEN** the attempt is refused with a retry time

#### Scenario: Remembered across a restart
- **WHEN** a browser logged in, the daemon restarts, and a stranger then delays the username
- **THEN** the browser logs in at once; the key file is 32 bytes and the record of devices is beside it, both readable by the daemon's user only, and a damaged key is replaced with a warning

#### Scenario: The cookie's attributes
- **WHEN** a login succeeds
- **THEN** the device cookie has `HttpOnly`, `SameSite=Strict`, `Path=/` and a lifetime of 90 days

### Requirement: A proxy that should be trusted is pointed out
While no trusted proxy is configured, when ten requests carrying `X-Forwarded-For` have all come from one private address (loopback, private, link-local or unique-local) and none from another, the daemon SHALL say once in its log, and on the Settings and Reverse proxies pages for as long as that holds, that every such request came from that address, that if it is a reverse proxy every client then shares one login throttle, and that it can be added to the trusted proxies. Whenever the connection limits have refused anything, those pages SHALL also say how many, the last address and why, and that a reverse proxy at that address belongs in the trusted proxies.

#### Scenario: Forwarded requests from one private address
- **WHEN** no proxy is trusted and twelve requests with `X-Forwarded-For` arrive from 127.0.0.1
- **THEN** the log says so once, naming 127.0.0.1 and `server.trusted_proxies`, and the Reverse proxies page says so too

#### Scenario: A trusted proxy
- **WHEN** 127.0.0.1 is a trusted proxy and the same requests arrive
- **THEN** nothing is said

#### Scenario: A refusal is pointed out
- **WHEN** the per-address cap refuses a connection
- **THEN** the hint says one was refused, from which address, and names `server.trusted_proxies`

