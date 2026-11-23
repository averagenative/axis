#!/usr/bin/env python3
# axis-screenshots.py - screenshots and layout checks of Axis web pages
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Take screenshots of a running Axis web instance with headless Chrome.

Usage:
  tests/tools/axis-screenshots.py --base URL --out DIR
      [--user NAME --password PW] [--pages FILE] [--widths phone,desktop]
      [--tiles N] [--chrome PATH] [--only NAME[,NAME...]] [--settle MS]

Options:
  --base URL        the instance, for example http://127.0.0.1:20080 (required)
  --out DIR         where the images and reports go; created if missing (required)
  --user NAME       log in as NAME before the first page (needs --password)
  --password PW     the password for --user; omit both to stay logged out
  --pages FILE      the page list (below); without it, one page: "home /"
  --widths LIST     phone, desktop or both (default phone,desktop)
                      phone:   390x844 CSS px, device scale 2, mobile, touch
                      desktop: 1440x900 CSS px, device scale 1
                    and, for measuring the header (the report says where
                    each of its parts is and which overlap):
                      phone360 (360x780), tablet768 (768x1024),
                      wide1024 (1024x768), wide1280 (1280x800)
                    or a size of one's own: 1248x800 (a desktop: scale 1),
                    360x780m (a phone: scale 2, mobile, touch)
  --tiles N         at most N viewport-high images from the top (default 4;
                    0 takes only the full-page image)
  --chrome PATH     the Chrome binary (default google-chrome)
  --only NAMES      capture only these entries of the list (comma-separated);
                    @login and @logout lines still apply
  --settle MS       wait this long after each load and after each snippet,
                    for htmx to swap content in (default 300)

The page list has one entry per line:

  NAME PATH [JS]

NAME names the files (letters, digits, '.', '_' and '-'); PATH is appended
to --base; JS, the rest of the line, is evaluated in the page before the
capture, for example  document.querySelector('details.add').open = true
If it throws, the error is reported and the page is captured anyway.  Lines
that start with '#' and blank lines are ignored.  Two more lines change who
is logged in for the entries after them:

  @logout   clear the browser's cookies
  @login    log in again with --user and --password

With --user, the tool logs in before the first entry; put the login page
after an @logout, since /login sends a logged-in browser on to /.

For each entry and width the tool writes, into --out:

  NAME-WIDTH-1.png ... NAME-WIDTH-N.png   what a person sees, one screen at a
                                          time from the top (the last one only
                                          the part not shown yet)
  NAME-WIDTH-full.png                     the whole page, at most 6000 CSS px
                                          tall

and measures the page: the HTTP status of the document, whether it scrolls
sideways, the elements whose right edge passes the viewport (elements inside
a box that scrolls or clips sideways are counted separately), tap targets
under 44 CSS px, and the smallest text.  The results go to report.json and
report.txt in --out, and a summary is printed at the end, the pages that
are wider than the phone screen first.

