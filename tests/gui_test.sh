#!/bin/sh
# Run tests/test_gui headless on a private broadway display, against the
# throwaway sshd of sftp_test.sh when sshd is installed.
#   usage: tests/gui_test.sh <test-binary>
set -eu
BIN=$1
BW=$(command -v gtk4-broadwayd || true)
if [ -z "$BW" ]; then echo "gui tests skipped: gtk4-broadwayd not installed"; exit 0; fi
D=$((40 + $$ % 50))
"$BW" ":$D" >/dev/null 2>&1 &
BWPID=$!
trap 'kill $BWPID 2>/dev/null || true' EXIT INT TERM
sleep 0.5
export GDK_BACKEND=broadway BROADWAY_DISPLAY=":$D"
SSHD=$(command -v sshd || echo /usr/sbin/sshd)
if [ -x "$SSHD" ]; then
    "$(dirname "$0")/sftp_test.sh" "$BIN"
else
    "$BIN"
fi
