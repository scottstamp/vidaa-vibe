#!/bin/sh
# vidaa-vibe early: keep the unused stock services from starting at all.
# app_man reads /basic/am_config once, at about 6.5 s of uptime (after it
# joins the DirectFB world). This script is started by the boot hook's first
# stage (about 5.3 s), as root, in the background, and bind-mounts a trimmed
# copy over it first (see early-prepare.sh, docs/findings/06-native-route.md).
#
# Acts only when armed:
#   /var/local/vibe/early.arm   this boot only (always deleted before mounting)
#   /var/local/vibe/early.on    every boot
# and never with an off switch present (vibe/off, /var/local/vibe-off,
# /var/local/vibe-stock, vibe/debloat.off), when boot.sh, debloat.sh or
# vibe-ui are missing (nothing could confirm the overlay), or when either
# md5 in am_config.md5 does not match (e.g. after a firmware update).
#
# Crash guard: before mounting it writes early.pending (fsync'ed);
# debloat.sh deletes it once vibe-ui runs and hisenseUI has appeared while
# the overlay is mounted (app_man parsed the copy). If any boot finds the
# marker, the last overlaid boot never confirmed: the overlay is disarmed
# (early.on renamed early.on.failed, early.arm deleted), the event goes to
# early.history, and nothing is mounted. Re-arming is a deliberate step. So
# a bad overlay costs one boot, with either switch, and needs no SSH.
# Nothing is written to a verified partition; the mount ends at reboot.
# Logs: early.log (this boot), early.history (kept).
V=/var/local/vibe
BB=/usr/local/bin/busybox
exec > $V/early.log 2>&1
echo "early.sh at uptime $(cat /proc/uptime)"

# first, armed or not: did the last overlaid boot fail?
if [ -f $V/early.pending ]; then
    echo "early.pending: the last overlaid boot never confirmed: disarming, not mounting"
    [ -f $V/early.on ] && mv -f $V/early.on $V/early.on.failed && echo "early.on -> early.on.failed"
    rm -f $V/early.arm $V/early.pending
    echo "$(date) uptime $(cat /proc/uptime): last overlaid boot unconfirmed; disarmed" >> $V/early.history
    sync
    exit 0
fi

arm=""
[ -f $V/early.arm ] && arm=once
[ -f $V/early.on ] && arm=on
[ -n "$arm" ] || { echo "not armed"; exit 0; }
for f in $V/off /var/local/vibe-off /var/local/vibe-stock $V/debloat.off; do
    [ -f $f ] && { echo "$f present: not overlaying"; exit 0; }
done
for f in $V/boot.sh $V/debloat.sh $V/vibe-ui; do
    [ -f $f ] || { echo "$f missing (nothing could confirm): not overlaying"; exit 0; }
done

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

# The marker (and the one-boot switch's removal) must be on disk before the
# overlay can matter: app_man reboots without a sync if apm_bond stops.
# fsync of the file and the directory takes ~10 ms (a global sync this early
# could take long and lose the race).
if ! : > $V/early.pending; then
    echo "cannot write early.pending: not overlaying"
    exit 0
fi
rm -f $V/early.arm
$BB fsync $V/early.pending $V || { echo "fsync failed: not overlaying"; rm -f $V/early.pending; exit 0; }
echo "pending on disk at uptime $(cat /proc/uptime) (armed: $arm)"

if /bin/mount --bind $V/am_config.trim /basic/am_config; then
    echo "overlaid at uptime $(cat /proc/uptime)"
    echo "$(date) uptime $(cat /proc/uptime): overlaid ($arm)" >> $V/early.history
else
    echo "mount failed rc=$? at uptime $(cat /proc/uptime)"
    rm -f $V/early.pending
    $BB fsync $V
    exit 0
fi
[ -e /tmp/fifo_am_read ] && echo "note: app_man's fifo present right after the mount"
exit 0