Chrome runs headless with the GPU disabled, its DevTools port on 127.0.0.1
at a random port above 20000, and a throwaway profile that is removed at the
end.  The exit status is 0 when every page was captured, 1 otherwise.
"""

import argparse
import base64
import ctypes
import datetime
import hashlib
import json
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

COMMAND_TIMEOUT = 15.0      # seconds for one DevTools command
NAVIGATION_TIMEOUT = 30.0   # seconds for a page to load
SCREENSHOT_TIMEOUT = 90.0   # seconds for one screenshot
CHROME_START_TIMEOUT = 30.0
HTMX_WAIT = 3.0             # seconds to wait for htmx requests in flight
FULL_PAGE_CAP = 6000        # CSS px
TAP_MIN = 44                # CSS px
SMALL_TEXT = 12             # CSS px
EXAMPLES = 15
MAX_MESSAGE = 256 << 20     # bytes

WIDTHS = {
    "phone": {"width": 390, "height": 844, "scale": 2, "mobile": True, "touch": True},
    "desktop": {"width": 1440, "height": 900, "scale": 1, "mobile": False, "touch": False},
    # The widths the header is measured at (add-attention-notices): the
    # narrowest phone, a tablet, and the two widths either side of where the
    # top row stops being one line.  Not part of the default run.
    "phone360": {"width": 360, "height": 780, "scale": 2, "mobile": True, "touch": True},
    "tablet768": {"width": 768, "height": 1024, "scale": 1, "mobile": True, "touch": True},
    "wide1024": {"width": 1024, "height": 768, "scale": 1, "mobile": False, "touch": False},
    "wide1280": {"width": 1280, "height": 800, "scale": 1, "mobile": False, "touch": False},
}

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


class ToolError(Exception):
    """An error that ends the run."""


class CaptureError(Exception):
    """An error that ends one capture; the run goes on."""


class JSError(Exception):
    """JavaScript evaluated in the page threw."""


# -- WebSocket (RFC 6455, client side) ---------------------------------

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


class WebSocketError(Exception):
    pass


class WebSocketClosed(WebSocketError):
    pass


def ws_mask(data, key):
    """XORs data with the repeated 4-byte key."""
    n = len(data)
    if n == 0:
        return b""
    repeated = (key * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(repeated, "big")).to_bytes(n, "big")


class WebSocket:
    """A minimal blocking WebSocket client over a plain TCP socket."""

    def __init__(self, url, timeout=COMMAND_TIMEOUT):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "ws" or not parts.hostname:
            raise WebSocketError(f"not a ws:// URL: {url}")
        host, port = parts.hostname, parts.port or 80
        path = (parts.path or "/") + (f"?{parts.query}" if parts.query else "")
        self.buf = bytearray()
        self.fragments = None    # [opcode, bytearray] while a message arrives in pieces
        self.closed = False
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            self._handshake(host, port, path, timeout)
        except BaseException:
            self.sock.close()
            self.sock = None
            raise

    def _handshake(self, host, port, path, timeout):
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        # No Origin header: Chrome refuses DevTools connections that send one
        # unless it was started with --remote-allow-origins.
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.settimeout(timeout)
        self.sock.sendall(request.encode("ascii"))
        deadline = time.monotonic() + timeout
        while b"\r\n\r\n" not in self.buf:
            if len(self.buf) > 65536:
                raise WebSocketError("the handshake answer is too long")
            self._recv_some(deadline)
        head, _, rest = bytes(self.buf).partition(b"\r\n\r\n")
        self.buf = bytearray(rest)
        lines = head.decode("iso-8859-1").split("\r\n")
        status = lines[0].split(" ", 2)
        if len(status) < 2 or status[1] != "101":
            raise WebSocketError(f"the server refused the upgrade: {lines[0]}")
        headers = {}
        for line in lines[1:]:
            name, sep, value = line.partition(":")
            if sep:
                headers[name.strip().lower()] = value.strip()
        expect = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode()
        if headers.get("upgrade", "").lower() != "websocket":
            raise WebSocketError("the answer does not upgrade to websocket")
        if headers.get("sec-websocket-accept") != expect:
            raise WebSocketError("the answer has a wrong Sec-WebSocket-Accept")

    def _recv_some(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timed out waiting for the WebSocket")
        self.sock.settimeout(remaining)
        chunk = self.sock.recv(1 << 20)   # a socket timeout raises TimeoutError
        if not chunk:
            self.closed = True
            raise WebSocketClosed("the connection was closed")
        self.buf += chunk

    def _send_frame(self, opcode, payload, timeout=COMMAND_TIMEOUT):
        header = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        key = os.urandom(4)
        header += key
        self.sock.settimeout(timeout)
        self.sock.sendall(bytes(header) + ws_mask(payload, key))

    def send_text(self, text):
        if self.closed or self.sock is None:
            raise WebSocketClosed("the connection is closed")
        self._send_frame(OP_TEXT, text.encode("utf-8"))

    def _take_frame(self):
        """Returns (fin, opcode, payload) once a whole frame is buffered, else None.

        Nothing is consumed until the frame is complete, so a timeout in the
        middle of a frame loses nothing."""
        b = self.buf
        if len(b) < 2:
            return None
        fin = bool(b[0] & 0x80)
        if b[0] & 0x70:
            raise WebSocketError("a frame has RSV bits set, but no extension was agreed")
        opcode = b[0] & 0x0F
        masked = bool(b[1] & 0x80)
        n = b[1] & 0x7F
        pos = 2
        if n == 126:
            if len(b) < 4:
                return None
            n = struct.unpack_from("!H", b, 2)[0]
            pos = 4
        elif n == 127:
            if len(b) < 10:
                return None
            n = struct.unpack_from("!Q", b, 2)[0]
            pos = 10
        if n > MAX_MESSAGE:
            raise WebSocketError(f"a frame of {n} bytes is over the limit")
        key = None
        if masked:
            if len(b) < pos + 4:
                return None
            key = bytes(b[pos:pos + 4])
            pos += 4
        if len(b) < pos + n:
            return None
        payload = bytes(b[pos:pos + n])
        del b[:pos + n]
        if key is not None:
            payload = ws_mask(payload, key)
        return fin, opcode, payload

    def recv(self, timeout):
        """Returns the next text message as str; answers pings on the way."""
        if self.sock is None:
            raise WebSocketClosed("the connection is closed")
        deadline = time.monotonic() + timeout
        while True:
            frame = self._take_frame()
            if frame is None:
                self._recv_some(deadline)
                continue
            fin, opcode, payload = frame
            if opcode >= 0x8:
                if not fin or len(payload) > 125:
                    raise WebSocketError("a control frame is fragmented or too long")
                if opcode == OP_PING:
                    self._send_frame(OP_PONG, payload)
                elif opcode == OP_CLOSE:
                    code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 1005
                    if not self.closed:
                        self.closed = True
                        try:
                            self._send_frame(OP_CLOSE, payload[:2])
                        except OSError:
                            pass
                    raise WebSocketClosed(f"the peer closed the connection (code {code})")
                elif opcode != OP_PONG:
                    raise WebSocketError(f"unknown control opcode {opcode:#x}")
                continue
            if opcode in (OP_TEXT, OP_BINARY):
                if self.fragments is not None:
                    raise WebSocketError("a new message started inside a fragmented one")
                if fin:
                    return payload.decode("utf-8")
                self.fragments = [opcode, bytearray(payload)]
            elif opcode == OP_CONT:
                if self.fragments is None:
                    raise WebSocketError("a continuation frame without a message")
                self.fragments[1] += payload
                if len(self.fragments[1]) > MAX_MESSAGE:
                    raise WebSocketError("a fragmented message is over the limit")
                if fin:
                    data = bytes(self.fragments[1])
                    self.fragments = None
                    return data.decode("utf-8")
            else:
                raise WebSocketError(f"unknown opcode {opcode:#x}")

    def close(self):
        if self.sock is None:
            return
        try:
            if not self.closed:
                self.closed = True
                self._send_frame(OP_CLOSE, struct.pack("!H", 1000), timeout=1.0)
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    self.recv(max(0.01, deadline - time.monotonic()))
        except (OSError, WebSocketError, UnicodeDecodeError):
            pass
        finally:
            self.sock.close()
            self.sock = None


# -- Chrome DevTools Protocol ------------------------------------------

class CDPError(Exception):
    pass


class CDP:
    """Commands and events on one target's WebSocket.

    Events are appended to self.events as they arrive; callers keep their
    own index into it.  Session.navigate() empties it."""

    def __init__(self, ws):
        self.wsock = ws
        self.next_id = 0
        self.events = []

    def _read(self, timeout):
        msg = json.loads(self.wsock.recv(timeout))
        if "method" in msg and "id" not in msg:
            self.events.append(msg)
        return msg

    def call(self, method, params=None, timeout=COMMAND_TIMEOUT):
        self.next_id += 1
        mid = self.next_id
        self.wsock.send_text(json.dumps({"id": mid, "method": method, "params": params or {}}))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            try:
                if remaining <= 0:
                    raise TimeoutError
                msg = self._read(remaining)
            except TimeoutError:
                raise TimeoutError(f"{method}: no answer within {timeout:g} s") from None
            if msg.get("id") == mid:
                if "error" in msg:
                    err = msg["error"]
                    raise CDPError(f"{method}: {err.get('message')} {err.get('data', '')}".strip())
                return msg.get("result", {})
            # An answer to a command that timed out earlier: dropped.

    def wait_event(self, method, start=0, predicate=None, timeout=NAVIGATION_TIMEOUT):
        """Returns the first event METHOD at or after index START, or None."""
        deadline = time.monotonic() + timeout
        i = start
        while True:
            while i < len(self.events):
                ev = self.events[i]
                i += 1
                if ev["method"] == method and (predicate is None or predicate(ev.get("params", {}))):
                    return ev
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                self._read(remaining)
            except TimeoutError:
                return None

    def pump(self, seconds):
        """Takes in events for SECONDS."""
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                self._read(remaining)
            except TimeoutError:
                return

    def evaluate(self, expression, timeout=COMMAND_TIMEOUT, await_promise=False,
                 user_gesture=False, by_value=True):
        params = {
            "expression": expression,
            "returnByValue": by_value,
            "awaitPromise": await_promise,
            "userGesture": user_gesture,
            "timeout": int(timeout * 1000),
        }
        if not by_value:
            params["objectGroup"] = "axis-screenshots"
        res = self.call("Runtime.evaluate", params, timeout=timeout + 2)
        if not by_value:
            try:
                self.call("Runtime.releaseObjectGroup", {"objectGroup": "axis-screenshots"})
            except CDPError:
                pass
        if "exceptionDetails" in res:
            raise JSError(describe_exception(res["exceptionDetails"]))
        return res.get("result", {}).get("value")


def describe_exception(details):
    exc = details.get("exception") or {}
    text = exc.get("description") or exc.get("value") or details.get("text") or "an exception"
    lines = str(text).strip().splitlines()
    return lines[0][:300] if lines else "an exception"


# -- Chrome -------------------------------------------------------------

def http_json(url, method="GET", timeout=5.0):
    # No proxy: the DevTools endpoint is on loopback.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    req = urllib.request.Request(url, method=method)
    with opener.open(req, timeout=timeout) as resp:
        return json.load(resp)


def free_port():
    """A random port above 20000 (and below the usual ephemeral range) that
    nothing listens on at 127.0.0.1 right now."""
    for _ in range(200):
        port = random.randint(20001, 32767)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    raise ToolError("found no free port on 127.0.0.1 above 20000")


def die_with_parent():
    """Runs in the child before exec: Linux sends Chrome SIGKILL if this
    script dies without cleaning up."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl(1, int(signal.SIGKILL), 0, 0, 0)    # PR_SET_PDEATHSIG
    except Exception:
        pass


