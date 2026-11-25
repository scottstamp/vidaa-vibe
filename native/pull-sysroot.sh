#!/bin/sh
# Copy the TV libraries native/ links against into native/.deps/sysroot
# (gitignored: vendor binaries). Read-only on the TV. Run from Windows
# (Git Bash), since WSL here cannot reach the TV:
#   TV_HOST=192.168.2.152 native/pull-sysroot.sh
set -eu
cd "$(dirname "$0")"
KEYS=../server/keys
TV=${TV_HOST:?set TV_HOST}
mkdir -p .deps/sysroot
ssh -o UserKnownHostsFile=$KEYS/tv_known_hosts -p 2222 -i $KEYS/tv_root_ed25519 root@"$TV" \
    'cd / && tar -cf - vendor/lib/directfb/libdirect-1.4.so.0.2.0 vendor/lib/directfb/libdirectfb-1.4.so.0.2.0 vendor/lib/directfb/libfusion-1.4.so.0.2.0' \
    | tar -xf - -C .deps/sysroot
ls -la .deps/sysroot/vendor/lib/directfb
