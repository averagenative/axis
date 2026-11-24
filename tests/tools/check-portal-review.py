#!/usr/bin/env python3
# check-portal-review.py - the documents and the program agree on whether the
# request portal has been reviewed for the internet
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   check-portal-review.py [--root DIR]     check the tree (default: this one)
#   check-portal-review.py --self-test      check the checker
#
# (add-request-portal; the `deployment` specification, "The guides say
# whether the portal has been reviewed for the internet".)  The request
# portal is the one piece of Axis meant to face the internet, and no document
# may suggest that it can before an independent security review has passed.
# What says whether it has is one thing: the line axis-portal logs at start
# (clients/portal/main.c).
#
# While that line is there (before the review), every place that tells how to
# run the portal must say that it has not been reviewed for exposure to the
# internet and should be reached only from a network you trust: the
# deployment guide, the portal's part of each example, the user guide, the
# example configuration and the page "Request portal".  A place that is
# missing, or that tells how to run the portal without saying so, fails.
#
# Once the line is gone (after the review, slice P5), no place may still say
# "not reviewed", and the deployment guide must name the review.  P5 extends
# this part with what the review covered and what it left.
#
# No network and no build: it reads tracked files only.

import argparse
import os
import re
import sys

# The line at start.
PROGRAM = "clients/portal/main.c"

# Every place that tells how to run the portal.
PLACES = [
    "deploy/README.md",
    "deploy/examples/traefik/README.md",
    "deploy/examples/traefik/compose.portal.yaml",
    "deploy/examples/kubernetes/README.md",
    "deploy/examples/kubernetes/portal.yaml",
    "deploy/examples/quadlet/README.md",
    "deploy/examples/quadlet/axis-portal.container",
    "docs/20-let-other-people-ask-for-titles.md",
    "data/axis-portal.conf.example",
    "src/web/axis-web-portal.c",
]

NOT_REVIEWED = re.compile(r"not (been )?reviewed for (exposure to )?the internet", re.IGNORECASE)
TRUSTED_ONLY = re.compile(r"only from a network you trust", re.IGNORECASE)
REVIEW_NAMED = re.compile(r"^#+ .*security review", re.IGNORECASE | re.MULTILINE)


def plain(text):
    """The text as a reader reads it: comment marks, Markdown emphasis and
    the joins of a C string that continues on the next line taken out, and
    every run of white space one space."""
    text = re.sub(r'"\s*\n\s*"', "", text)
    text = re.sub(r"^\s*(#|//|\*|>)+ ?", "", text, flags=re.MULTILINE)
    text = text.replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", text)


def read(root, path):
    try:
        with open(os.path.join(root, path), encoding="utf-8") as handle:
            return handle.read()
    except OSError:
        return None


def check(root):
    """The list of what is wrong (empty: nothing)."""
    wrong = []
    program = read(root, PROGRAM)
    if program is None:
        return ["%s is missing: it holds the line that says whether the portal has been reviewed" % PROGRAM]
    before = NOT_REVIEWED.search(plain(program)) is not None

    for path in PLACES:
        raw = read(root, path)
        if raw is None:
            wrong.append("%s is missing: it is one of the places that tell how to run the portal" % path)
            continue
        text = plain(raw)
        says = NOT_REVIEWED.search(text) is not None
        if before and not says:
            wrong.append(
                "%s tells how to run the portal and does not say that it has not been reviewed for "
                "exposure to the internet" % path
            )
        elif before and not TRUSTED_ONLY.search(text):
            wrong.append("%s does not say that the portal should be reached only from a network you trust" % path)
        elif not before and says:
            wrong.append(
                "%s still says that the portal is not reviewed, and axis-portal no longer says so at "
                "start" % path
            )
    if not before:
        guide = read(root, "deploy/README.md") or ""
        if not REVIEW_NAMED.search(guide):
            wrong.append(
                "deploy/README.md does not name the security review (a heading with \"security review\"), "
                "and axis-portal no longer says at start that it is not reviewed"
            )
    return wrong


def self_test():
    import tempfile

    said = (
        "The request portal has not been reviewed for exposure to the\n"
        "# internet. Let it be reached only from a network you\n# trust.\n"
    )
    line = 'g_warning("axis-portal has not been reviewed for exposure to the internet. Do not "\n\t"put it there.");\n'

    def tree(program, places, guide_extra=""):
        root = tempfile.mkdtemp(prefix="check-portal-review.")
        for path, body in [(PROGRAM, program)] + [(p, places) for p in PLACES]:
            full = os.path.join(root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as handle:
                handle.write(body + (guide_extra if path == "deploy/README.md" else ""))
        return root

    def remove(root):
        import shutil

        shutil.rmtree(root)

    failures = []

    def expect(name, root, count):
        got = check(root)
        if len(got) != count:
            failures.append("%s: expected %d complaints, got %d: %s" % (name, count, len(got), got))
        remove(root)

    # Before the review: the line is there, every place says so.
    expect("before, all say so", tree(line, said), 0)
    # One place that does not say it.
    root = tree(line, said)
    with open(os.path.join(root, PLACES[2]), "w", encoding="utf-8") as handle:
        handle.write("How to run the portal.\n")
    expect("before, one silent", root, 1)
    # One place that says it without "a network you trust".
    root = tree(line, said)
    with open(os.path.join(root, PLACES[0]), "w", encoding="utf-8") as handle:
        handle.write("The portal has not been reviewed for exposure to the internet.\n")
    expect("before, one without the network", root, 1)
    # One place missing.
    root = tree(line, said)
    os.remove(os.path.join(root, PLACES[4]))
    expect("before, one missing", root, 1)
    # After the review: the line is gone and the places still say it.
    expect("after, still said", tree("int main(void) { return 0; }\n", said), len(PLACES) + 1)
    # After the review, as it should be.
    expect(
        "after, review named",
        tree("int main(void) { return 0; }\n", "The portal was reviewed.\n", "\n### The security review\n"),
        0,
    )
    # The program is missing.
    root = tree(line, said)
    os.remove(os.path.join(root, PROGRAM))
    expect("no program", root, 1)

    for failure in failures:
        sys.stderr.write("check-portal-review: self-test: %s\n" % failure)
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    wrong = check(args.root)
    for line in wrong:
        sys.stderr.write("check-portal-review: %s\n" % line)
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