class Chrome:
    def __init__(self, path):
        self.path = path
        self.proc = None
        self.tmp = None
        self.port = None
        self.version = {}

    def start(self):
        self.tmp = tempfile.mkdtemp(prefix="axis-screenshots-")
        for attempt in range(4):
            profile = os.path.join(self.tmp, f"profile-{attempt}")
            log_path = os.path.join(self.tmp, f"chrome-{attempt}.log")
            os.mkdir(profile)
            self.port = free_port()
            args = [
                self.path,
                "--headless=new",
                "--disable-gpu",
                "--no-first-run",
                "--no-default-browser-check",
                "--hide-scrollbars",
                f"--remote-debugging-port={self.port}",
                "--remote-debugging-address=127.0.0.1",
                f"--user-data-dir={profile}",
                "--disable-extensions",
                "--disable-background-networking",
                "--disable-component-update",
                "--disable-default-apps",
                "--disable-sync",
                "--no-pings",
                "--mute-audio",
                "--password-store=basic",
                "about:blank",
            ]
            with open(log_path, "wb") as log:
                try:
                    self.proc = subprocess.Popen(
                        args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                        start_new_session=True, preexec_fn=die_with_parent)
                except OSError as e:
                    raise ToolError(f"cannot run Chrome ({self.path}): {e}") from None
            state = self._wait_ready(profile, log_path)
            if state == "ready":
                break
            self._kill()
            if state == "exited":
                raise ToolError("Chrome exited at start:\n" + tail(log_path))
            if state == "timeout":
                raise ToolError(f"Chrome's DevTools did not come up within {CHROME_START_TIMEOUT:g} s:\n"
                                + tail(log_path))
            # The port was taken between free_port() and Chrome: try another.
        else:
            raise ToolError("Chrome found every port it was given taken")
        self.version = http_json(f"http://127.0.0.1:{self.port}/json/version")

    def _wait_ready(self, profile, log_path):
        """Waits until Chrome says its DevTools server listens on our port
        (on stderr, or in DevToolsActivePort): then the port is Chrome's and
        not something else's.  When the port is taken, Chrome runs on
        without DevTools and says neither."""
        listening = f"DevTools listening on ws://127.0.0.1:{self.port}/"
        active = os.path.join(profile, "DevToolsActivePort")
        deadline = time.monotonic() + CHROME_START_TIMEOUT
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return "exited"
            try:
                with open(log_path, encoding="utf-8", errors="replace") as f:
                    log = f.read()
                if listening in log:
                    return "ready"
                if "Cannot start http server for devtools" in log:
                    return "port taken"
                with open(active, encoding="ascii") as f:
                    if f.readline().strip() == str(self.port):
                        return "ready"
            except (OSError, ValueError):
                pass
            time.sleep(0.05)
        return "timeout"

    def page_websocket_url(self):
        base = f"http://127.0.0.1:{self.port}"
        deadline = time.monotonic() + 10
        while True:
            pages = [t for t in http_json(f"{base}/json/list") if t.get("type") == "page"]
            if pages:
                return pages[0]["webSocketDebuggerUrl"]
            if time.monotonic() > deadline:
                return http_json(f"{base}/json/new?about:blank", method="PUT")["webSocketDebuggerUrl"]
            time.sleep(0.1)

    def _kill(self):
        if self.proc is None:
            return
        pgid = self.proc.pid    # start_new_session: Chrome leads its own group
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(pgid, sig)
            except (ProcessLookupError, PermissionError):
                break
            deadline = time.monotonic() + 5.0
            gone = False
            while time.monotonic() < deadline:
                self.proc.poll()
                try:
                    os.killpg(pgid, 0)
                except (ProcessLookupError, PermissionError):
                    gone = True
                    break
                time.sleep(0.05)
            if gone:
                break
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        self.proc = None

    def stop(self):
        self._kill()
        if self.tmp is not None:
            shutil.rmtree(self.tmp, ignore_errors=True)
            self.tmp = None


def tail(path, lines=20):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-lines:]).rstrip()
    except OSError:
        return "(no log)"


# -- What runs in the page ---------------------------------------------

# Waits until the images on screen have loaded (at most 1.5 s), then two
# animation frames (or 250 ms, whichever comes first); gives the top of what
# is on screen, in page coordinates.  That is the visual viewport's: when
# something sticks out sideways on a phone, Chrome makes the layout
# viewport (innerWidth, innerHeight, scrollY) larger than the screen.
FRAMES_JS = r"""
new Promise((resolve) => {
  const t0 = performance.now();
  const vv = window.visualViewport;
  const finish = () => {
    let done = false;
    const end = () => { if (!done) { done = true; resolve(vv ? vv.pageTop : window.scrollY); } };
    requestAnimationFrame(() => requestAnimationFrame(end));
    setTimeout(end, 250);
  };
  const check = () => {
    const vh = vv ? vv.height : window.innerHeight;
    const waiting = Array.from(document.images).some((img) => {
      if (img.complete) return false;
      const r = img.getBoundingClientRect();
      return r.width > 0 && r.bottom > 0 && r.top < vh;
    });
    if (!waiting || performance.now() - t0 > 1500) finish(); else setTimeout(check, 50);
  };
  check();
})
"""

