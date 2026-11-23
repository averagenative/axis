# deployment Specification

## Purpose
How Axis is packaged and run: a reproducible image that runs without root on a read-only filesystem, a compose example with PostgreSQL where media is mounted read-only, and a check that proves it.
## Requirements
### Requirement: Reproducible image
The project SHALL provide a container build that produces an image containing the daemon, `ffmpeg` and `ffprobe` from a pinned base, with no compiler or development packages in the final image. The image SHALL also contain yt-dlp with the Python and JavaScript (Node.js) runtimes it needs, both from the distribution's packages. yt-dlp SHALL be the newest release at the time of the build, or the release the build argument `YTDLP_VERSION` names, fetched over https from yt-dlp's own release location and checked against the checksum list published with that release, and its version SHALL be recorded in the image. That part of the image is not reproducible: two builds of one commit can hold different yt-dlp releases unless `YTDLP_VERSION` pins one.

#### Scenario: Image builds from a clean checkout
- **WHEN** the container build is run on a fresh clone with submodules
- **THEN** it completes without network access beyond the base image, distribution packages and yt-dlp's release location, and the image starts the daemon

#### Scenario: No build tools shipped
- **WHEN** the final image is inspected
- **THEN** it contains no C compiler and no development packages

#### Scenario: The yt-dlp that was taken
- **WHEN** the image is built
- **THEN** the yt-dlp file's SHA-256 is one the release's checksum list names, the build prints its version, and the image holds that version in `/usr/share/axis/yt-dlp.version`

### Requirement: Unprivileged, read-only operation
The image SHALL run the daemon as a non-root user, SHALL work under an arbitrary user id supplied at run time, and SHALL work with a read-only root filesystem and all capabilities dropped.

#### Scenario: Not root
- **WHEN** the example is started
- **THEN** the daemon process's user id is not 0

#### Scenario: Read-only root
- **WHEN** the container runs with a read-only root filesystem and no added capabilities
- **THEN** the daemon starts and reports healthy

#### Scenario: Different user id
- **WHEN** the container is started with another user id
- **THEN** the daemon starts and reports healthy

### Requirement: Compose example
The project SHALL provide a compose example that starts the daemon and PostgreSQL, keeps database data in a named volume, does not publish the database port, takes the database password from a secret, and mounts media read-only.

#### Scenario: Starts healthy
- **WHEN** the example is started on a machine with a container runtime
- **THEN** both services become healthy and the web interface answers

#### Scenario: Media cannot be written
- **WHEN** the example is running unmodified
- **THEN** a write to any media mount from inside the container fails

#### Scenario: Data survives restart
- **WHEN** the example is stopped and started again
- **THEN** the database contents are unchanged

### Requirement: Clean stop
The container SHALL exit with status 0 within the daemon's grace period when asked to stop.

#### Scenario: Stop
- **WHEN** the runtime stops the container
- **THEN** the daemon shuts down in order and the container exits 0 before the runtime's kill timeout

### Requirement: A volume for the data directory
The compose example SHALL give the daemon's data directory a named volume, with the root filesystem still read-only.

#### Scenario: Read-only root with data
- **WHEN** the example runs with a read-only root filesystem
- **THEN** the daemon starts, and files it stores survive a restart

### Requirement: The guide says how to send Axis's traffic through a VPN
The deployment guide SHALL present two ways to send Axis's traffic through a VPN: running Axis in the VPN container's network (which covers name lookups too), with what that means for the published port, the database and the download clients; and `AXIS_OUTBOUND_BIND`, with the guarantee of each form (an interface, an address), the kernel it needs, what each scope covers, and that name lookups and the database are never covered by it. The full-stack Compose example SHALL carry both, commented out.

#### Scenario: Reading the guide
- **WHEN** a person reads "Sending Axis's traffic through a VPN" in `deploy/README.md`
- **THEN** it says that name lookups are not covered by `AXIS_OUTBOUND_BIND` and that running Axis in the VPN container's network covers them

#### Scenario: The example still runs
- **WHEN** the full-stack example is started unchanged
- **THEN** it behaves as before: the VPN lines are comments

### Requirement: The arm64 image is built on an arm64 machine of one's own
`make container-arm64` SHALL build the arm64 image natively on the machine `ARM_BUILDER` names over SSH, with Docker or Podman there and without sudo, from the same build context `make container` uses, and SHALL bring the image back as `localhost/axis:<version>-arm64`. It SHALL install and configure nothing on the builder and SHALL remove its work directory there afterwards. Without `ARM_BUILDER` (and without `ARM_EMULATE=1`) it SHALL stop and say what to set.

#### Scenario: A builder is named
- **WHEN** `make container-arm64 ARM_BUILDER=mypi` runs and mypi is an arm64 machine with Docker
- **THEN** `localhost/axis:<version>-arm64` exists here, its architecture is arm64, and no `axis-build` directory is left on mypi

