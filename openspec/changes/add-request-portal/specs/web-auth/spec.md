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
The portal's addresses SHALL be one static setting of the daemon, `server.portal_peers` (`AXIS_PORTAL_PEERS`): a list of IP addresses and CIDR ranges, empty by default, in which a host name SHALL be refused at start. A range SHALL be as wide as it is written (an IPv6 address SHALL NOT be widened to its /64), and an IPv4-mapped IPv6 address SHALL count as its IPv4 address. The daemon SHALL refuse a login at the engine's own interface, and the first setup, when the address of the connection is in the list, when the client's address, as the main listener resolves it through its trusted proxies, is in the list, or when any address in the request's `X-Forwarded-For` is in the list, whoever sent that header and wherever in it the address stands. While the list is not empty, the daemon SHALL also refuse a login and the first setup when one of those addresses is an IPv6 link-local address (`fe80::/10`, with or without a zone), and when the request carries `X-Forwarded-For`, its connection is not from a trusted proxy, and a host name among the trusted proxies stands for no address (not looked up yet, or failing past its grace). While the list is empty, none of this SHALL apply. An address taken from a header SHALL reach the log only as the daemon writes it again, never as the header's own text. The refusal SHALL be the answer of a wrong password (of a wrong setup token), without checking a password and without counting a failure against anyone. A session that already exists SHALL NOT be affected. Nothing SHALL be learnt from what a portal does and nothing SHALL expire: neither a portal that stops calling nor a restart of the daemon SHALL end the rule. The daemon SHALL write a line to its log that names the address and the setting, at once for the first refusal and then at most once a minute with the number it left out, and the page "Request portal" SHALL show when a login was last refused for this reason. When an address is both in this list and among the trusted proxies, the daemon SHALL warn at start and SHALL show a notice under "Worth a look" that says what to change.

#### Scenario: A login from the portal's address
- **WHEN** `server.portal_peers` holds an address, no portal has called yet, and a login with an administrator's right password arrives at the engine's login page on a connection from that address
- **THEN** it is refused as a wrong password would be, no password is checked, no failure is counted and no session is made

#### Scenario: Another address
- **WHEN** the same administrator logs in from an address that is not in the list, directly and with no proxy between
- **THEN** the login succeeds

#### Scenario: The client a trusted proxy names is the portal's
- **WHEN** a login arrives from a trusted proxy that is not in the list, with `X-Forwarded-For` naming an address that is
- **THEN** it is refused as a wrong password would be, and the log names the client's address and the setting

#### Scenario: A forged X-Forwarded-For from the portal's own address
- **WHEN** an address is both in the list and a trusted proxy, and a login arrives from it with `X-Forwarded-For` naming an address that is in neither
- **THEN** it is refused as a wrong password would be

#### Scenario: A proxy that adds to X-Forwarded-For
- **WHEN** an address is both in the list and a trusted proxy, it sends a login with `X-Forwarded-For` naming an address that is in neither to a second trusted proxy, and that proxy adds the first one's address to the header
- **THEN** the login is refused as a wrong password would be, although the client the trusted proxies arrive at is in neither list, and the log says that `X-Forwarded-For` names one of the portal's addresses

#### Scenario: An address in X-Forwarded-For beyond the client
- **WHEN** a login arrives from a trusted proxy with an `X-Forwarded-For` in which the client the trusted proxies arrive at is nobody's, and an address in front of it, which the walk never reaches, is in the list
- **THEN** it is refused as a wrong password would be

#### Scenario: X-Forwarded-For from a sender that is not trusted
- **WHEN** no proxy is trusted and a login arrives with `X-Forwarded-For` naming an address in the list
- **THEN** it is refused as a wrong password would be

#### Scenario: An IPv6 link-local address
- **WHEN** the list is not empty and a login arrives from a trusted proxy with `X-Forwarded-For: fe80::2%eth1`
- **THEN** it is refused as a wrong password would be, the log names `fe80::2` as an IPv6 link-local address, and the zone's text is not in the log

#### Scenario: A forwarded login while the proxy's name has no address
- **WHEN** the list is not empty, `server.trusted_proxies` names a host that has not been looked up yet, and a login with an administrator's right password arrives with `X-Forwarded-For` from an address that is not trusted
- **THEN** it is refused as a wrong password would be and the log says why; the same login without the header succeeds; and once the name has its address, the login through the proxy succeeds

#### Scenario: No portal, no rule
- **WHEN** the list is empty
- **THEN** no login is refused for an address: not from an IPv6 link-local address, and not with a forwarding header while a proxy's name has no address

#### Scenario: Nothing is learnt and nothing expires
- **WHEN** two days pass in which no portal calls the daemon, or the daemon restarts
- **THEN** a login from an address in the list is refused as before, and one from an address outside it is checked as before

#### Scenario: A range is as wide as it is written
- **WHEN** the list holds `192.0.2.16/28`, `2001:db8:1:2::7` and `::ffff:172.30.2.3`
- **THEN** `192.0.2.16` and `192.0.2.31` are the portal's and `192.0.2.15` and `192.0.2.32` are not, `2001:db8:1:2::7` is and `2001:db8:1:2::8` is not, and `172.30.2.3` is

#### Scenario: The first setup from the portal's address
- **WHEN** no account exists and the right setup token is posted from an address in the list
- **THEN** it is answered as a wrong setup token is, and no account is made

#### Scenario: The portal's addresses and the trusted proxies overlap
- **WHEN** an address is in `server.portal_peers` and among `server.trusted_proxies`
- **THEN** the daemon warns at start when the trusted proxy is written as an address or a range, and a notice under "Worth a look" names both entries and says what to change, also for a trusted proxy written as a host name once the name has its address

#### Scenario: An administrator who is refused finds out why
- **WHEN** an administrator's own login is refused because their browser arrives from an address in the list, from or by way of an IPv6 link-local address, or through a proxy whose name has no address yet
- **THEN** the daemon's log has a line that says which it was and names the address and the setting, and the guide says how to get back in by changing a line of configuration and restarting, without touching the database
