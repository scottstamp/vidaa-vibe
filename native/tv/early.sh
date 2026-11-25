#!/bin/sh
# vidaa-vibe early: keep the unused stock services from starting at all.
# app_man reads /basic/am_config once, at about 6.5 s of uptime (after it
# joins the DirectFB world). This script is started by the boot hook's first
# stage (about 5.3 s), as root, in the background, and bind-mounts a trimmed
# copy over it first (see early-prepare.sh, docs/findings/06-native-route.md).
#
# Acts only when armed:
#   /var/local/vibe/early.arm   this boot only (deleted after mounting)
#   /var/local/vibe/early.on    every boot
# and never with an off switch present (vibe/off, /var/local/vibe-off,
# /var/local/vibe-stock, vibe/debloat.off) or when either md5 in
# am_config.md5 does not match (e.g. after a firmware update).
#
# Crash guard: after mounting it writes early.pending; debloat.sh deletes it
# once vibe-ui runs and hisenseUI has appeared (app_man parsed the copy). A
# boot that never gets there leaves the marker, and the next boot skips the
# overlay (and deletes the marker): a bad overlay costs one boot, no SSH
# needed. Nothing is written to a verified partition; the mount ends at reboot.
# Log: /var/local/vibe/early.log (rewritten each boot).
V=/var/local/vibe
BB=/usr/local/bin/busybox
exec > $V/early.log 2>&1
echo "early.sh at uptime $(cat /proc/uptime)"

arm=""
[ -f $V/early.arm ] && arm=once
[ -f $V/early.on ] && arm=on
[ -n "$arm" ] || { echo "not armed"; exit 0; }
for f in $V/off /var/local/vibe-off /var/local/vibe-stock $V/debloat.off; do
    [ -f $f ] && { echo "$f present: not overlaying"; exit 0; }
done
if [ -f $V/early.pending ]; then
    echo "early.pending: the last overlaid boot never confirmed; skipping this boot"
    rm -f $V/early.pending
    sync
    exit 0
fi

md5of() { set -- $($BB md5sum "$1" 2>/dev/null); echo "${1:-}"; }
set -- $(cat $V/am_config.md5 2>/dev/null)
want_src=${1:-}
want_trim=${2:-}
if [ -z "$want_src" ] || [ "$(md5of /basic/am_config)" != "$want_src" ]; then
    echo "/basic/am_config is not the one the copy was made from: not overlaying"
    exit 0
fi
if [ -z "$want_trim" ] || [ "$(md5of $V/am_config.trim)" != "$want_trim" ]; then
    echo "trimmed copy missing or changed: not overlaying"
    exit 0
fi
if grep -q " /basic/am_config " /proc/mounts; then
    echo "already overlaid"
    exit 0
fi
[ -e /tmp/fifo_am_read ] && echo "note: app_man's fifo exists already, it may have read the config"

# the mount first: it has to beat app_man; the bookkeeping (and its sync,
# which can take a while this early) comes after
if /bin/mount --bind $V/am_config.trim /basic/am_config; then
    echo "overlaid at uptime $(cat /proc/uptime) ($arm)"
else
    echo "mount failed rc=$? at uptime $(cat /proc/uptime)"
    exit 0
fi
[ -e /tmp/fifo_am_read ] && echo "note: app_man's fifo present right after the mount"
: > $V/early.pending
[ "$arm" = once ] && rm -f $V/early.arm
sync
echo "pending written, armed=$arm, at uptime $(cat /proc/uptime)"
