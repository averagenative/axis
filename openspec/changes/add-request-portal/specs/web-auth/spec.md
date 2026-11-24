# web-auth Specification (delta)

## MODIFIED Requirements

### Requirement: Login
The daemon SHALL create a session for a correct username and password of an enabled account whose role is administrator or ordinary user, and SHALL respond identically to an unknown username, a disabled account, a wrong password and an account whose role is requester.

#### Scenario: Successful login
- **WHEN** a client submits the correct credentials of an enabled account
- **THEN** a session cookie is set and the client is sent to the page it originally requested

#### Scenario: Indistinguishable failures
- **WHEN** a client submits an unknown username, a disabled account's credentials, or a wrong password
- **THEN** each response has the same status and the same message

#### Scenario: Username case
- **WHEN** a user created as `alice` logs in as `Alice`
- **THEN** the login succeeds

#### Scenario: A requester at the engine's login
- **WHEN** a client submits the correct credentials of an enabled requester to the engine's login page
- **THEN** the response has the same status and message as a wrong password, and no session is made

## ADDED Requirements

### Requirement: The requester role
There SHALL be a third role, requester, below the ordinary user. A requester SHALL have no access to the engine's interface at all: no session of the engine's interface SHALL exist for a requester, and every route of the main listener that is not public SHALL refuse one, whatever the route's own class. An administrator SHALL create a requester on the Users page and SHALL be able to change an account's role among the three. A change of an account's role SHALL end every session and every portal session proof of that account at once. A requester's password is typed into the portal, so a change of a requester to another role SHALL set a new password in the same step, held to the rule of the new role and not the password the account has; without one the change SHALL be refused. The rule that an enabled administrator always exists SHALL be unchanged.

#### Scenario: Created by an administrator
- **WHEN** an administrator creates the account `robin` with the role requester
- **THEN** the account exists with that role and with both automatic-approval switches off

#### Scenario: No session for a requester
- **WHEN** a session row exists for an account whose role is requester (written from outside, or left from before a role change)
- **THEN** a request that presents its cookie is treated as unauthenticated on every route

#### Scenario: A role change ends sessions
- **WHEN** an administrator makes a logged-in ordinary user a requester
- **THEN** that user's next request to the engine's interface is sent to the login page, and logging in there is refused

#### Scenario: A requester given another role
- **WHEN** an administrator makes the requester `vic` an ordinary user
- **THEN** the change is refused without a new password or with the one `vic` has, and once it is made with a new one, the password `vic` typed into the portal fails at the engine's login

#### Scenario: The last administrator
- **WHEN** an attempt is made to make the only enabled administrator a requester
- **THEN** it is refused

### Requirement: Portal API credential class
Routes under the portal API SHALL require a portal token and SHALL NOT accept a session or a worker token; every other route SHALL NOT accept a portal token. Failed portal tokens SHALL be throttled per client address. No single credential SHALL be able to act as an arbitrary account: a portal token acts for a person only together with that person's session proof.

#### Scenario: Default deny holds
- **WHEN** a request without any credential reaches the portal API on its listener
- **THEN** it is refused with 401 and no redirect

#### Scenario: Throttled
- **WHEN** wrong tokens are presented repeatedly from one address
- **THEN** further attempts from that address are delayed or refused for a time

#### Scenario: A new route is closed to the token
- **WHEN** a module registers a route and does nothing about authentication
- **THEN** that route refuses a portal token, on both listeners

### Requirement: Portal sessions are kept apart from the engine's
A session made by a login through the portal API SHALL be stored with its kind, only as a hash of its proof, with the account's epoch, and SHALL be accepted only by the portal API. A session of the engine's interface SHALL be accepted only by the engine's interface. Both kinds SHALL expire and be revoked by the same rules, and an administrator SHALL see an account's portal sessions on the Users page and SHALL be able to end them.

#### Scenario: Kinds do not cross
- **WHEN** a portal session's proof is presented as the engine's session cookie, and an engine session's cookie value as a proof
- **THEN** both are refused

#### Scenario: Ended by an administrator
- **WHEN** an administrator ends `robin`'s portal sessions on the Users page
- **THEN** each of their proofs is refused from the next request on

### Requirement: Password hashing keeps a place for the engine's own logins
Password checks that the portal API asks for SHALL use at most one of the daemon's concurrent password hashes and SHALL wait in a queue of their own of at most 8, beyond which `login` is answered 503. A flood of logins through the portal API SHALL NOT delay or refuse a login at the engine's own interface.

#### Scenario: An administrator logs in during a flood
- **WHEN** logins arrive through the portal API faster than they can be checked, and an administrator logs in at the engine's interface
- **THEN** the administrator's login is checked without waiting behind them and succeeds

#### Scenario: The portal's queue is bounded
- **WHEN** more than 8 portal logins wait
- **THEN** further ones are answered 503 and no password hash is started for them

### Requirement: An address that speaks as the portal is not let in as a person
The daemon SHALL remember, for 24 hours after the last time, each address from which a valid portal token was presented (the address of the connection, an IPv6 address by its /64; at most 256 of them, the one unused longest forgotten for a new one), and SHALL refuse a login at the engine's own interface whose client address, as the main listener resolves it through its trusted proxies, is such an address, with the same answer as a wrong password, without checking the password and without counting a failure against anyone. A token that is not valid SHALL make no address such an address. A session that already exists SHALL not be affected. The refusal SHALL be written to the audit log with its reason.

#### Scenario: A login from the portal's address
- **WHEN** a portal has called the portal API from an address, and a login with an administrator's right password then arrives at the engine's login page from that address
- **THEN** it is refused as a wrong password would be, and no session is made

#### Scenario: Another address
- **WHEN** the same administrator logs in from another address
- **THEN** the login succeeds

#### Scenario: A day after the portal's last request
- **WHEN** 24 hours have passed since the last request with a valid portal token from an address
- **THEN** a login from that address is checked as any other
