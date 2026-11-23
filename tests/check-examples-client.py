#!/usr/bin/env python3
# check-examples-client.py - the small HTTP client check-examples.sh runs in a
# container of its own, so that a request reaches Axis from an address that is
# neither the host's nor Traefik's.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   check-examples-client.py login BASE HOST ORIGIN USER PASSWORD
#       Logs in through BASE (a URL; TLS is not verified), sending HOST as the
#       Host header (empty: none).  Prints "status=NNN" and one
#       "cookie=NAME secure|plain" line per cookie the server set.
#   check-examples-client.py forged BASE
#       Sends the forwarded headers a client might forge to BASE, and prints
#       the cookies it gets back ("cookie=NAME secure|plain") and the status of
#       a worker-API request with a token that is no worker's ("claim=NNN").
#
# Standard library only, so it runs in any python image.

import http.cookiejar
import re
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def make_opener(jar):
    context = ssl._create_unverified_context()
    return urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar),
        urllib.request.HTTPSHandler(context=context),
        NoRedirect,
    )


def fetch(opener, request):
    try:
        with opener.open(request, timeout=20) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace")


def show_cookies(jar):
    for cookie in jar:
        print("cookie=%s %s" % (cookie.name, "secure" if cookie.secure else "plain"))


def csrf_value(page):
    for tag in re.findall(r"<input[^>]*>", page):
        if 'name="_csrf"' in tag:
            match = re.search(r'value="([^"]*)"', tag)
            if match:
                return match.group(1)
    return ""


def login(base, host, origin, user, password):
    jar = http.cookiejar.CookieJar()
    opener = make_opener(jar)
    headers = {"Host": host} if host else {}
    status, page = fetch(opener, urllib.request.Request(base + "/login", headers=headers))
    if status != 200:
        print("status=%d" % status)
        return
    form = urllib.parse.urlencode(
        {"_csrf": csrf_value(page), "username": user, "password": password}
    ).encode()
    request = urllib.request.Request(
        base + "/login", data=form, headers=dict(headers, Origin=origin), method="POST"
    )
    status, _ = fetch(opener, request)
    print("status=%d" % status)
    show_cookies(jar)


def forged(base):
    jar = http.cookiejar.CookieJar()
    opener = make_opener(jar)
    headers = {
        "X-Forwarded-For": "203.0.113.9",
        "X-Forwarded-Proto": "https",
        "X-Forwarded-Host": "axis.example.com",
    }
    fetch(opener, urllib.request.Request(base + "/login", headers=headers))
    show_cookies(jar)
    request = urllib.request.Request(
        base + "/api/v1/worker/claim",
        data=b"{}",
        headers=dict(
            headers,
            Authorization="Bearer " + "b" * 43,
            **{"Content-Type": "application/json"}
        ),
        method="POST",
    )
    status, _ = fetch(opener, request)
    print("claim=%d" % status)


def main(argv):
    if len(argv) == 7 and argv[1] == "login":
        login(*argv[2:7])
    elif len(argv) == 3 and argv[1] == "forged":
        forged(argv[2])
    else:
        sys.stderr.write(__doc__ or "")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