MEASURE_JS = r"""
(function (vw, tapMin, smallText, examples) {
  const de = document.documentElement;
  const body = document.body || de;
  const sx = window.scrollX, sy = window.scrollY;
  const styles = new Map();
  const style = (el) => {
    let s = styles.get(el);
    if (!s) { s = getComputedStyle(el); styles.set(el, s); }
    return s;
  };
  const esc = (s) => (window.CSS && CSS.escape) ? CSS.escape(s) : s;
  const path = (el) => {
    const parts = [];
    for (let e = el, n = 0; e && e.nodeType === 1 && n < 5; e = e.parentElement, n++) {
      let p = e.localName;
      if (e.id) p += '#' + esc(e.id);
      for (const c of Array.from(e.classList).slice(0, 2)) p += '.' + esc(c);
      parts.unshift(p);
      if (e === body || e === de) break;
    }
    return parts.join(' > ');
  };
  const clean = (s) => String(s || '').replace(/\s+/g, ' ').trim().slice(0, 40);
  const round = (x) => Math.round(x * 10) / 10;
  const clipsX = (el) => ['auto', 'scroll', 'hidden', 'clip'].includes(style(el).overflowX);
  // The nearest box that scrolls or clips sideways, below <body>.  The
  // root's and body's own overflow apply to the viewport, so they do not
  // count as a box.
  const container = (el, self) => {
    for (let e = self ? el : el.parentElement; e && e !== body && e !== de; e = e.parentElement)
      if (clipsX(e)) return e;
    return null;
  };
  const invisible = (el) => {
    const s = style(el);
    return s.visibility === 'hidden' || s.visibility === 'collapse' || s.opacity === '0';
  };

  // Elements whose right edge passes the viewport.
  const offenders = [];
  const offending = new Set();
  const boxes = new Map();
  for (const el of body.querySelectorAll('*')) {
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) continue;
    if (r.right + sx <= vw + 0.5) continue;
    const box = container(el, false);
    if (box) { boxes.set(box, (boxes.get(box) || 0) + 1); continue; }
    if (style(el).visibility === 'hidden') continue;
    offenders.push({ el: el, r: r, kind: 'box' });
    offending.add(el);
  }
  const outer = new Set();
  for (const o of offenders)
    for (let e = o.el.parentElement; e; e = e.parentElement)
      if (offending.has(e)) outer.add(e);

  // Text: past the right edge, and its size.
  const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT);
  const range = document.createRange();
  const skip = new Set(['script', 'style', 'noscript', 'template', 'title']);
  let minFont = null, minFontAt = null;
  const small = new Map();
  while (walker.nextNode()) {
    const t = walker.currentNode;
    if (!t.data.trim()) continue;
    const p = t.parentElement;
    if (!p || skip.has(p.localName)) continue;
    range.selectNodeContents(t);
    if (!range.getClientRects().length) continue;
    const rr = range.getBoundingClientRect();
    if (rr.width <= 0 || rr.height <= 0) continue;
    const pr = p.getBoundingClientRect();
    if (pr.width <= 1 || pr.height <= 1) continue;   // visually hidden text
    if (invisible(p)) continue;
    const fs = parseFloat(style(p).fontSize);
    if (minFont === null || fs < minFont) { minFont = fs; minFontAt = { path: path(p), text: clean(t.data) }; }
    if (fs < smallText && !small.has(p)) small.set(p, { path: path(p), font_px: round(fs), text: clean(t.data) });
    if (rr.right + sx > vw + 0.5 && !offending.has(p)) {
      const box = container(p, true);
      if (box) boxes.set(box, (boxes.get(box) || 0) + 1);
      else offenders.push({ el: p, r: rr, kind: 'text', text: clean(t.data) });
    }
  }
  // The innermost offenders first: they are usually the cause.
  const ordered = offenders.filter((o) => !outer.has(o.el)).concat(offenders.filter((o) => outer.has(o.el)));
  const overflow = ordered.slice(0, examples).map((o) => {
    const x = { path: path(o.el), kind: o.kind, width: round(o.r.width), right: round(o.r.right + sx),
      innermost: !outer.has(o.el) };
    if (o.text !== undefined) x.text = o.text;
    return x;
  });
  const contained = Array.from(boxes.keys()).slice(0, 5).map((b) => ({
    path: path(b), overflow_x: style(b).overflowX, client_width: b.clientWidth, scroll_width: b.scrollWidth,
    elements_past_edge: boxes.get(b),
  }));

  // Tap targets.
  const label = (el) => {
    const tag = el.localName;
    let s = el.getAttribute('aria-label') || '';
    if (!s && tag === 'select' && el.selectedIndex >= 0) s = el.options[el.selectedIndex].text;
    if (!s && tag !== 'input' && tag !== 'select' && tag !== 'textarea') s = el.innerText || el.textContent;
    if (!s && tag === 'input' && ['submit', 'button', 'reset'].includes(el.type)) s = el.value;
    if (!s) s = el.getAttribute('placeholder') || el.getAttribute('title') || el.getAttribute('name')
      || el.getAttribute('href') || '';
    return clean(s);
  };
  let taps = 0;
  const smallTaps = [];
  for (const el of body.querySelectorAll('a[href], button, input, select, textarea, summary, label')) {
    const tag = el.localName;
    if (tag === 'input' && el.type === 'hidden') continue;
    if (tag === 'label' && !el.querySelector('input:not([type=hidden]), select, textarea')) continue;
    // A check box or radio button inside a label is tapped through the label.
    if (tag === 'input' && (el.type === 'checkbox' || el.type === 'radio') && el.closest('label')) continue;
    const r = el.getBoundingClientRect();
    if (r.width <= 1 || r.height <= 1) continue;     // not shown, or visually hidden
    if (r.right + sx <= 0 || r.bottom + sy <= 0) continue;   // parked off screen
    if (invisible(el)) continue;
    taps++;
    if (r.width < tapMin || r.height < tapMin)
      smallTaps.push({ path: path(el), width: round(r.width), height: round(r.height), text: label(el),
        inline: style(el).display === 'inline' });
  }

  // The header: where each of its parts is, which of them overlap, which
  // reach past the screen's edges, and how many lines the search box, the
  // bell, the running-jobs indicator and the user's corner are on.
  const header = (() => {
    const parts = {
      brand: '.site-header .brand', tabs: '#primary-nav', search: '.site-header .search-slot',
      search_kind: '#search-kind', search_field: '#search-q', bell: '#attention-bell',
      bell_badge: '#attention-bell .attn-bell-badge', activity: '.site-header .activity',
      user: '.site-header .user-menu', user_name: '.site-header .user-name',
    };
    const boxes = {};
    for (const [name, sel] of Object.entries(parts)) {
      const el = document.querySelector(sel);
      if (!el) continue;
      const r = el.getBoundingClientRect();
      if (r.width <= 0 || r.height <= 0) continue;
      boxes[name] = { left: round(r.left), top: round(r.top), right: round(r.right), bottom: round(r.bottom),
        width: round(r.width), height: round(r.height) };
    }
    const outer = ['brand', 'tabs', 'search', 'bell', 'activity', 'user'].filter((n) => boxes[n]);
    const overlaps = [];
    for (let i = 0; i < outer.length; i++)
      for (let k = i + 1; k < outer.length; k++) {
        const a = boxes[outer[i]], b = boxes[outer[k]];
        if (Math.min(a.right, b.right) - Math.max(a.left, b.left) > 0.5
            && Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > 0.5)
          overlaps.push(outer[i] + '/' + outer[k]);
      }
    const past = Object.keys(boxes).filter((n) => boxes[n].right > vw + 0.5 || boxes[n].left < -0.5);
    // Two parts are on one line when the middle of each is inside the other.
    const sameLine = (a, b) => {
      const ma = (a.top + a.bottom) / 2, mb = (b.top + b.bottom) / 2;
      return ma > b.top && ma < b.bottom && mb > a.top && mb < a.bottom;
    };
    const line = (names) => names.filter((n) => boxes[n]).every((n, i, all) => sameLine(boxes[all[0]], boxes[n]));
    const badge = document.querySelector('#attention-bell .attn-bell-badge');
    const bell = document.querySelector('#attention-bell');
    return {
      boxes, overlaps, past_the_edges: past,
      bell: bell ? (bell.getAttribute('aria-label') || '') : null,
      bell_badge: badge ? badge.textContent.trim() : null,
      search_and_bell_on_one_line: line(['search', 'bell']),
      top_row_on_one_line: line(['brand', 'tabs', 'search', 'bell', 'activity', 'user']),
    };
  })();

  // The root's overflow-x, or the body's when the root's is visible, is
  // the viewport's.
  const rootX = style(de).overflowX !== 'visible' ? style(de).overflowX : style(body).overflowX;
  const clipped = ['hidden', 'clip'].includes(rootX);
  const sw = de.scrollWidth;
  const vv = window.visualViewport;
  return {
    url: location.href,
    title: document.title,
    viewport_width: vw,
    // What is on screen, in CSS px: the visual viewport.
    visual_width: vv ? vv.width : window.innerWidth,
    visual_height: vv ? vv.height : window.innerHeight,
    visual_scale: vv ? vv.scale : 1,
    client_width: de.clientWidth,
    inner_width: window.innerWidth,
    inner_height: window.innerHeight,
    pointer_coarse: matchMedia('(pointer: coarse)').matches,
    scroll_width: sw,
    document_height: Math.max(de.scrollHeight, body.scrollHeight),
    viewport_overflow_x: rootX,
    scrolls_sideways: sw > vw && !clipped,
    cut_off_sideways: sw > vw && clipped,
    header: header,
    overflow: { count: offenders.length, examples: overflow },
    contained_overflow: { boxes: boxes.size, examples: contained },
    tap_targets: { visible: taps, small: smallTaps.length, examples: smallTaps.slice(0, examples) },
    text: {
      min_font_px: minFont === null ? null : round(minFont),
      min_font_at: minFontAt,
      small_elements: small.size,
      small_examples: Array.from(small.values()).slice(0, 5),
    },
  };
})
"""

