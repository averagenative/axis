# deployment Specification (delta)

## ADDED Requirements

### Requirement: The portal ships in the image and runs with nothing
The image SHALL contain `axis-portal`, run with `--entrypoint /usr/bin/axis-portal`. As the portal, a container SHALL work as a non-root user, under an arbitrary user id, with a read-only root filesystem, all capabilities dropped, no new privileges, no volume and no writable directory, and with one secret, the portal token, supplied as a file. The portal SHALL have a health check mode (`axis-portal --healthcheck`) that needs no token.

#### Scenario: Hardened and empty
- **WHEN** the image runs `axis-portal` with a read-only root filesystem, no capability, an arbitrary user id, no volume, no tmpfs and its token as a secret file
- **THEN** it starts, its health check passes, and a person can log in and ask through it

#### Scenario: Nothing of the daemon's is needed
- **WHEN** the portal's container is given no database address, no media mount and no other secret
- **THEN** it works, and its environment and its mounts hold nothing but its own settings and its token

### Requirement: An optional portal service in the examples
The Compose example with a reverse proxy, the Kubernetes example and the Quadlet example SHALL each hold an optional portal service that is left out unless a person switches it on, with instructions. In each, the portal SHALL be the only Axis service that the reverse proxy for the internet reaches; the portal SHALL share with the daemon a network that holds those two alone; the portal SHALL NOT be on the network of PostgreSQL or of the download clients and SHALL have no media mount; in the Compose and Quadlet examples every network of the portal SHALL be internal (no route out of it), because a container on an ordinary network reaches the host's other container networks by address; and the daemon's main port SHALL be published as before, not through the internet-facing proxy. Where the platform can limit a connection by port (the Kubernetes NetworkPolicy), the portal SHALL be allowed to reach the daemon's portal port only and to be reached by the proxy only. Where the daemon can be given a fixed address on each network (the Compose example), the portal API SHALL listen on the daemon's address on the network it shares with the portal, and the main listener on its address on the proxy's network, so that the portal's container reaches the portal port and not the main port. Where neither can be done (the Quadlet example), the example SHALL say that the portal's container can open the daemon's login page, and that a login from it is refused. Each example SHALL tell the daemon the portal's addresses (`AXIS_PORTAL_PEERS`), and nothing wider: in the Compose and Quadlet examples the portal SHALL have a fixed address on each of its networks, and every address of a network that holds a fixed one SHALL be fixed; in the Kubernetes example the manifests SHALL say that the portal's pods need a range of their own from the network plugin and what holds on a cluster that does not enforce NetworkPolicy. Where the reverse proxy shares a network with the portal and also routes to the engine (the Compose example), the proxy SHALL refuse the portal's address whatever it asks for, by a rule that does not depend on the portal's container running or being healthy, and the daemon's list SHALL hold the address the proxy would name as the client. Because the daemon and the proxy know the portal by the addresses the example fixes, the portal's container SHALL have no other: in the Compose and Quadlet examples IPv6 SHALL be switched off in the portal's container by sysctl, so that its interfaces have no IPv6 link-local address, and the portal's networks SHALL have no IPv6 subnet; the proxy's rule SHALL refuse IPv6 link-local clients as well; and the Kubernetes example SHALL say why it cannot switch IPv6 off by default and what stands in for it. In the Kubernetes example as shipped, the portal's range SHALL NOT lie inside the trusted proxies' range. Each example SHALL say that the token and the passwords of requesters cross the network between the portal and the daemon unencrypted, and why that network holds only those two.

#### Scenario: The portal cannot reach the database
- **WHEN** the Compose example runs with the portal switched on, and a connection to PostgreSQL's port is tried from the portal's container, by PostgreSQL's name and by its address
- **THEN** the name does not resolve, the connection by address is not made, and the portal's container has no default route

#### Scenario: A request end to end
- **WHEN** a requester logs in at the portal through the proxy, over HTTPS, and asks for a movie
- **THEN** the request waits on the daemon's Requests page

