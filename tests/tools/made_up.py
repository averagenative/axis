# made_up.py - what a made-up value looks like, for the repository's check
# (check-made-up.py) and the fixture scrubber (scrub-fixture.py)
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The repository is public: its tests, fixtures, documents and code hold
# made-up titles, identifiers, hosts and addresses only, and nothing of an
# owner's or contributor's own setup.  A real value does not look different
# from a made-up one, so the rules below are conventions: a made-up value
# takes one of the forms named here, and a value of another form is either
# replaced or added here on purpose, with the reason beside it.
#
# Loaded by path (both scripts run under python3 -I, which leaves their own
# directory off the module path, and so they add it themselves).
#
# An optional local extension: a person can keep a list of further words
# that the check refuses anywhere in the tree, such as the names of their
# own machines.  The list is a plain text file outside the repository, one
# word per line, and the environment variable AXIS_MADE_UP_PRIVATE_WORDS
# names it.  When the variable is not set, or names no file, that part of
# the check is skipped and nothing is said.  The repository holds no such
# list.

import ipaddress
import os
import re

# ── Info-hashes ────────────────────────────────────────────────────────

# A run of exactly 40 hexadecimal digits (an info-hash of a torrent, which
# identifies the torrent and so its film; or a commit id).
HASH = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{40}(?![0-9A-Fa-f])")
# A magnet link's info-hash written in base 32.
BASE32_MAGNET = re.compile(r"btih:([A-Za-z2-7]{32})(?![A-Za-z2-7])")


def made_up_hash(run):
    """Whether @run (40 hexadecimal digits, or 32 of base 32) is one of the
    made-up patterns: a unit of at most 16 characters repeated ("5a5a5a...",
    "0123456789abcdef0123..."), or ten runs of four of one digit
    ("aaaa1111bbbb2222...").  The rule of /builtin/fixtures-made-up
    (tests/test-indexer-builtin.c).  An info-hash of a real torrent is
    neither."""
    run = run.lower()
    size = len(run)
    if any(run == (run[:p] * size)[:size] for p in range(1, 17)):
        return True
    return all(run[i] == run[i - i % 4] for i in range(size))


# ── Provider identifiers ───────────────────────────────────────────────

IMDB = re.compile(r"(?<![A-Za-z0-9])tt(\d{7,8})(?!\d)")


def _run_of(digits):
    """Whether @digits are one digit repeated, or each one more (or each
    one less) than the one before."""
    if len(set(digits)) == 1:
        return True
    steps = {int(b) - int(a) for a, b in zip(digits, digits[1:])}
    return steps == {1} or steps == {-1}


def made_up_imdb(digits):
    """Whether the IMDb identifier tt<@digits> is a made-up one: four zeros
    in a row (tt0000001 to tt0000999, the scrubber's series; tt7000007,
    tt9900001), one digit repeated (tt9999999), or a run (tt1234567,
    tt0123456, tt7654321).  Any other identifier of seven or eight digits
    is taken to name a real title."""
    return "0000" in digits or _run_of(digits)


# TMDb and TheTVDB identifiers where a name or an address says what they
# are: {tmdb-12345}, [tvdbid-77], tmdbid=42, "tmdbId": 4321, tvdb_id = 7,
# TvdbId:56789, themoviedb.org/movie/12345, /3/tv/4321, thetvdb.com/...id=.
PROVIDER_ID = re.compile(
    r"(?:\{(?:tmdb|tvdb)-0*(\d+)\}"
    r"|\[(?:tmdb|tvdb)id-0*(\d+)\]"
    r"|\b(?:tmdb|tvdb)[_ ]?id\b[\"']?\s*(?:[:=]|==)\s*\"?(\d+)"
    r"|themoviedb\.org/(?:movie|tv)/(\d+)"
    r"|/3/(?:movie|tv)/(\d+)"
    r"|thetvdb\.com/\S*?(?:id=|series/)(\d+))", re.I)

# The made-up TMDb and TheTVDB identifiers the tests use beside their
# made-up titles (each number is some real title's identifier too: what
# makes these made up is the title they stand for here).  Numbers under 100
# and the forms made_up_imdb() accepts need no entry.
PROVIDER_IDS = frozenset(
    # Films of the identification stand-in and the metadata fixtures
    # (tests/axis-test-identify.c: 6001 "Hearth" and on), and the series'
    # (7001 "Lantern Row" and on; 7101 "Cracking Slate"), with their TheTVDB
    # numbers (81001 and on, 82001 and on).
    list(range(6001, 6100)) + [6100] + list(range(7001, 7100)) + list(range(7101, 7110)) +
    list(range(81001, 81100)) + list(range(82001, 82010)) +
    # Episodes of those series at TheTVDB, as a Sonarr stand-in gives them:
    # the series' number, then three digits (81061001 and on).
    [s * 1000 + e for s in range(81001, 81100) for e in range(1, 100)] +
    # The Discover stand-in's lists (tests/fixtures/discover).
    list(range(9001, 9010)) + [9101, 9601] +
    # Series and films of other tests: "Fixture Street" (201, 350001),
    # "Harbour Lights" (360002), "Lantern Row" (80001, 80002), "Marrowfield
    # Lanterns" (990120); and a number too long to be an identifier.
    [201, 350001, 360002, 80001, 80002, 990120, 1234567890123]
)