LOGIN_JS = r"""
(function (user, password) {
  const u = document.querySelector('input[name="username"]');
  const p = document.querySelector('input[name="password"]');
  if (!u || !p) return 'the login page has no inputs named username and password';
  const form = u.form || u.closest('form');
  if (!form) return 'the username input is not in a form';
  for (const [el, v] of [[u, user], [p, password]]) {
    el.focus();
    el.value = v;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }
  if (form.requestSubmit) form.requestSubmit();
  else {
    const b = form.querySelector('[type=submit], button:not([type])');
    if (b) b.click(); else form.submit();
  }
  return '';
})
"""

# The page's error message, else its heading.
NOTICE_JS = r"""
(() => {
  for (const sel of ['[role=alert]', '.notice-error', '.notice', 'h1']) {
    const n = document.querySelector(sel);
    if (n && n.innerText.trim()) return n.innerText.replace(/\s+/g, ' ').trim().slice(0, 200);
  }
  return '';
})()
"""


# -- The page list ------------------------------------------------------

class Entry:
    def __init__(self, kind, lineno, name=None, path=None, js=None):
        self.kind = kind          # "page", "login" or "logout"
        self.lineno = lineno
        self.name = name
        self.path = path
        self.js = js


def load_pages(path):
    entries = []
    names = set()
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise ToolError(f"cannot read the page list: {e}") from None
    for lineno, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        where = f"{path}:{lineno}"
        if line.startswith("@"):
            if line == "@login":
                entries.append(Entry("login", lineno))
            elif line == "@logout":
                entries.append(Entry("logout", lineno))
            else:
                raise ToolError(f"{where}: unknown line {line!r} (expected @login or @logout)")
            continue
        parts = line.split(None, 2)
        if len(parts) < 2:
            raise ToolError(f"{where}: expected NAME PATH [JS], got {line!r}")
        name, page = parts[0], parts[1]
        if not NAME_RE.match(name):
            raise ToolError(f"{where}: the name {name!r} may hold only letters, digits, '.', '_' and '-'")
        if name in names:
            raise ToolError(f"{where}: the name {name!r} is used twice")
        if not page.startswith("/"):
            raise ToolError(f"{where}: the path {page!r} must start with '/'")
        names.add(name)
        entries.append(Entry("page", lineno, name, page, parts[2].strip() if len(parts) > 2 else None))
    return entries


# -- Capturing ----------------------------------------------------------

def png_size(data):
    if len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR":
        return struct.unpack("!II", data[16:24])
    return (0, 0)


