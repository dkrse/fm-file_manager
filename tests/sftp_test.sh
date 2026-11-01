#!/bin/sh
# Run tests/test_sftp against a throwaway sshd on 127.0.0.1.
# Everything (host key, client key, authorized_keys, files) lives in a
# temp dir – ~/.ssh is never touched.  Skips (exit 0) without sshd.
#   usage: tests/sftp_test.sh <test-binary>
set -eu
BIN=$1
SSHD=$(command -v sshd || echo /usr/sbin/sshd)
if [ ! -x "$SSHD" ] || ! command -v ssh-keygen >/dev/null; then
    echo "sftp tests skipped: sshd / ssh-keygen not installed"
    exit 0
fi

T=$(mktemp -d)
PORT=$((20000 + $$ % 20000))
cleanup() {
    [ -f "$T/sshd.pid" ] && kill "$(cat "$T/sshd.pid")" 2>/dev/null || true
    chmod -R u+rwX "$T" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

ssh-keygen -q -t ed25519 -N '' -f "$T/hostkey"
ssh-keygen -q -t ed25519 -N '' -f "$T/clientkey"
cp "$T/clientkey.pub" "$T/authorized_keys"
chmod 600 "$T/authorized_keys"
mkdir "$T/work"
cat > "$T/sshd_config" <<CFG
Port $PORT
ListenAddress 127.0.0.1
HostKey $T/hostkey
AuthorizedKeysFile $T/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $T/sshd.pid
Subsystem sftp internal-sftp
CFG

"$SSHD" -f "$T/sshd_config" -E "$T/sshd.log"
i=0
while [ ! -f "$T/sshd.pid" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if [ ! -f "$T/sshd.pid" ]; then
    echo "sftp tests: sshd did not start"; cat "$T/sshd.log"; exit 1
fi

FM_SFTP_PORT=$PORT FM_SFTP_KEY="$T/clientkey" FM_SFTP_HOSTKEY="$T/hostkey.pub" FM_SFTP_DIR="$T/work" "$BIN"