#### Scenario: No builder
- **WHEN** `make container-arm64` runs without `ARM_BUILDER`
- **THEN** it builds nothing and says to set `ARM_BUILDER` (or `ARM_EMULATE=1`)

#### Scenario: The builder needs sudo
- **WHEN** the builder's Docker refuses the user
- **THEN** it stops, saying that sudo or the docker group is needed there, and never runs sudo

### Requirement: Both architectures, pushed only when asked
`make container-multiarch` SHALL build the amd64 and arm64 images and a manifest list of both. It SHALL push nothing unless `PUSH=1` and `REGISTRY` are given; a push SHALL require a committed working tree and SHALL use the image workflow's tag policy (at a commit tagged `vX.Y.Z`: `X.Y.Z`, `X.Y` and `latest`; at a commit tagged as a pre-release, `vX.Y.Z-NAME`: `X.Y.Z-NAME` and `rc`; otherwise `sha-<commit>` only). No file in the repository SHALL hold a registry secret.

#### Scenario: Not asked to push
- **WHEN** `make container-multiarch ARM_BUILDER=mypi` runs
- **THEN** the manifest list exists locally, nothing is pushed, and it says which tags `PUSH=1` would push

#### Scenario: An untagged commit
- **WHEN** the tags are computed at a commit with no version tag
- **THEN** the only tag is `sha-<commit>`

### Requirement: The base image is pinned for every architecture
The Containerfile SHALL pin its base image by the digest of the image index (every architecture of one build), so that the amd64 and arm64 builds each take their own architecture's image of the same build.

#### Scenario: The arm64 build's base
- **WHEN** the image is built on an arm64 machine
- **THEN** its build stage runs arm64 programs from the base, and its unit tests pass there

### Requirement: The first disk can be added as the examples mount it
A newcomer who follows an example as written (media mounted read-only) SHALL be able to add the first disk as a volume and a library root on it, and scan it, without changing the mount; the documents SHALL say to mount the media read-only and that Axis writes its marker later, the first time the disk is mounted read-write. The example checks SHALL do this through the web interface in every example they run.

#### Scenario: The minimal example as written
- **WHEN** the Compose example is started as its README says and its disk, mounted read-only, is added on the Disks page with a library root, and scanned
- **THEN** the volume is online, read-only, the scan finds the film, and nothing is written to the disk

#### Scenario: Made writable later
- **WHEN** `:ro` is taken off the disk's line and the example is started again
- **THEN** Axis writes the disk's marker and recognises the disk by it

### Requirement: A tested backup and restore
The deployment guide SHALL document a backup taken while Axis runs and a restore into a database created empty, and a check SHALL run the documented commands with the examples' images: back up, change data, restore, start Axis, and compare. The guide SHALL NOT tell a person to restore over a database that Axis has used.

#### Scenario: Going back after an upgrade
- **WHEN** a backup taken before an upgrade is restored as the guide says, after the newer Axis migrated the database
- **THEN** the database holds exactly the backup's data and the older Axis starts on it

#### Scenario: The restore fails part-way
- **WHEN** a restore as the guide says meets an error
- **THEN** it stops at that error and says so, instead of finishing with errors ignored

### Requirement: New databases have data checksums
The examples SHALL create their PostgreSQL database with data checksums, and the guide SHALL say how to check an existing database and how to turn checksums on for it.

#### Scenario: An example started on an empty volume
- **WHEN** an example stack starts with an empty database volume
- **THEN** `SHOW data_checksums` in its database answers `on`

### Requirement: A worked example of a worker on another machine
The repository SHALL include an example of Axis on a server and `axis-worker` on another machine with a GPU, `deploy/examples/remote-worker/`, that says how to give the worker its token in a file only its user can read, reach Axis through a reverse proxy over https with the proxy in `AXIS_TRUSTED_PROXIES`, mount the server's disks at the paths the worker is told (NFS first, sshfs or SMB with `volumes_without_file_identity`) writable for the work area, run the worker with rootless Podman and the GPU, limit its encoders and its CPU share, and recognise it working and the failures a reader will meet. It SHALL use placeholders for every host, domain, user and path. `deploy/README.md` SHALL link it, and `make check-examples` SHALL say that it does not run it and why.

#### Scenario: Reading the example
- **WHEN** a person reads `deploy/examples/remote-worker/README.md`
- **THEN** it says what to do when a conversion fails with "source changed" through an sshfs mount

#### Scenario: The examples' check
- **WHEN** `make check-examples` runs
- **THEN** the remote-worker example is not started, and the script's header says it needs two machines and a GPU

