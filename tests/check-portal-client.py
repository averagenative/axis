#!/usr/bin/env python3
# check-portal-client.py - the small client check-container.sh and
# check-examples.sh use for the request portal (axis-portal)
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   check-portal-client.py ask BASE HOST USER PASSWORD TITLE [CLIENT]
#       What a requester does, as a browser does it: opens the login page of
#       the portal at BASE (a URL; TLS is not verified), sending HOST as the
#       Host header (empty: none), logs in as USER, opens the page of the
#       title numbered TITLE, asks for it (a movie), and opens "My requests".
#       With CLIENT, the client plays the reverse proxy itself: every request
#       carries X-Forwarded-Proto: https and X-Forwarded-For: CLIENT (for a
#       check that has no proxy; the portal must list this client's address
#       as a trusted proxy).  Prints one line for each step:
#         form=NNN                  the login page's status
#         login=NNN                 303 when the login worked
#         cookie=NAME secure|plain  one for each cookie the portal set
#         title=NNN                 the title's page
#         ask=NNN made|not-made     the answer to the request
#         requests=NNN listed|not-listed
#   check-portal-client.py engine-login BASE HOST USER PASSWORD
#       The login of Axis itself (not the portal's) at BASE, with HOST as the
#       Host header (empty: none): opens /login and posts USER and PASSWORD
#       with the form's token, whatever the page was.  Prints:
#         form=NNN                  the login page's status (000: no answer)
#         login=NNN                 303 when the login worked
#         session=yes|no            whether a session cookie of Axis was set
#   check-portal-client.py page BASE HOST PATH
#       GET of PATH at BASE with HOST as the Host header, redirects not
#       followed.  Prints "status=NNN" (000: no answer), "hsts=VALUE" (or
#       "-"), "cookie=NAME secure|plain" for each cookie set, and
#       "password-field=yes|no".
#   check-portal-client.py api-hello BASE
#       The portal API itself at BASE (the daemon's portal listener), with the
#       portal token read from standard input (so: the script from a file).
#       Prints "hello=NNN BODY".
#   check-portal-client.py paths BASE HOST PATH...
#       GET of each PATH, redirects not followed.  Prints "PATH NNN" for each
#       (000 when there was no answer).
#   check-portal-client.py reach HOST PORT
#       Whether a TCP connection to HOST:PORT can be made from where this
#       runs.  Prints "reach=open", "reach=refused", "reach=unreachable" (no
#       route to it), "reach=unresolved" (the name is not known here) or
#       "reach=timeout".
#   check-portal-client.py v6 http|https PORT HOST USER PASSWORD [TARGET...]
#       Whether the container this runs in has an IPv6 address, and what the
#       login of Axis comes to over IPv6.  Prints "inet6=none", or
#       "inet6=ADDRESS INTERFACE" for each address that is not loopback's;
#       then, for each TARGET (an IPv6 address without a zone) and each
#       interface of this container, "try=TARGET%INTERFACE connect=failed
#       (WHY)" or "try=... connect=open form=NNN login=NNN session=yes|no".
#   check-portal-client.py probe HOST PORT
#       For a check that failed: the resolver's configuration, what HOST
#       resolves to and how long that took, and what a connection to each
#       address comes to.
#
# Standard library only: it runs in the Axis image itself (which has python3
# for yt-dlp), read from standard input (`python3 - reach HOST PORT < file`),
# so that a check can ask from inside a container that has no mount.

import errno
import http.cookiejar
import re
import socket
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class AnyCookies(http.cookiejar.DefaultCookiePolicy):
    """Keeps a cookie whatever the host is called: the checks reach the
    portal by an address and name it in the Host header."""

    def set_ok(self, cookie, request):
        return True

    def return_ok(self, cookie, request):
        return True

    def domain_return_ok(self, domain, request):
        return True

    def path_return_ok(self, path, request):
        return True


def make_opener(jar):
    context = ssl._create_unverified_context()
    return urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar),
        urllib.request.HTTPSHandler(context=context),
        NoRedirect,
    )


def fetch(opener, request):
    try:
        with opener.open(request, timeout=30) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError):
        return 0, ""


def csrf_value(page, action):
    """The _csrf field of the form that posts to @action."""
    start = page.find('action="%s"' % action)
    if start < 0:
        return ""
    end = page.find("</form>", start)
    for tag in re.findall(r"<input[^>]*>", page[start : end if end > 0 else len(page)]):
        if 'name="_csrf"' in tag:
            match = re.search(r'value="([^"]*)"', tag)
            if match:
                return match.group(1)
    return ""


