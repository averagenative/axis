#!/bin/sh
# fake-tool.sh - stands in for ffprobe in tests/test-probe-run.c and
# tests/test-probe-job.c: the first argument says what it does.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# It is run as `/bin/sh fake-tool.sh MODE [ARG]`, so that it needs no
# execute permission.

mode=$1

case "$mode" in
echo)
	echo "out line"
	echo "err line" >&2
	exit 0
	;;
exit3)
	echo partial
	echo "something broke" >&2
	exit 3
	;;
segv)
	kill -SEGV $$
	sleep 5
	;;
abrt)
	# Dies of a signal as a crashed encoder does (ABRT, KILL from the
	# kernel's OOM killer), or one stopped by its own limits (XFSZ, XCPU).
	kill -ABRT $$
	sleep 5
	;;
kill)
	kill -KILL $$
	sleep 5
	;;
xfsz)
	kill -XFSZ $$
	sleep 5
	;;
xcpu)
	kill -XCPU $$
	sleep 5
	;;
fd3)
	# What the probe gives it as descriptor 3.
	cat <&3
	;;
stdin)
	# Standard input must be /dev/null: this returns at once.
	cat
	echo "stdin was empty"
	;;
env)
	env
	;;
fdopen)
	# Is descriptor $2 open here?
	if [ -e "/proc/$$/fd/$2" ]; then echo open; else echo closed; fi
	;;
limits)
	cat /proc/$$/limits
	;;
stat)
	cat /proc/$$/stat
	echo "pid=$$"
	;;
hang)
	# Never finishes; has a child that does not either.  Prints the
	# child's pid on the first line and its own on the second (it may not
	# write a file: its file size limit is 0).
	sleep 600 &
	echo $!
	echo $$
	wait
	;;
flood)
	while :; do
		echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
	done
	;;
flood-err)
	while :; do
		echo eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee >&2
	done
	;;
daemonize)
	# Closes its output and goes on forever.
	exec >&- 2>&-
	sleep 600
	;;
leave-child)
	# Exits at once, leaving a child that keeps the output open; prints
	# the child's pid.
	sleep 600 &
	echo $!
	exit 0
	;;
big)
	i=0
	while [ $i -lt 30000 ]; do
		echo "line $i of an output that is long but not too long"
		i=$((i + 1))
	done
	;;
garbage)
	# Output that is not JSON at all.
	echo "this is not json {{{"
	exit 0
	;;
json-empty)
	echo "{}"
	exit 0
	;;
deep)
	# 40 levels of nesting, then a lot of members.
	i=0
	s=""
	while [ $i -lt 40 ]; do s="$s["; i=$((i + 1)); done
	echo "$s"
	exit 0
	;;
sleep1)
	# Takes a second, then prints a report with no container.
	sleep 1
	echo "{}"
	exit 0
	;;
sleep2)
	sleep 2
	echo "{}"
	exit 0
	;;
evil-error)
	# Fails with an error text full of things that must not reach a page
	# or a log line as they are.
	printf '[matroska,webm @ 0x55d0c0b1] first line\n\033[31mboom\033[0m \342\200\256<script>alert(1)</script>\a\r\n' >&2
	exit 1
	;;
*)
	echo "unknown mode" >&2
	exit 99
	;;
esac