### Requirement: The guides say what runs where
`deploy/examples/remote-worker/README.md` and `deploy/README.md` SHALL say, for each value of `convert.checks_where`, what runs on the server and what on a worker (encodes, the checks' measurements, the checks the server always runs), how to make a server do no heavy work, what holding conversion work after a restart does and how to resume it, and what choosing a worker trusts the worker with. The README's status table SHALL say the same in one line.

#### Scenario: A server that must not do heavy work
- **WHEN** a person reads the remote-worker example
- **THEN** it says to set `convert.checks_where` to `worker_only`, and what the server still reads in that mode

### Requirement: One daemon per database is documented
The deployment guide SHALL say that one daemon serves a database; what a second daemon answers and logs and when it takes over; how Compose, Kubernetes `Recreate` and a rolling update meet the lock; what happens when the daemon loses its lock; and that Axis needs a direct connection to PostgreSQL or a pooler in session mode, a pooler in transaction or statement mode being unsupported.

#### Scenario: A second container on one database
- **WHEN** a person reads the deployment guide's section on one daemon per database
- **THEN** it says what a second daemon answers and logs, when it takes over, which deployment strategies and connection poolers work, and what the daemon does when it loses its lock

### Requirement: The published tree holds made-up values only
The repository SHALL hold, in its tests, fixtures, fuzz seeds, documents and code, made-up titles, identifiers, hosts and addresses only, and nothing of any person's own setup (the host names and domains of their machines, a home directory, a LAN address, an account, a service's answer as it came), as the worked examples already use placeholders. `make check-made-up`, run first by `make check` and needing no network, SHALL fail, naming the file and line, on a tracked file that holds an info-hash, an IMDb identifier, a TMDb or TheTVDB identifier, a host name, an e-mail address, a private address of a host, a home directory or a word of the private list that is not of a made-up form, and SHALL keep the private list's words hashed. A recorded answer SHALL be kept as a structurally faithful recording with invented content, and its README SHALL say so. The one real title the code needs (the token check asks the real metadata service) SHALL be named in one place only.

#### Scenario: A real value in a fixture
- **WHEN** a tracked fixture holds an info-hash that is not a repeated pattern, or a host name that is neither reserved for examples nor one the code must name
- **THEN** `make check-made-up` fails and names the file, the line and the kind of value

#### Scenario: Made-up values pass
- **WHEN** the tree holds only made-up forms (`e1e2e3e4...` hashes, `tt0600001`, `{tmdb-6001}`, `api.newsowl.example`, documentation addresses)
- **THEN** `make check-made-up` passes, and `make check` goes on to build and test

#### Scenario: The private words are not listed
- **WHEN** a person reads the check's rules
- **THEN** the host names of the machines Axis was built and run on do not appear in them, only their salted hashes

#### Scenario: Titles are left to a person
- **WHEN** `tests/tools/check-made-up.py --titles` runs
- **THEN** it lists every title-like string with its count and first place, and does not fail