#### Scenario: The engine is not behind the portal's proxy
- **WHEN** the engine's setup page (`/setup`), `/admin/users`, `/storage`, `/readyz`, `/api/v1/worker/claim` and `/api/v1/portal/hello` are requested from the address the portal is published at
- **THEN** each is answered 404 by the portal, and the page at `/login` there is the portal's own login form, not the engine's

#### Scenario: The engine is not reached from the portal's container through the proxy
- **WHEN** the Compose example runs with the portal switched on, and from the portal's container the administrator's right password is posted to the reverse proxy with the engine's host name
- **THEN** the proxy refuses the request, no session is made, and the same login from a container that is not the portal's succeeds

#### Scenario: The portal's container has no IPv6 address
- **WHEN** the Compose example or the Quadlet example runs with the portal switched on
- **THEN** the portal's container has no IPv6 address but loopback's, and from inside it no IPv6 connection can be made to an IPv6 address of the reverse proxy (Compose) or of the daemon (Quadlet)

#### Scenario: The proxy's refusal outlasts the portal's container
- **WHEN** the Compose example runs with the portal switched on and the portal's container is stopped, so that the proxy has dropped the portal's own route
- **THEN** a request from the portal's address on the network it shares with the proxy, for the engine's host name with the administrator's right password, is still refused by the proxy

#### Scenario: A login the proxy forwards for the portal's address
- **WHEN** the reverse proxy forwards a login with the administrator's right password for a client whose address the daemon was told is a portal's
- **THEN** the daemon refuses it as a wrong password, and its log says why

#### Scenario: The main port is open to the portal in the Quadlet example
- **WHEN** the Quadlet example runs with the portal switched on, and from the portal's container the administrator's right password is posted to the daemon's main port
- **THEN** the login page is served, the answer is that of a wrong password, no session is made, and the daemon's log names the portal's address and the setting

#### Scenario: Left out by default
- **WHEN** an example is brought up as it is published
- **THEN** no portal runs and the daemon has no portal listener

#### Scenario: Kubernetes
- **WHEN** the manifests are applied with the portal's
- **THEN** the portal becomes ready, and its NetworkPolicy allows it the daemon's portal port and nothing else of the cluster

### Requirement: The guide says what the proxy in front of the portal must do
`deploy/README.md` SHALL say that the portal must be published only through a reverse proxy that terminates TLS, and what that proxy must do because the portal cannot: redirect plain HTTP, send HSTS, limit the size of a request and the time for its head and body, limit connections for each address, and pass `X-Forwarded-For`, `X-Forwarded-Proto` and `X-Forwarded-Host`. It SHALL say that the proxy must be listed in the portal's trusted proxies (`AXIS_PORTAL_TRUSTED_PROXIES`, with the rules of the daemon's `server.trusted_proxies`), and that without it every client shares one login throttle and no login is accepted, because the portal does not then know that the request came over HTTPS.

#### Scenario: The proxy is listed
- **WHEN** the example's proxy is listed and a login arrives through it over HTTPS
- **THEN** the portal uses the forwarded client address for its throttle and accepts the login

#### Scenario: The proxy is not listed
- **WHEN** the proxy is not listed
- **THEN** the portal serves no login form, says that it needs HTTPS, and its log says once that forwarded headers came from an address that is not trusted

### Requirement: The guides say whether the portal has been reviewed for the internet
Until an independent security review of the portal, the portal API and the requester role has been made and its findings fixed, `deploy/README.md`, the portal's part of each example and the user guide SHALL say, where they tell how to run the portal, that it has not been reviewed for exposure to the internet and should be reached only from a network you trust, and `axis-portal` SHALL log the same once at start. After the review, those places SHALL name the review, say what it covered and what it left, and the line at start SHALL go.

#### Scenario: Before the review
- **WHEN** the portal's documents are read and the portal is started, in a tree where the review is not recorded
- **THEN** each says that the portal is not reviewed for the internet

#### Scenario: After the review
- **WHEN** the review is recorded
- **THEN** the documents say what it covered and what it left, and the portal starts without that line