class Session:
    def __init__(self, cdp, args):
        self.cdp = cdp
        self.base = args.base
        self.out = args.out
        self.tiles = args.tiles
        self.settle_s = args.settle / 1000.0
        self.user = args.user
        self.password = args.password
        self.width = None
        self.frame_id = None

    def setup(self):
        for method in ("Page.enable", "Network.enable", "Runtime.enable", "Log.enable"):
            self.cdp.call(method)
        tree = self.cdp.call("Page.getFrameTree")
        self.frame_id = tree["frameTree"]["frame"]["id"]

    def emulate(self, wname):
        if self.width == wname:
            return
        w = WIDTHS[wname]
        params = {
            "width": w["width"], "height": w["height"], "deviceScaleFactor": w["scale"],
            "mobile": w["mobile"], "screenWidth": w["width"], "screenHeight": w["height"],
        }
        if w["mobile"]:
            params["screenOrientation"] = {"type": "portraitPrimary", "angle": 0}
        self.cdp.call("Emulation.setDeviceMetricsOverride", params)
        touch = {"enabled": w["touch"]}
        if w["touch"]:
            touch["maxTouchPoints"] = 5
        self.cdp.call("Emulation.setTouchEmulationEnabled", touch)
        self.width = wname

    def _main_frame(self, params):
        return params.get("frameId") == self.frame_id

    def wait_load(self, start, warnings):
        ev = self.cdp.wait_event("Page.loadEventFired", start, timeout=NAVIGATION_TIMEOUT)
        deadline = time.monotonic() + (5.0 if ev is not None else 0.0)
        while True:
            state = self.cdp.evaluate("document.readyState")
            if state == "complete" or time.monotonic() >= deadline:
                break
            self.cdp.pump(0.1)
        if state != "complete":
            warnings.append(f"the page did not finish loading within {NAVIGATION_TIMEOUT:g} s "
                            f"(readyState {state})")

    def navigate(self, url, warnings):
        self.cdp.events.clear()
        res = self.cdp.call("Page.navigate", {"url": url}, timeout=NAVIGATION_TIMEOUT)
        self.frame_id = res.get("frameId", self.frame_id)
        error = res.get("errorText")
        if error and error != "net::ERR_HTTP_RESPONSE_CODE_FAILURE":
            raise CaptureError(f"cannot open {url}: {error}")
        if error:
            warnings.append("the response had no body; Chrome shows its own error page")
        if "loaderId" in res:
            self.wait_load(0, warnings)

    def document_status(self, start):
        status, redirects = None, []
        for ev in self.cdp.events[start:]:
            p = ev.get("params", {})
            if p.get("type") != "Document" or not self._main_frame(p):
                continue
            if ev["method"] == "Network.requestWillBeSent" and p.get("redirectResponse"):
                rr = p["redirectResponse"]
                redirects.append({"status": rr.get("status"), "url": rr.get("url")})
            elif ev["method"] == "Network.requestWillBeSent":
                redirects = []      # a new navigation
            elif ev["method"] == "Network.responseReceived":
                status = p["response"].get("status")
        if status is None:
            status = self.cdp.evaluate(
                "(performance.getEntriesByType('navigation')[0] || {}).responseStatus || null")
        return status, redirects

    def browser_errors(self):
        messages = []
        for ev in self.cdp.events:
            p = ev.get("params", {})
            if ev["method"] == "Runtime.exceptionThrown":
                messages.append(describe_exception(p.get("exceptionDetails", {})))
            elif ev["method"] == "Runtime.consoleAPICalled" and p.get("type") == "error":
                args = p.get("args", [])
                messages.append(" ".join(str(a.get("value", a.get("description", ""))) for a in args)[:300])
            elif ev["method"] == "Log.entryAdded" and p.get("entry", {}).get("level") == "error":
                e = p["entry"]
                messages.append((e.get("text", "") + (f" ({e['url']})" if e.get("url") else ""))[:300])
        unique = list(dict.fromkeys(messages))
        return {"count": len(messages), "messages": unique[:5]}

    def settle(self, warnings):
        self.cdp.pump(self.settle_s)
        try:
            self.cdp.evaluate("document.fonts ? document.fonts.ready.then(() => true) : true",
                              await_promise=True, timeout=5)
        except (JSError, TimeoutError, CDPError):
            pass
        deadline = time.monotonic() + HTMX_WAIT
        while self.cdp.evaluate("document.querySelector('.htmx-request') !== null"):
            if time.monotonic() >= deadline:
                warnings.append(f"htmx was still loading after {HTMX_WAIT:g} s")
                break
            self.cdp.pump(0.1)

    def login(self):
        if not self.user:
            raise ToolError("@login needs --user and --password")
        warnings = []
        self.emulate(self.width or "desktop")
        try:
            self.navigate(self.base + "/login", warnings)
        except CaptureError as e:
            raise ToolError(f"cannot log in: {e}") from None
        where = urllib.parse.urlsplit(self.cdp.evaluate("location.href"))
        if where.path == "/setup":
            raise ToolError("cannot log in: the instance has no administrator yet (/login sends to /setup)")
        if where.path != "/login":
            print(f"login: already logged in (/login went on to {where.path})")
            return
        start = len(self.cdp.events)
        problem = self.cdp.evaluate(f"({LOGIN_JS})({json.dumps(self.user)}, {json.dumps(self.password)})",
                                    user_gesture=True)
        if problem:
            raise ToolError(f"cannot log in: {problem}")
        ev = self.cdp.wait_event("Page.frameNavigated", start,
                                 lambda p: not p.get("frame", {}).get("parentId"), timeout=NAVIGATION_TIMEOUT)
        if ev is None:
            raise ToolError(f"cannot log in: submitting the form opened no page within {NAVIGATION_TIMEOUT:g} s")
        self.wait_load(start, warnings)
        where = urllib.parse.urlsplit(self.cdp.evaluate("location.href"))
        if where.path == "/login":
            status, _ = self.document_status(start)
            notice = self.cdp.evaluate(NOTICE_JS)
            raise ToolError(f"cannot log in as {self.user}: still on the login page (HTTP {status}): "
                            f"{notice or 'no message'}")
        print(f"login: logged in as {self.user} (landed on {where.path})")

    def logout(self):
        self.cdp.call("Network.clearBrowserCookies")
        print("logout: cookies cleared")

    def capture(self, entry, wname):
        w = WIDTHS[wname]
        url = self.base + entry.path
        rec = {
            "name": entry.name, "path": entry.path, "width": wname,
            "viewport": f"{w['width']}x{w['height']}@{w['scale']}x", "url": url,
            "captured": False, "status": None, "redirects": [], "final_url": None,
            "snippet": entry.js, "snippet_error": None, "images": [],
            "errors": [], "warnings": [],
        }
        t0 = time.monotonic()
        try:
            self.emulate(wname)
            self.navigate(url, rec["warnings"])
            self.settle(rec["warnings"])
            start = 0
            if entry.js:
                start = len(self.cdp.events)
                try:
                    self.cdp.evaluate(entry.js, await_promise=True, user_gesture=True, by_value=False)
                except JSError as e:
                    rec["snippet_error"] = str(e)
                except TimeoutError:
                    rec["snippet_error"] = f"did not finish within {COMMAND_TIMEOUT:g} s"
                self.cdp.pump(self.settle_s)
                # Did the snippet open another page (a click on a link, a submit)?
                nav = self.cdp.wait_event("Network.requestWillBeSent", start,
                                          lambda p: p.get("type") == "Document" and self._main_frame(p),
                                          timeout=0)
                if nav is not None:
                    self.wait_load(start, rec["warnings"])
                else:
                    start = 0
                self.settle(rec["warnings"])
            rec["status"], rec["redirects"] = self.document_status(start)
            self.cdp.evaluate("window.scrollTo({top: 0, left: 0, behavior: 'instant'})")
            self.cdp.evaluate(FRAMES_JS, await_promise=True)
            m = self.cdp.evaluate(f"({MEASURE_JS})({w['width']}, {TAP_MIN}, {SMALL_TEXT}, {EXAMPLES})",
                                  timeout=30)
            rec["final_url"] = m.pop("url")
            rec["title"] = m.pop("title")
            rec["measure"] = m
            if m["client_width"] and m["client_width"] != w["width"]:
                rec["warnings"].append(f"the layout is {m['client_width']} px wide, not {w['width']}: "
                                       "does the page have a viewport meta tag?")
            rec["images"] = self.shoot(entry.name, wname, m)
            rec["browser_errors"] = self.browser_errors()
            rec["captured"] = True
        except (CaptureError, CDPError, JSError, TimeoutError) as e:
            rec["errors"].append(str(e))
        rec["seconds"] = round(time.monotonic() - t0, 2)
        print(capture_line(rec), flush=True)
        return rec

    def screenshot(self, clip=None, beyond=False, scale=1):
        """A PNG of the screen, or of CLIP (CSS px, in page coordinates)."""
        params = {"format": "png", "captureBeyondViewport": beyond}
        if clip is not None:
            params["clip"] = dict(clip, scale=scale)
        res = self.cdp.call("Page.captureScreenshot", params, timeout=SCREENSHOT_TIMEOUT)
        return base64.b64decode(res["data"])

    def write(self, filename, data, extra):
        with open(os.path.join(self.out, filename), "wb") as f:
            f.write(data)
        width, height = png_size(data)
        return dict({"file": filename, "width_px": width, "height_px": height}, **extra)

    def shoot(self, name, wname, m):
        # Tiles of an earlier run that this one will not overwrite.
        old = re.compile(re.escape(f"{name}-{wname}-") + r"[0-9]+\.png")
        for filename in os.listdir(self.out):
            if old.fullmatch(filename):
                os.remove(os.path.join(self.out, filename))
        images = []
        vw = m["viewport_width"]
        # Step by what is on screen (the visual viewport), not by innerHeight.
        screen_w = m["visual_width"] or vw
        vh = m["visual_height"] or WIDTHS[wname]["height"]
        doc_h = m["document_height"]
        for i in range(self.tiles):
            y = i * vh
            if y >= doc_h - 0.5:
                break
            self.cdp.evaluate(f"window.scrollTo({{top: {y}, left: 0, behavior: 'instant'}})")
            top = self.cdp.evaluate(FRAMES_JS, await_promise=True) or 0
            if top >= y - 0.5:
                # The screen, as it is.
                data = self.screenshot()
                extra = {"top": round(top, 1), "height_css": round(vh, 1)}
            else:
                # Scrolling stopped short: at the end of the page, or (on a
                # phone, when something sticks out sideways) at the end of
                # the layout viewport Chrome widened, which a script cannot
                # scroll past but a finger can.  Take the part of the page
                # not shown yet, rendering beyond the screen if need be.
                h = min(vh, doc_h - y)
                beyond = y + h > top + vh + 0.5
                data = self.screenshot({"x": 0, "y": y, "width": screen_w, "height": h},
                                       beyond=beyond, scale=m["visual_scale"] or 1)
                extra = {"top": round(y, 1), "height_css": round(h, 1), "beyond_screen": beyond}
            images.append(self.write(f"{name}-{wname}-{i + 1}.png", data, extra))
        self.cdp.evaluate("window.scrollTo({top: 0, left: 0, behavior: 'instant'})")
        self.cdp.evaluate(FRAMES_JS, await_promise=True)
        full_h = max(1, min(doc_h, FULL_PAGE_CAP))
        data = self.screenshot({"x": 0, "y": 0, "width": vw, "height": full_h}, beyond=True)
        images.append(self.write(f"{name}-{wname}-full.png", data,
                                 {"top": 0, "height_css": full_h, "cut_at_cap": doc_h > FULL_PAGE_CAP}))
        self.cdp.evaluate("window.scrollTo({top: 0, left: 0, behavior: 'instant'})")
        return images