### Requirement: Secrets can be supplied as files
Every secret bootstrap value (the database URL, the first administrator's password, the metrics token) SHALL be acceptable as the path of a file holding it, named by the value's variable with `_FILE` added, as an alternative to the value itself; setting both SHALL stop the daemon at start with an error naming both, and no message SHALL contain a secret or a file's content.

#### Scenario: Secret file
- **WHEN** the database address is given as a file path
- **THEN** the daemon reads the address from that file and starts

#### Scenario: Both forms set
- **WHEN** a secret and its `_FILE` form are both set
- **THEN** the daemon exits with a non-zero status and an error naming both variables, without the value

### Requirement: The first administrator from the configuration
When no account exists, the daemon SHALL create the administrator named by its configuration, with the password from the environment or a secret file, instead of printing a setup token; the username and password SHALL pass the same rules as the setup page, and the password SHALL never be logged. When an account exists, the configured administrator SHALL NOT be created.

#### Scenario: First administrator from a secret
- **WHEN** the daemon starts on an empty database with a username and a password file configured
- **THEN** that administrator exists and can log in, no setup token is printed, and the password is in no log

#### Scenario: Rules broken
- **WHEN** the configured password is shorter than the setup page allows, or the username breaks the username rule
- **THEN** the daemon exits with a non-zero status, naming the setting and the rule but not the password, and creates nothing

#### Scenario: Accounts exist
- **WHEN** the daemon starts with a configured administrator and an account already exists
- **THEN** no account is created and the log says the setting was not used

### Requirement: Liveness and readiness are separate
The daemon SHALL report that it is alive separately from whether it is ready to serve, and SHALL be ready only when its database is reachable and at the expected schema version. Neither report SHALL need a session or say more than a status word.

#### Scenario: Database not up yet
- **WHEN** the daemon starts before its database
- **THEN** it is alive, not ready, serves no page, keeps trying with back-off, and becomes ready when the database is

#### Scenario: Schema of another build
- **WHEN** the database records a schema version other than the one the running build expects
- **THEN** the daemon is alive and not ready

#### Scenario: Bounded wait
- **WHEN** a maximum wait for the database is configured and the database does not answer within it
- **THEN** the daemon exits with a non-zero status, saying it gave up waiting and never having served a page

### Requirement: The image runs as any user
The image SHALL run as whatever non-root user and group the platform assigns, without a root phase and without changing the ownership of anything at start.

#### Scenario: Arbitrary user
- **WHEN** the container is started with an arbitrary user id and group 0
- **THEN** the daemon starts and can write its data directory

### Requirement: Examples for Compose and Kubernetes
The project SHALL provide working examples for Docker or Podman Compose and for Kubernetes in which downloads and library are reached through one mount, so that imports are hardlinks.

#### Scenario: Kubernetes
- **WHEN** the example manifests are applied to a cluster
- **THEN** the daemon becomes ready

### Requirement: Worked examples for the common ways of running containers
The project SHALL provide tested examples for plain run commands, Compose (alone and with the download stack), Traefik in front, Kubernetes, and a systemd-managed container, each with instructions for what to edit and how to check it works.

#### Scenario: Full stack
- **WHEN** the full-stack Compose example is brought up
- **THEN** Axis, its database, an indexer manager and the download clients run with downloads and library on one mount

### Requirement: Forwarded headers are trusted only from listed proxies
The daemon SHALL take the public scheme, host and client address from forwarded headers only when the request comes from a proxy the administrator has listed, and SHALL ignore (not refuse) them otherwise. Behind several listed proxies the client SHALL be the rightmost forwarded address that is not itself listed; a malformed value SHALL be ignored.

#### Scenario: Through the proxy
- **WHEN** a request arrives through a listed proxy with forwarded headers
- **THEN** the daemon uses the forwarded scheme, host and client address

#### Scenario: Not from a proxy
- **WHEN** the same headers arrive from an address that is not a listed proxy
- **THEN** they are ignored

#### Scenario: Malformed values
- **WHEN** a listed proxy forwards a scheme other than http or https, or a host that is not a host name or address
- **THEN** that value is ignored and the request's own scheme or host stands

### Requirement: Published images
The project SHALL publish versioned images for amd64 and arm64, built and signed by its own pipeline on version tags, with the source revision in the image labels; that pipeline SHALL be the repository's only workflow. A release tag (`vX.Y.Z`) SHALL be published under the exact version, the minor line and `latest`. A pre-release tag (a version tag with a hyphen suffix, such as `v0.1.0-rc1`) SHALL be published under the exact version with its suffix and the moving tag `rc` only, and SHALL NOT move the minor line or `latest`. A version tag SHALL NOT publish a `sha-<commit>` tag; a run started by hand at a commit without a version tag SHALL publish `sha-<commit>` only. The programs in an image SHALL report the image's version, with a pre-release's suffix, and the build SHALL fail when they report another.

#### Scenario: A version tag
- **WHEN** a release tag such as `v0.1.2` is pushed
- **THEN** an image for both architectures is pushed under `0.1.2`, `0.1` and `latest`, and signed

#### Scenario: A release candidate
- **WHEN** the tag `v0.1.0-rc1` is pushed
- **THEN** an image for both architectures is pushed under `0.1.0-rc1` and `rc` only, and signed, and `axisd --version` in it reports `0.1.0-rc1`

#### Scenario: A tag the source was not moved to
- **WHEN** an image is built as version `0.2.0` from a source whose version is `0.1.0`
- **THEN** the build fails and nothing is pushed

### Requirement: Optional metrics
The daemon SHALL offer a Prometheus endpoint only when a metrics token is configured, SHALL answer it only to requests carrying that token, and SHALL report counts and timings without titles, paths or names.

#### Scenario: Off by default
- **WHEN** no metrics token is configured
- **THEN** the metrics path does not exist

#### Scenario: Token required
- **WHEN** the metrics path is requested without the token, or with another
- **THEN** the request is refused, and with the token it gets the metrics

### Requirement: Optional JSON logs
The daemon SHALL be able to write its log as one JSON object per line, carrying the same message text, with the same redaction, as the plain format.

#### Scenario: JSON logs
- **WHEN** the JSON log format is configured
- **THEN** every line the daemon writes is one JSON object, and no secret is in any of them

### Requirement: A stop leaves nothing partial
On SIGTERM the daemon SHALL finish or give back the work in flight within its grace period and exit with status 0. A copy into the library that the stop interrupts SHALL leave no partial file under a library name and nothing counted against its import, and the daemon started again SHALL complete the import once.

#### Scenario: Stopped during a copy
- **WHEN** the daemon receives SIGTERM while an import copies a file into the library
- **THEN** it exits within its grace period with status 0, no partial file is in the library, the download is as it was, and started again it imports the file once, its records agreeing with the disk

