## MODIFIED Requirements

### Requirement: Workers page
The daemon SHALL provide a page, for administrators, listing workers with their state, versions, capabilities, accepted volumes and current work, and allowing a worker to be created, revoked or asked to measure again. It SHALL offer two ways to create a worker: "Attach a worker", a guide, and "Add a worker without the guide", which asks for a name and shows the token once. Its description of what workers do SHALL be true for the effective `convert.checks_where`: who encodes, and who checks converted files. It SHALL say when conversion work is held after a restart and offer to resume it.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page or an action
- **THEN** the response is 403

#### Scenario: Where checks run, said
- **WHEN** `convert.checks_where` is `worker_only`
- **THEN** the page says that workers encode and check converted files, and that this server does no heavy work

#### Scenario: Held after a restart
- **WHEN** conversion work is held after a restart
- **THEN** the page says so and offers to resume it

#### Scenario: Both ways to create a worker
- **WHEN** an administrator opens the page
- **THEN** it links to "Attach a worker" and has the form "Add a worker without the guide"

## ADDED Requirements

### Requirement: Attach a worker
The daemon SHALL provide a guide of plain pages and forms, for administrators, that leads from a description of the worker's machine to the worker's first registration: (1) the worker's name, its container tool (Docker, Docker Compose, Podman, Podman Quadlet), its encoder (an NVIDIA GPU, an Intel or AMD GPU through VA-API, the CPU only), its processor family as information only, and the address the worker reaches Axis at; (2) the disks Axis knows, each to select, with how it is mounted on that machine (a disk of that machine, NFS, sshfs, SMB) and its path there; (3) the commands that mount each selected disk; (4) the worker, its token and the text that starts it; (5) what the worker reported. Each step SHALL be reachable again from the step after it with every choice kept. Every route of the guide SHALL be for administrators only, and every request that is not a GET SHALL pass the CSRF check before anything is read or done. Each route SHALL check the input of every step before its own again and SHALL show the first step whose input is not good, with a sentence that says what is wrong, and SHALL create nothing then.

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests any page or form of the guide
- **THEN** the response is 403 and nothing is created

#### Scenario: A form without the session's CSRF token
- **WHEN** an administrator's session posts a step of the guide without its CSRF token
- **THEN** the response is 403 and nothing is created

#### Scenario: Going back
- **WHEN** an administrator goes from the third step back to the second and the first
- **THEN** each page shows what was chosen before, and no worker exists yet

#### Scenario: The browser's address is offered
- **WHEN** an administrator opens the first step
- **THEN** the address field holds the scheme and host the browser used to reach Axis, and the page prints nothing else of the host Axis runs on

#### Scenario: Plain http to another machine
- **WHEN** the address is plain http to a host that is not the worker's own machine and the person has not said that the network is trusted
- **THEN** the first step is shown again saying that a worker refuses to send its token that way

#### Scenario: A disk without a marker
- **WHEN** a disk Axis knows has no marker yet
- **THEN** the second step names it, says that no worker can recognise it, and does not let it be selected

#### Scenario: A name that is taken
- **WHEN** the name is that of an existing worker, revoked ones included
- **THEN** the first step is shown again saying so, at whichever step it was posted, and no token is shown

### Requirement: The token is created at the step that shows it
The guide SHALL create the worker and its token only when its fourth step is posted with input that passes every check, and SHALL show the token on that one answer only, with the instruction to put it in a file only that user can read. No page, form field or address of an earlier step SHALL hold a token, and the address of the fifth step SHALL name only the worker, its encoder and its disks. The token SHALL NOT appear in the text the guide generates, in any log line, or in any event.

#### Scenario: Nothing before the fourth step
- **WHEN** an administrator has posted the first three steps
- **THEN** no worker has been created

#### Scenario: Shown once
- **WHEN** the fourth step is posted
- **THEN** the page shows the token once, only its hash is stored, and the token works for the worker API

#### Scenario: Posted again
- **WHEN** the fourth step's form is posted a second time
- **THEN** no token is shown and no second worker is created

### Requirement: The text that starts a worker comes from one place
The text the guide gives a person to run or save SHALL be made by one module that is never given the token: a `docker run` or `podman run` command, a `compose.yaml` service, or a Quadlet `.container` file, with the worker's name, the address, the disks, the disks without file identities and the encoder filled in. Every form of it SHALL run the container read-only, without capabilities, without new privileges, with a limit on its processes and a tmpfs for `/tmp`; SHALL name the image of the daemon's own version; SHALL name the token's file and never a token; SHALL list as disks without file identities exactly the disks mounted with sshfs or SMB; and SHALL pass a GPU only when one was chosen. Nothing a person typed SHALL reach the text unless it has the form the module checks: a worker's name, an http or https address of a plain form, and absolute paths of letters, digits and `/ . _ -`. A plan that does not pass SHALL get no text. The mount commands SHALL use placeholders for the server's address, the path or share on it and the user, and SHALL NOT hold anything of the host Axis runs on.

#### Scenario: Every combination
- **WHEN** the text is generated for each of the four tools, each of the three encoders and each kind of mount, alone and mixed
- **THEN** each text has the hardening options, the image's tag is the version given, the token's file is named and no token is present, and the disks without file identities are exactly the sshfs and SMB ones

#### Scenario: Characters that mean something to a shell
- **WHEN** a name, an address or a path holds a quote, a space, a line feed, `$`, `;`, `%` or a backslash
- **THEN** the step it was typed on is shown again and no text is generated

#### Scenario: A disk mounted with NFS only
- **WHEN** every selected disk is a disk of that machine or mounted with NFS
- **THEN** the text does not set `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`

### Requirement: The guide shows what the worker reported
The guide's last page SHALL show, for the worker it was opened for: before a registration, that it waits, and after two minutes without one what to check (the address, the token, a reverse proxy); a worker refused for its protocol version, saying so; after a registration, the worker's state, its version, its ffmpeg version, the encoders of the chosen family it measured as usable, and each selected disk as proved or not, with a plain reason and what to do for a disk that is not proved and for a chosen encoder family with no usable encoder. When the worker is connected, has a usable encoder of the chosen family and has proved every selected disk, the page SHALL say so and link to the sample measurement on a worker and to planning a conversion run. The daemon SHALL publish an event without content, on a topic only administrators may follow, each time a registration is accepted; the page SHALL ask for its changing part again on that event, and every few seconds until everything is good, so that it is complete without the event.

#### Scenario: Not registered yet
- **WHEN** the last page is opened for a worker created a moment ago that has not registered
- **THEN** it says that it waits and that it updates by itself

#### Scenario: Not registered after a while
- **WHEN** the worker was created more than two minutes ago and has not registered
- **THEN** the page says to check the address, the token and a reverse proxy

#### Scenario: One disk not proved
- **WHEN** the worker registers with one of two selected disks
- **THEN** an event is published, and the page shows one disk as proved and the other as not proved, with the path where the marker was expected and what to do

#### Scenario: No usable encoder of the chosen family
- **WHEN** the worker reports no usable encoder of the family chosen in the guide
- **THEN** the page says so, says what that GPU needs on the machine, and shows why each encoder failed

#### Scenario: All good
- **WHEN** the worker is connected, has a usable encoder of the chosen family and has proved every selected disk
- **THEN** the page says the worker is attached, links to the sample measurement and to planning a run, and no longer asks by itself

#### Scenario: Not an administrator on the topic
- **WHEN** a user who is not an administrator asks to follow the workers' topic
- **THEN** the response is 403
