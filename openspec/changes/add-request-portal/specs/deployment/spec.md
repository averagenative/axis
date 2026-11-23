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
The Compose example with a reverse proxy, the Kubernetes example and the Quadlet example SHALL each hold an optional portal service that is left out unless a person switches it on, with instructions. In each, the portal SHALL be the only Axis service that the reverse proxy for the internet reaches; the portal SHALL share with the daemon a network that holds those two alone; the portal SHALL NOT be on the network of PostgreSQL or of the download clients and SHALL have no media mount; and the daemon's main port SHALL be published as before, not through the internet-facing proxy. Where the platform can limit a connection by port (the Kubernetes NetworkPolicy), the portal SHALL be allowed to reach the daemon's portal port only and to be reached by the proxy only. Each example SHALL say that the token and the passwords of requesters cross the network between the portal and the daemon unencrypted, and why that network holds only those two.

#### Scenario: The portal cannot reach the database
- **WHEN** the Compose example runs with the portal switched on, and a connection to PostgreSQL's port is tried from the portal's container
- **THEN** the name does not resolve or the connection is not made

#### Scenario: A request end to end
- **WHEN** a requester logs in at the portal through the proxy, over HTTPS, and asks for a movie
- **THEN** the request waits on the daemon's Requests page

#### Scenario: The engine is not behind the portal's proxy
- **WHEN** the engine's login page, `/admin/users` and `/api/v1/portal/hello` are requested from the address the portal is published at
- **THEN** each is answered 404 by the portal

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