# -- Reports ------------------------------------------------------------

def url_path(url):
    return urllib.parse.urlsplit(url).path if url else ""


def capture_line(rec):
    status = rec["status"] if rec["status"] is not None else "-"
    head = f"{rec['width']:<7} {rec['name']:<18} {status!s:>4}"
    if not rec["captured"]:
        return f"{head}  NOT CAPTURED: {'; '.join(rec['errors'])}"
    m = rec["measure"]
    tiles = [i for i in rec["images"] if not i["file"].endswith("-full.png")]
    full = rec["images"][-1]
    side = "yes" if m["scrolls_sideways"] else ("cut off" if m["cut_off_sideways"] else "no")
    parts = [
        head,
        f"tiles {len(tiles)}",
        f"full {full['width_px']}x{full['height_px']}",
        f"sideways {side} ({m['scroll_width']}/{m['viewport_width']})",
        f"small taps {m['tap_targets']['small']}/{m['tap_targets']['visible']}",
        f"min font {m['text']['min_font_px']}px",
    ]
    if url_path(rec["final_url"]) != url_path(rec["url"]):
        parts.append(f"-> {url_path(rec['final_url'])}")
    if rec["snippet_error"]:
        parts.append(f"snippet threw: {rec['snippet_error']}")
    return "  ".join(parts)


def report_text(meta, records):
    out = [
        f"Axis screenshots of {meta['base']}",
        f"Taken {meta['generated']} with {meta['chrome']}",
    ]
    if meta.get("fatal"):
        out.append(f"The run stopped early: {meta['fatal']}")
    for rec in records:
        out.append("")
        out.append(f"== {rec['name']} ({rec['path']}) at {rec['width']} {rec['viewport']} ==")
        redirects = "".join(f"{r['status']} {r['url']} -> " for r in rec["redirects"])
        at = f" at {rec['final_url']}" if rec["final_url"] else ""
        out.append(f"status: {redirects}{rec['status']}{at}")
        if rec.get("title"):
            out.append(f"title: {rec['title']}")
        for e in rec["errors"]:
            out.append(f"NOT CAPTURED: {e}")
        for msg in rec["warnings"]:
            out.append(f"warning: {msg}")
        if rec["snippet"]:
            out.append(f"snippet: {rec['snippet']}")
            if rec["snippet_error"]:
                out.append(f"snippet threw: {rec['snippet_error']}")
        if not rec["captured"]:
            continue
        m = rec["measure"]
        out.append("images: " + ", ".join(f"{i['file']} ({i['width_px']}x{i['height_px']})" for i in rec["images"]))
        if m["scrolls_sideways"]:
            side = "scrolls sideways"
        elif m["cut_off_sideways"]:
            side = f"wider than the screen and cut off (overflow-x: {m['viewport_overflow_x']})"
        else:
            side = "does not scroll sideways"
        if m["inner_width"] > m["viewport_width"]:
            side += f" (Chrome widened the layout viewport to {m['inner_width']} px)"
        out.append(f"width: scroll width {m['scroll_width']} px, viewport {m['viewport_width']} px: {side}; "
                   f"page {m['document_height']} px tall")
        ov, co = m["overflow"], m["contained_overflow"]
        inside = f"; {co['boxes']} box(es) that scroll or clip sideways hold more" if co["boxes"] else ""
        out.append(f"past the right edge: {ov['count']} element(s){inside}")
        for x in ov["examples"]:
            text = f" \"{x['text']}\"" if x.get("text") else ""
            mark = "" if x["innermost"] else " (holds another)"
            out.append(f"  {x['kind']:<4} right {x['right']:>7} width {x['width']:>7}  {x['path']}{text}{mark}")
        for b in co["examples"]:
            out.append(f"  contained: {b['path']} (overflow-x {b['overflow_x']}, "
                       f"{b['scroll_width']} px in {b['client_width']} px)")
        hd = m.get("header") or {}
        if hd.get("boxes"):
            bell = f"bell \"{hd['bell']}\" badge {hd['bell_badge'] or 'none'}" if hd.get("bell") is not None \
                else "no bell"
            out.append(f"header: {bell}; overlaps: {', '.join(hd['overlaps']) or 'none'}; "
                       f"past the edges: {', '.join(hd['past_the_edges']) or 'none'}; "
                       f"search and bell on one line: {'yes' if hd['search_and_bell_on_one_line'] else 'no'}; "
                       f"top row on one line: {'yes' if hd['top_row_on_one_line'] else 'no'}")
            for name, b in hd["boxes"].items():
                out.append(f"  {name:<12} left {b['left']:>7} right {b['right']:>7} top {b['top']:>6} "
                           f"width {b['width']:>6} height {b['height']:>5}")
        tt = m["tap_targets"]
        out.append(f"tap targets: {tt['visible']} visible, {tt['small']} under {TAP_MIN} px")
        for x in tt["examples"]:
            inline = " (inline)" if x["inline"] else ""
            out.append(f"  {x['width']:>6} x {x['height']:<6} {x['path']} \"{x['text']}\"{inline}")
        tx = m["text"]
        where = f" ({tx['min_font_at']['path']} \"{tx['min_font_at']['text']}\")" if tx["min_font_at"] else ""
        out.append(f"smallest text: {tx['min_font_px']} px{where}; "
                   f"elements with text under {SMALL_TEXT} px: {tx['small_elements']}")
        for x in tx["small_examples"]:
            out.append(f"  {x['font_px']:>5} px {x['path']} \"{x['text']}\"")
        be = rec.get("browser_errors") or {}
        if be.get("count"):
            out.append(f"browser errors: {be['count']}")
            for msg in be["messages"]:
                out.append(f"  {msg}")
    return "\n".join(out) + "\n"


