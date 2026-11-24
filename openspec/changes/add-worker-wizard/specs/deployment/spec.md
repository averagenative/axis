## MODIFIED Requirements

### Requirement: A worked example of a worker on another machine
The repository SHALL include an example of Axis on a server and `axis-worker` on another machine, `deploy/examples/remote-worker/`, that says how to give the worker its token in a file only its user can read, reach Axis through a reverse proxy over https with the proxy in `AXIS_TRUSTED_PROXIES`, mount the server's disks at the paths the worker is told (NFS first, sshfs or SMB with `volumes_without_file_identity`) writable for the work area, run the worker with rootless Podman (a script and a Quadlet unit) or with Docker Compose (a `compose.yaml`), with an NVIDIA GPU, with an Intel or AMD GPU through VA-API or with the CPU only, limit its encoders and its CPU share, and recognise it working and the failures a reader will meet. It SHALL point to the guide "Attach a worker", which generates the same text. It SHALL say which of its files and encoders were run on real hardware and which were not. It SHALL use placeholders for every host, domain, user and path. `deploy/README.md` SHALL link it. `make check-examples` SHALL check of it only what one machine without a GPU can (the script's syntax, that a Compose front end accepts the Compose file for each encoder, that Quadlet's generator accepts the unit in a dry run), SHALL start no worker, and SHALL say that it does not run the worker and why.

#### Scenario: Reading the example
- **WHEN** a person reads `deploy/examples/remote-worker/README.md`
- **THEN** it says what to do when a conversion fails with "source changed" through an sshfs mount

#### Scenario: What was run
- **WHEN** a person reads `deploy/examples/remote-worker/README.md`
- **THEN** a table says, for each file and each encoder, whether it ran on real hardware

#### Scenario: The examples' check
- **WHEN** `make check-examples` runs
- **THEN** no worker of the remote-worker example is started, the Compose file is accepted by `compose config` for each encoder, and the script's header says that running the worker needs two machines and, for two encoders, a GPU