def ask(base, host, user, password, title, client):
    jar = http.cookiejar.CookieJar(AnyCookies())
    opener = make_opener(jar)
    headers = {"Host": host} if host else {}
    if client:
        headers["X-Forwarded-Proto"] = "https"
        headers["X-Forwarded-For"] = client

    status, page = fetch(opener, urllib.request.Request(base + "/login", headers=headers))
    print("form=%03d" % status)
    if status != 200:
        return
    form = urllib.parse.urlencode(
        {"_csrf": csrf_value(page, "/login"), "username": user, "password": password}
    ).encode()
    status, _ = fetch(
        opener, urllib.request.Request(base + "/login", data=form, headers=headers, method="POST")
    )
    print("login=%03d" % status)
    for cookie in jar:
        print("cookie=%s %s" % (cookie.name, "secure" if cookie.secure else "plain"))
    if status != 303:
        return

    path = "/title/%d" % int(title)
    status, page = fetch(opener, urllib.request.Request(base + path, headers=headers))
    print("title=%03d" % status)
    if status != 200:
        return
    form = urllib.parse.urlencode(
        {"_csrf": csrf_value(page, path + "/request"), "kind": "movie"}
    ).encode()
    status, page = fetch(
        opener,
        urllib.request.Request(base + path + "/request", data=form, headers=headers, method="POST"),
    )
    # The answer to a request is the title's page, with what the daemon said.
    made = status == 200 and "Your request is made." in page
    print("ask=%03d %s" % (status, "made" if made else "not-made"))

    status, page = fetch(opener, urllib.request.Request(base + "/requests", headers=headers))
    listed = status == 200 and ('href="%s"' % path) in page
    print("requests=%03d %s" % (status, "listed" if listed else "not-listed"))


def engine_login(base, host, user, password):
    """What whoever holds the portal's container would do with an
    administrator's password: the engine's own login, from here."""
    jar = http.cookiejar.CookieJar(AnyCookies())
    opener = make_opener(jar)
    headers = {"Host": host} if host else {}

    status, page = fetch(opener, urllib.request.Request(base + "/login", headers=headers))
    print("form=%03d" % status)
    token = ""
    for tag in re.findall(r"<input[^>]*>", page):
        if 'name="_csrf"' in tag:
            match = re.search(r'value="([^"]*)"', tag)
            if match:
                token = match.group(1)
    # Posted whatever the page was: a refusal must not depend on the form.
    form = urllib.parse.urlencode({"_csrf": token, "username": user, "password": password}).encode()
    post = dict(headers)
    post["Origin"] = base if not host else "%s://%s" % (base.split(":", 1)[0], host)
    status, _ = fetch(opener, urllib.request.Request(base + "/login", data=form, headers=post, method="POST"))
    print("login=%03d" % status)
    session = any(cookie.name.endswith("axis_session") and cookie.value for cookie in jar)
    print("session=%s" % ("yes" if session else "no"))


def api_hello(base):
    """The portal API's hello at @base, with the token on standard input."""
    token = sys.stdin.readline().strip()
    request = urllib.request.Request(
        base + "/api/v1/portal/hello",
        data=b"{}",
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"},
        method="POST",
    )
    status, body = fetch(make_opener(http.cookiejar.CookieJar(AnyCookies())), request)
    print("hello=%03d %s" % (status, " ".join(body.split())[:200]))


def page(base, host, path):
    """One page as a browser gets it first: its status, and what its headers
    and its body say that the checks ask about."""
    headers = {"Host": host} if host else {}
    opener = make_opener(http.cookiejar.CookieJar(AnyCookies()))
    try:
        response = opener.open(urllib.request.Request(base + path, headers=headers), timeout=30)
    except urllib.error.HTTPError as error:
        response = error
    except (urllib.error.URLError, OSError):
        print("status=000")
        return
    body = response.read().decode("utf-8", "replace")
    print("status=%03d" % response.getcode())
    print("hsts=%s" % (response.headers.get("Strict-Transport-Security") or "-"))
    for value in response.headers.get_all("Set-Cookie") or []:
        name = value.split("=", 1)[0].strip()
        secure = any(part.strip().lower() == "secure" for part in value.split(";")[1:])
        print("cookie=%s %s" % (name, "secure" if secure else "plain"))
    print("password-field=%s" % ("yes" if 'name="password"' in body else "no"))


def paths(base, host, wanted):
    opener = make_opener(http.cookiejar.CookieJar(AnyCookies()))
    headers = {"Host": host} if host else {}
    for path in wanted:
        status, _ = fetch(opener, urllib.request.Request(base + path, headers=headers))
        print("%s %03d" % (path, status))


def reach(host, port):
    try:
        addresses = socket.getaddrinfo(host, int(port), type=socket.SOCK_STREAM)
    except socket.gaierror:
        print("reach=unresolved")
        return
    result = "refused"
    for family, kind, proto, _, address in addresses:
        try:
            with socket.socket(family, kind, proto) as sock:
                sock.settimeout(4)
                sock.connect(address)
                print("reach=open")
                return
        except socket.timeout:
            result = "timeout"
        except OSError as error:
            if error.errno in (errno.ENETUNREACH, errno.EHOSTUNREACH):
                result = "unreachable"
    print("reach=%s" % result)


def inet6_here():
    """This container's IPv6 addresses that are not loopback's, as (address,
    interface), from the kernel's own table.  No table (IPv6 is switched off
    for the whole container): none."""
    found = []
    try:
        with open("/proc/net/if_inet6", encoding="ascii") as handle:
            for line in handle:
                fields = line.split()
                if len(fields) == 6 and fields[5] != "lo":
                    raw = fields[0]
                    found.append((":".join(raw[i : i + 4] for i in range(0, 32, 4)), fields[5]))
    except OSError:
        pass
    return found


