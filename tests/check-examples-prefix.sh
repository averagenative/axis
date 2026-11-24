#!/bin/bash
# check-examples-prefix.sh - check-examples.sh refuses a prefix that could be
# the name of something real, before it lists or removes anything
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-examples-prefix.sh          (or: make check-examples-prefix)
#
# tests/check-examples.sh removes every container, pod, network, volume and
# secret named <prefix>-*, and AXIS_XTEST_PREFIX sets the prefix.  With "axis"
# that would be a real installation's own (axis-portal, axis-proxy, axis-db).
# This runs the script with prefixes it must refuse and expects, for each:
# exit status 2, a sentence that says why, and that no container tool and no
# systemctl was called at all.
#
# Nothing real can be touched, whatever the script under test does: podman,
# docker and systemctl are stand-ins that only write down that they were
# called, and the runtime directory and the home directory are an empty
# scratch directory.  Needs nothing but bash.

set -u

cd "$(dirname "$0")/.." || exit 1

scratch=$(mktemp -d) || exit 1
trap 'rm -rf "$scratch"' EXIT

mkdir "$scratch/bin" "$scratch/home" "$scratch/run"
for tool in podman docker systemctl; do
	printf '#!/bin/sh\necho "%s $*" >> "%s/called"\nexit 1\n' "$tool" "$scratch" > "$scratch/bin/$tool"
	chmod +x "$scratch/bin/$tool"
done

failed=0
checked=0

# refused PREFIX: the script must refuse it.
refused() {
	local out status

	rm -f "$scratch/called"
	out=$(AXIS_XTEST_PREFIX=$1 PATH="$scratch/bin:$PATH" HOME="$scratch/home" XDG_RUNTIME_DIR="$scratch/run" \
		TMPDIR="$scratch" bash tests/check-examples.sh worker 2>&1)
	status=$?
	checked=$((checked + 1))
	if [ "$status" != 2 ]; then
		echo "  FAIL     the prefix \"$1\" was not refused (exit status $status)" >&2
		failed=$((failed + 1))
	elif [ -e "$scratch/called" ]; then
		echo "  FAIL     with the prefix \"$1\" a tool was called before the refusal: $(head -n 1 "$scratch/called")" >&2
		failed=$((failed + 1))
	elif [[ $out != *"AXIS_XTEST_PREFIX must be axis-xtest or axis-p4x"*"Nothing was listed and nothing was removed."* ]]; then
		echo "  FAIL     the refusal of \"$1\" does not say why: $out" >&2
		failed=$((failed + 1))
	fi
}

# What a real installation is called, what is not a test's prefix, and what
# would say more than a name.
for prefix in axis axis- axis-portal axis-proxy axis-db systemd-axis pod_axis AXIS-XTEST axis-p4 axis-xtes \
	xaxis-xtest axis-xtest- axis-xtest-two axis-xtest_two "axis-xtest two" 'axis-xtest*' 'axis-xtest/..' \
	axis-xtestabcdefghijklm a '*' '?' ' ' 'axis-xtest
axis'; do
	refused "$prefix"
done

if [ "$failed" != 0 ]; then
	echo "check-examples-prefix: $failed of $checked prefixes were not refused as they must be" >&2
	exit 1
fi
echo "  ok       check-examples.sh refuses $checked prefixes that are not a test's, before it calls any tool"