def made_up_provider_id(number):
    digits = str(int(number))
    return int(number) < 100 or _run_of(digits) or int(number) in PROVIDER_IDS


# ── Host names and addresses ───────────────────────────────────────────

# Names reserved for documentation and tests (RFC 2606, RFC 6761), and
# names that are no host on the internet.
RESERVED_SUFFIXES = (
    "example", "invalid", "test", "localhost", "local", "lan", "internal", "home.arpa",
    "example.com", "example.org", "example.net",
)

# Hosts the code, the definitions or the documents must name: the services
# Axis talks to or points a person to, the sites it supports by name, the
# standards and schemas it reads, and the places its images and its
# dependency come from.  A host here and its subdomains are accepted.
NAMED_HOSTS = (
    # Metadata providers.
    "themoviedb.org", "tmdb.org", "imdb.com", "thetvdb.com", "thetvdb.github.io",
    # The sites supported by name (A18, A58), the mirrors and caches their
    # design names, and the public trackers a magnet link is given.
    "apibay.org", "thepiratebay.org", "yts.mx", "yts.gg", "eztvx.to", "torrentproject.cc", "limetorrents.fun", "limetorrents.lol",
    "ninjaproxy1.com", "proxyninja.org", "torrentbay.st", "torrentsbay.org", "itorrents.net", "mylink.cloud",
    "therarbg.com", "therarbg.to", "rargb.to", "torrents-csv.com", "ddos-guard.net",
    "opentrackr.org", "stealth.si", "torrent.eu.org", "desync.com", "openbittorrent.com", "tracker.cl",
    # Download clients, media servers and indexer managers: their documents.
    "sabnzbd.org", "deluge.readthedocs.io", "jellyfin.org", "plex.tv", "servarr.com", "prowlarr.com",
    "trash-guides.info",
    # The one site the YouTube downloader takes addresses of (add-youtube-downloader).
    "youtube.com", "youtu.be",
    # Standards and schemas.
    "newznab.com", "newznab.readthedocs.io", "torznab.com", "torznab.github.io", "newzbin.com", "w3.org",
    "purl.org", "htmx.org", "gnu.org", "fsf.org", "openssh.com",
    # Where images, examples and the dependency come from.
    "github.com", "githubusercontent.com", "ghcr.io", "docker.io", "lscr.io", "linuxserver.io",
    "kubernetes.io", "k8s.io", "intel.com", "nvidia.com", "traefik.io", "containo.us",
    "git.podbielniak.com",
    # Not hosts: the annotations by which two Kubernetes network plugins give
    # pods an address pool (deploy/examples/kubernetes/portal.yaml).
    "projectcalico.org", "cilium.io",
    # Sites of other projects that design notes have named.
    "igdb.com", "romm.app", "emulatorjs.org", "ghost-of-usenet.org",
    # Not hosts: Deluge's JSON-RPC method of that name.
    "daemon.info",
)

URL_HOST = re.compile(r"(?<![A-Za-z0-9+.-])[A-Za-z][A-Za-z0-9+.-]*://(?:[^/\s@\"'<>]*@)?([A-Za-z0-9.-]+)")
# A bare host name: labels of lower-case letters, digits and hyphens, and one
# of the top-level domains host names here have used.  Release names are
# capitalised and code's members are not checked (see check-made-up.py).
BARE_HOST = re.compile(
    r"(?<![A-Za-z0-9._>@&*%-])((?:[a-z0-9][a-z0-9-]+\.)(?:[a-z0-9][a-z0-9-]*\.)*"
    r"(?:com|org|net|io|tv|to|mx|me|ru|cc|info|xyz|dev|app|co|uk|de|fr|nl|se|ch|eu|ca|au|nz|lol|club|"
    r"online|top|ws|ag|si|li|pw|be|cz|pl|lv|sx|onion|arpa|gg|ly|gl|cloud|tech|tk|ml|ga|cf|gq|ninja|website|"
    r"biz|pro|fun|moe|vip|cx|nu|la|ms|ac|ai|cl|us))(?![A-Za-z0-9_(-]|\.[A-Za-z0-9])")


def named_host(host):
    """Whether @host may be named: a reserved name, a name with "example"
    as one of its labels (example.co.uk), an address (checked as one), or a
    host of NAMED_HOSTS or below it."""
    host = host.lower().rstrip(".")
    if not host or "." not in host or re.fullmatch(r"[\d.]+", host) or "example" in host.split("."):
        return True
    for allowed in RESERVED_SUFFIXES + NAMED_HOSTS:
        if host == allowed or host.endswith("." + allowed):
            return True
    return False