def v6_exchange(scheme, sockaddr, host, user, password):
    """The engine's login over one IPv6 connection.  The connection is made
    here, so that an address with a zone needs no URL."""
    import http.client

    raw = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    raw.settimeout(6)
    try:
        raw.connect(sockaddr)
    except OSError as error:
        raw.close()
        return "connect=failed (%s)" % (errno.errorcode.get(error.errno, "timeout") if error.errno else "timeout")
    try:
        if scheme == "https":
            conn = http.client.HTTPSConnection(host, timeout=10)
            conn.sock = ssl._create_unverified_context().wrap_socket(raw, server_hostname=host)
        else:
            conn = http.client.HTTPConnection(host, timeout=10)
            conn.sock = raw
        conn.request("GET", "/login", headers={"Host": host, "Connection": "keep-alive"})
        response = conn.getresponse()
        page = response.read().decode("utf-8", "replace")
        form = response.status
        cookies = {}
        for value in response.headers.get_all("Set-Cookie") or []:
            name, _, rest = value.partition("=")
            cookies[name.strip()] = rest.split(";", 1)[0]
        token = ""
        for tag in re.findall(r"<input[^>]*>", page):
            if 'name="_csrf"' in tag:
                match = re.search(r'value="([^"]*)"', tag)
                if match:
                    token = match.group(1)
        body = urllib.parse.urlencode({"_csrf": token, "username": user, "password": password})
        headers = {
            "Host": host,
            "Origin": "%s://%s" % (scheme, host),
            "Content-Type": "application/x-www-form-urlencoded",
            "Cookie": "; ".join("%s=%s" % pair for pair in cookies.items()),
        }
        if response.will_close:
            return "connect=open form=%03d login=000 session=no" % form
        conn.request("POST", "/login", body=body, headers=headers)
        response = conn.getresponse()
        response.read()
        session = False
        for value in response.headers.get_all("Set-Cookie") or []:
            name, _, rest = value.partition("=")
            if name.strip().endswith("axis_session") and rest.split(";", 1)[0]:
                session = True
        return "connect=open form=%03d login=%03d session=%s" % (form, response.status, "yes" if session else "no")
    except (OSError, ssl.SSLError, http.client.HTTPException) as error:
        return "connect=open exchange=failed (%s)" % type(error).__name__
    finally:
        raw.close()


def v6(scheme, port, host, user, password, targets):
    """Whether this container has an IPv6 address at all, and what the
    engine's login comes to over IPv6 at each of TARGETS, through each of
    this container's interfaces."""
    mine = inet6_here()
    if not mine:
        print("inet6=none")
    for address, interface in mine:
        print("inet6=%s %s" % (address, interface))
    interfaces = [name for _, name in socket.if_nameindex() if name != "lo"]
    for target in targets:
        for interface in interfaces:
            try:
                scope = socket.if_nametoindex(interface)
            except OSError:
                continue
            result = v6_exchange(scheme, (target, int(port), 0, scope), host, user, password)
            print("try=%s%%%s %s" % (target, interface, result))


def probe(host, port):
    """For a check that failed: what the name resolves to here, how long that
    took, and what a connection to each address comes to."""
    import time

    try:
        with open("/etc/resolv.conf", encoding="utf-8") as handle:
            print("resolv.conf: %s" % " | ".join(handle.read().split("\n")).strip(" |"))
    except OSError:
        pass
    start = time.time()
    try:
        found = sorted({a[4][0] for a in socket.getaddrinfo(host, int(port), type=socket.SOCK_STREAM)})
    except socket.gaierror as error:
        print("%s: unresolved (%s) in %.2f s" % (host, error, time.time() - start))
        return
    print("%s: %s in %.2f s" % (host, ", ".join(found), time.time() - start))
    for address in found:
        start = time.time()
        try:
            with socket.create_connection((address, int(port)), timeout=6):
                result = "open"
        except OSError as error:
            result = "not open (%s)" % error
        print("  %s port %s: %s in %.2f s" % (address, port, result, time.time() - start))


def main(argv):
    if len(argv) == 4 and argv[1] == "probe":
        probe(argv[2], argv[3])
        return 0
    if len(argv) in (7, 8) and argv[1] == "ask":
        ask(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7] if len(argv) == 8 else "")
    elif len(argv) == 5 and argv[1] == "page":
        page(argv[2], argv[3], argv[4])
    elif len(argv) == 3 and argv[1] == "api-hello":
        api_hello(argv[2])
    elif len(argv) == 6 and argv[1] == "engine-login":
        engine_login(argv[2], argv[3], argv[4], argv[5])
    elif len(argv) >= 5 and argv[1] == "paths":
        paths(argv[2], argv[3], argv[4:])
    elif len(argv) == 4 and argv[1] == "reach":
        reach(argv[2], argv[3])
    elif len(argv) >= 7 and argv[1] == "v6" and argv[2] in ("http", "https"):
        v6(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7:])
    else:
        sys.stderr.write("usage: see the head of tests/check-portal-client.py\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