def summary(records, report_path, fatal):
    lines = ["", "Summary"]
    phone = [r for r in records if r["width"] == "phone" and r["captured"]]
    wide = [r for r in phone if r["measure"]["scrolls_sideways"] or r["measure"]["cut_off_sideways"]]
    if not phone:
        lines.append("No phone captures.")
    elif wide:
        lines.append("Wider than the phone screen:")
        for r in wide:
            m = r["measure"]
            how = "scrolls sideways" if m["scrolls_sideways"] else "cut off"
            first = m["overflow"]["examples"][0]["path"] if m["overflow"]["examples"] else "?"
            lines.append(f"  {r['name']}: {how}, {m['scroll_width']} px in {m['viewport_width']} px; "
                         f"first: {first}")
    else:
        lines.append("No page is wider than the phone screen.")
    missed = [r for r in records if not r["captured"]]
    if missed:
        lines.append("Not captured:")
        for r in missed:
            lines.append(f"  {r['name']} at {r['width']}: {'; '.join(r['errors'])}")
    odd = [r for r in records if r["captured"] and r["status"] != 200]
    if odd:
        lines.append("Status other than 200: " + ", ".join(f"{r['name']} {r['width']} {r['status']}" for r in odd))
    threw = [r for r in records if r["snippet_error"]]
    if threw:
        lines.append("Snippets that threw: " + ", ".join(f"{r['name']} {r['width']}" for r in threw))
    if phone:
        lines.append(f"Phone tap targets under {TAP_MIN} px: "
                     + ", ".join(f"{r['name']} {r['measure']['tap_targets']['small']}" for r in phone))
        lines.append(f"Phone elements with text under {SMALL_TEXT} px: "
                     + ", ".join(f"{r['name']} {r['measure']['text']['small_elements']}" for r in phone))
    if fatal:
        lines.append(f"Stopped early: {fatal}")
    done = sum(1 for r in records if r["captured"])
    lines.append(f"Captured {done} of {len(records)}; report: {report_path}")
    return "\n".join(lines)


# -- Main ---------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="Take screenshots of a running Axis web instance with headless Chrome.",
        epilog="The page list format and the outputs are described at the top of this script.")
    ap.add_argument("--base", required=True, metavar="URL",
                    help="the instance, for example http://127.0.0.1:20080")
    ap.add_argument("--out", required=True, metavar="DIR", help="where the images and reports go")
    ap.add_argument("--user", metavar="NAME", help="log in as NAME first")
    ap.add_argument("--password", metavar="PW", help="the password for --user")
    ap.add_argument("--pages", metavar="FILE", help="the page list")
    ap.add_argument("--widths", default="phone,desktop", metavar="LIST",
                    help="phone, desktop or both (default phone,desktop)")
    ap.add_argument("--tiles", type=int, default=4, metavar="N",
                    help="at most N viewport-high images (default 4)")
    ap.add_argument("--chrome", default="google-chrome", metavar="PATH",
                    help="the Chrome binary (default google-chrome)")
    ap.add_argument("--only", metavar="NAMES", help="capture only these entries (comma-separated names)")
    ap.add_argument("--settle", type=int, default=300, metavar="MS",
                    help="ms to wait after a load or snippet (default 300)")
    args = ap.parse_args(argv)
    parts = urllib.parse.urlsplit(args.base)
    if parts.scheme not in ("http", "https") or not parts.netloc:
        ap.error("--base must be an http:// or https:// URL")
    args.base = args.base.rstrip("/")
    if (args.user is None) != (args.password is None):
        ap.error("--user and --password go together")
    widths = [w.strip() for w in args.widths.split(",") if w.strip()]
    for w in widths:
        # A size of one's own: 1248x800 (a desktop), 360x780m (a phone).
        custom = re.fullmatch(r"([1-9][0-9]{2,3})x([1-9][0-9]{2,3})(m?)", w)
        if w not in WIDTHS and custom:
            phone = custom.group(3) == "m"
            WIDTHS[w] = {"width": int(custom.group(1)), "height": int(custom.group(2)),
                         "scale": 2 if phone else 1, "mobile": phone, "touch": phone}
        if w not in WIDTHS:
            ap.error(f"unknown width {w!r} (expected one of {', '.join(WIDTHS)}, or a size such as "
                     "1248x800, or 360x780m for a phone)")
    args.widths = list(dict.fromkeys(widths))
    if not args.widths:
        ap.error("--widths is empty")
    if args.tiles < 0:
        ap.error("--tiles must be 0 or more")
    if args.settle < 0:
        ap.error("--settle must be 0 or more")
    args.only = [n.strip() for n in args.only.split(",") if n.strip()] if args.only else None
    return args


def main(argv=None):
    args = parse_args(argv)
    sys.stdout.reconfigure(line_buffering=True)    # in order with stderr, also into a file
    try:
        entries = load_pages(args.pages) if args.pages else [Entry("page", 0, "home", "/")]
        if any(e.kind == "login" for e in entries) and not args.user:
            raise ToolError("the page list has @login, which needs --user and --password")
        if args.only:
            known = {e.name for e in entries if e.kind == "page"}
            unknown = [n for n in args.only if n not in known]
            if unknown:
                raise ToolError(f"--only names entries the list does not have: {', '.join(unknown)}")
        os.makedirs(args.out, exist_ok=True)
    except (ToolError, OSError) as e:
        print(f"axis-screenshots: {e}", file=sys.stderr)
        return 1

    def on_signal(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)

    chrome = Chrome(args.chrome)
    ws = None
    records = []
    fatal = None
    try:
        chrome.start()
        print(f"chrome: {chrome.version.get('Browser', '?')} on 127.0.0.1:{chrome.port}")
        ws = WebSocket(chrome.page_websocket_url())
        session = Session(CDP(ws), args)
        session.setup()
        if args.user:
            session.login()
        for entry in entries:
            if entry.kind == "logout":
                session.logout()
            elif entry.kind == "login":
                session.login()
            elif args.only is None or entry.name in args.only:
                for wname in args.widths:
                    records.append(session.capture(entry, wname))
    except KeyboardInterrupt:
        fatal = "interrupted"
    except ToolError as e:
        fatal = str(e)
    except (WebSocketError, CDPError, JSError, OSError, urllib.error.URLError, ValueError) as e:
        fatal = f"lost Chrome: {type(e).__name__}: {e}"
    finally:
        old_int = signal.signal(signal.SIGINT, signal.SIG_IGN)
        if ws is not None:
            ws.close()
        chrome.stop()
        signal.signal(signal.SIGINT, old_int)

    if fatal:
        print(f"axis-screenshots: {fatal}", file=sys.stderr)
    meta = {
        "base": args.base,
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "chrome": chrome.version.get("Browser", "?"),
        "widths": {w: WIDTHS[w] for w in args.widths},
        "fatal": fatal,
    }
    report_txt = os.path.join(args.out, "report.txt")
    try:
        with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as f:
            json.dump(dict(meta, captures=records), f, indent=2)
            f.write("\n")
        with open(report_txt, "w", encoding="utf-8") as f:
            f.write(report_text(meta, records))
    except OSError as e:
        print(f"axis-screenshots: cannot write the report: {e}", file=sys.stderr)
        return 1
    print(summary(records, report_txt, fatal))
    return 0 if not fatal and all(r["captured"] for r in records) else 1


if __name__ == "__main__":
    sys.exit(main())
