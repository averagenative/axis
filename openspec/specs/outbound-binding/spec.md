# outbound-binding Specification

## Purpose
Sending Axis's own outbound connections through one named network interface (a VPN tunnel), and refusing them outright while that interface is missing, down or without an address, so that nothing leaves by another route; and stating plainly which traffic is covered and which (name resolution, the database, loopback) is not.
## Requirements
### Requirement: The value names an interface or an address
The daemon SHALL read `outbound.bind` (`AXIS_OUTBOUND_BIND`) as empty (no binding, the default), an IPv4 or IPv6 address, or an interface name of 1 to 15 printable characters without a space, `/` or `:`, and `outbound.bind_scope` (`AXIS_OUTBOUND_BIND_SCOPE`) as `internet` (the default) or `all`. A value of another form SHALL stop the start with a message naming the setting and what is expected. The unspecified, loopback, multicast and broadcast addresses, an IPv6 link-local address, an address with a zone and an IPv4 address written as IPv6 SHALL be refused. Whether the interface exists SHALL NOT be checked at start.

#### Scenario: An interface name
- **WHEN** `AXIS_OUTBOUND_BIND=tun0` and no interface tun0 exists yet
- **THEN** the daemon starts, and `/readyz` answers ready once the database is

#### Scenario: A value of the wrong form
- **WHEN** `AXIS_OUTBOUND_BIND="tun 0"`
- **THEN** the daemon does not start, and its message names `AXIS_OUTBOUND_BIND` and gives tun0 and wg0 as examples

### Requirement: The interface's state decides
For each connection the binding covers, the daemon SHALL read the interface's state as the connection is about to be made: by name, the device of that name; by address, the device that holds that address. The interface SHALL count as up only when it is up and running (has carrier), and its addresses SHALL be its IPv4 address and an IPv6 address that is not link-local (by address: that address only).

#### Scenario: A VPN reconnects with a new address
- **WHEN** tun0 comes back with another address
- **THEN** the next connection is tied to the new address

### Requirement: A covered connection leaves by the interface only
A connection the binding covers, while the interface is up with an address of the connection's family, SHALL be tied to the interface itself and to that address before it is made, so that its packets leave by that interface only, from that address, whatever the routing table says, and are not sent by another interface when the interface goes down during the connection.

#### Scenario: Up, by name
- **WHEN** `AXIS_OUTBOUND_BIND=tun0`, tun0 is up with 10.8.0.2, and the default route is another interface
- **THEN** an indexer search leaves by tun0 from 10.8.0.2, and nothing leaves by the default route

#### Scenario: Up, by address
- **WHEN** `AXIS_OUTBOUND_BIND=10.8.0.2` and tun0 holds it
- **THEN** a metadata request leaves by tun0 from 10.8.0.2

#### Scenario: The interface goes down during a connection
- **WHEN** tun0 goes down while a connection through it is being made
- **THEN** none of its packets leaves by the default route

### Requirement: Fail closed when the interface is not there
A connection the binding covers SHALL NOT be made while the interface is missing, down or without an address, or while the system refuses to tie a socket to it; the request SHALL fail at once with a message saying why ("the outbound interface tun0 is down; nothing was sent to the indexer") and nothing SHALL be sent by another route. A connection of a family the interface has no address of SHALL NOT be made; the server's other addresses SHALL still be tried.

#### Scenario: Nothing leaves while the interface is down
- **WHEN** tun0 is down and an indexer search is made
- **THEN** it fails at once saying "the outbound interface tun0 is down", and no packet leaves by any interface

#### Scenario: The interface does not exist
- **WHEN** no interface tun0 exists
- **THEN** a covered request fails at once saying "the outbound interface tun0 does not exist", and nothing is sent

#### Scenario: No address of the server's family
- **WHEN** the setting is an IPv4 address and the server has only an IPv6 address
- **THEN** the request fails saying the outbound address is IPv4, and nothing is sent

### Requirement: Which traffic is bound
With scope `internet`, the binding SHALL cover the indexers' connections (searches, release files, an indexer's own site after a redirect) and the metadata providers' (data and artwork) to public addresses; with scope `all`, every connection of the daemon's HTTP client, the download clients' and media servers' included, to any address. No scope SHALL cover a connection to a loopback address, the database's connections or name lookups.

#### Scenario: A Prowlarr on the same network
- **WHEN** the scope is `internet` and the indexer is at 192.168.77.50
- **THEN** its searches are not tied to the interface and are made while it is down

#### Scenario: Scope all
- **WHEN** the scope is `all`
- **THEN** a download client's connection leaves by the interface, and is refused while it is down

### Requirement: The state is shown, and what is not covered
A System page, Outbound network, SHALL show the setting, its scope, the interface's state and addresses read when the page is opened, whether the system allows tying a connection to an interface, and every kind of traffic with whether it is bound under the scope in effect, the database, name lookups (DNS), loopback and workers shown as never bound. Only administrators SHALL see it.

#### Scenario: Missing interface on the page
- **WHEN** an administrator opens the page while the interface does not exist
- **THEN** it says "Missing", gives the reason, and says that covered connections are refused and nothing is sent another way

#### Scenario: Not set
- **WHEN** no binding is set
- **THEN** the page says so and how to set it, and shows nothing as bound

### Requirement: Changes are logged once
The daemon SHALL log the interface's state once at start and once each time it changes (read at least every 10 seconds while the daemon runs, and at each covered connection), and SHALL NOT log it per request. Neither start-up nor readiness SHALL depend on it.

#### Scenario: Repeated reads
- **WHEN** the interface stays missing across many reads
- **THEN** one warning is logged

### Requirement: A fetch of site definitions is bound as an indexer's traffic is
The requests an administrator's fetch of site definitions makes to Jackett's repository SHALL be of a traffic kind of their own, and the "internet" scope SHALL bind that kind to the outbound interface as it binds an indexer's traffic. When the interface is missing, down or without an address, such a request SHALL fail saying so, and nothing SHALL be sent another way.

#### Scenario: The interface is down
- **WHEN** `outbound.bind` names an interface that is down and an administrator asks for the list of Jackett's definitions
- **THEN** the request is not made, and the page says the outbound interface is down