EMAIL = re.compile(r"(?<![A-Za-z0-9._%+-])[A-Za-z0-9._%+-]+@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+)")
# An e-mail address is made up when its domain is reserved.  SFTP names its
# extensions like addresses ("fsync@openssh.com").
NAMED_ADDRESSES = ("fsync@openssh.com",)


def made_up_email(address, domain):
    """Whether an e-mail address (or the user@host of an address) is made
    up: its domain reserved, or an IP address (checked as one)."""
    if address.lower() in NAMED_ADDRESSES:
        return True
    domain = domain.lower()
    if re.fullmatch(r"[\d.]+", domain) or "example" in domain.split("."):
        return True
    return any(domain == s or domain.endswith("." + s) for s in RESERVED_SUFFIXES)


IPV4 = re.compile(r"(?<![\d.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:/(\d{1,2}))?(?![\d.])")

# Private addresses of a host (not of a network) that tests, examples and
# documents use on purpose.  Any other private address of a host is taken
# for someone's own and refused.  Networks (a prefix, or an address ending in
# .0) and broadcast addresses (.255) need no entry.
PRIVATE_ADDRESSES = frozenset((
    # Trusted-proxy, forwarded-for and outbound-interface tests.
    "10.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.5", "10.0.0.7", "10.0.0.9", "10.1.1.1", "10.1.2.3",
    "10.200.3.4", "10.8.0.2", "10.8.0.3", "10.9.0.2", "10.9.9.9", "100.64.0.1", "100.64.0.7",
    "172.16.0.1", "172.16.0.10", "172.18.0.2", "172.18.0.5", "172.20.0.2", "172.20.0.3", "172.20.0.4",
    "172.20.0.9", "172.20.1.1", "172.20.1.16", "172.20.1.17", "172.20.1.40", "172.30.0.2",
    "192.168.0.1", "192.168.1.1", "192.168.1.5", "192.168.1.6", "192.168.1.10", "192.168.1.11",
    "192.168.1.20", "192.168.4.4", "192.168.4.5", "192.168.77.2", "192.168.77.50",
    # Rootless podman's slirp4netns address, as the deployment notes say.
    "10.0.2.15",
    # The test Jellyfin's address on its own container network, as its
    # recorded answers give it (tests/fixtures/compat/jellyfin/README.md).
    "10.89.1.2",
    # The request portal in the deployment examples: the daemon's fixed
    # addresses on Traefik's network and on the portal's (Compose), and the
    # daemon's address and the portal's on the portal's network (Quadlet).
    "172.30.0.3", "172.30.1.2", "172.31.78.2", "172.31.78.3",
    # The portal's addresses (AXIS_PORTAL_PEERS) in the same examples: the
    # portal's on its two networks and Traefik's beside it (Compose), and a
    # portal pod's fixed address (Kubernetes).
    "172.30.1.3", "172.30.2.2", "172.30.2.3", "10.44.250.2",
))


# Where a home network's or a provider's private addresses are (RFC 1918,
# and the shared address space of RFC 6598).  The documentation ranges
# (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24) are not among them.
PRIVATE_NETWORKS = tuple(ipaddress.IPv4Network(n) for n in
                         ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10"))


def made_up_ipv4(octets, prefix):
    """Whether a dotted IPv4 address is fine to name: not private, a
    network or a broadcast address, or one of PRIVATE_ADDRESSES."""
    if any(int(o) > 255 for o in octets):
        return True
    text = ".".join(str(int(o)) for o in octets)
    address = ipaddress.IPv4Address(text)
    if not any(address in network for network in PRIVATE_NETWORKS):
        return True
    if prefix is not None or octets[3] in ("0", "255"):
        return True
    return text in PRIVATE_ADDRESSES


# A home directory with a user's name in it.  The documents' own examples
# use these names.
HOME = re.compile(r"(?<![A-Za-z0-9_])(?:/home/|/Users/)([a-z][a-z0-9._-]*)")
HOME_NAMES = ("you", "user", "axis", "me", "someone", "name")

# ── A local list of words that must not appear ─────────────────────────

# The environment variable that names the local word list (see the header).
PRIVATE_WORDS_VARIABLE = "AXIS_MADE_UP_PRIVATE_WORDS"

WORD = re.compile(r"[a-z0-9]+")


def load_private_words(path):
    """The words of the file at @path, lower-cased: one word per line, blank
    lines and lines that start with "#" skipped.  A line is split the way
    the check splits text (at anything but a letter or a digit), so
    "my-host" lists "my" and "host".  An empty set when @path is empty or
    names no file that can be read."""
    if not path:
        return frozenset()
    try:
        with open(path, "rb") as source:
            text = source.read().decode("utf-8", "replace")
    except OSError:
        return frozenset()
    words = set()
    for line in text.split("\n"):
        line = line.strip().lower()
        if line and not line.startswith("#"):
            words.update(WORD.findall(line))
    return frozenset(words)


def private_words():
    """The local word list, or an empty set when the variable is not set or
    names no file."""
    return load_private_words(os.environ.get(PRIVATE_WORDS_VARIABLE, ""))
