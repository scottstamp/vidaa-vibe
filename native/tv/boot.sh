#!/bin/sh
# vidaa-vibe boot supervisor. Started by the boot hook (/var/local/otaTest.sh)
# after SSH is up, as root, detached. Starts and keeps vibe-ui running.
#
# Switches, checked at start (and between restarts):
#   /var/local/vibe/off     do not start vibe-ui (create over SSH)
#   /var/local/vibe-off     same, in a place the browser bridge can write
#   /var/local/vibe/ui.args extra vibe-ui flags, one line (e.g. --gam), read
#                           before each start
#   /var/local/vibe-stock   put the stock UI back: restore the original
#                           appRunTimeInfoFile.json (runFlag 0) in place, do
#                           not start vibe-ui. Takes effect at the next boot.
#                           Bridge-writable, so it works without SSH.
# Log: /var/local/vibe/boot.log (rewritten each boot).
umask 022
BB=/usr/local/bin/busybox
V=/var/local/vibe
LOG=$V/boot.log
J=/var/local/appUpgrade/appRunTimeInfoFile.json
ORIG=$V/backup/appRunTimeInfoFile.json.orig
ORIG_MD5=d5338bffc0faf7dc8431d06032b683ec

exec > "$LOG" 2>&1
up() { $BB cut -d' ' -f1 /proc/uptime; }
echo "vibe boot.sh at uptime $(up)"

if [ -f /var/local/vibe-stock ]; then
    if [ -f "$ORIG" ] && [ "$($BB md5sum "$ORIG" | $BB cut -d' ' -f1)" = "$ORIG_MD5" ]; then
        cat "$ORIG" > "$J"
        sync
        echo "vibe-stock: restored the original runtime file ($($BB md5sum "$J")); stock UI at next boot"
    else
        echo "vibe-stock: backup missing or wrong md5, nothing restored"
    fi
    exit 0
fi
off() { [ -f $V/off ] || [ -f /var/local/vibe-off ]; }
if off; then echo "off switch present, not starting"; exit 0; fi

# First: stop the unused stock services and put the Hisense block in place,
# early, so they stop competing for the CPU while we start. Once per boot
# (restart-ui.sh runs this script again); the marker is on tmpfs. (Grepping
# every /proc/*/cmdline instead took 5.5 s.)
if [ -f $V/debloat.sh ] && [ ! -f /tmp/vibe-debloat.started ]; then
    : > /tmp/vibe-debloat.started
    $BB setsid /bin/sh $V/debloat.sh </dev/null >/dev/null 2>&1 &
    echo "debloat.sh started at uptime $(up)"
fi

# vibe-ui's own state (e.g. "a soundbar is normally connected"), owned by
# the UI user it runs as, never by root. Made once, not on every boot: early
# in a boot each extra command costs time (debloat.sh started 0.47 s after
# boot.sh with this step ahead of it, unmeasured how much was this step).
if [ ! -d $V/state ]; then
    mkdir -p $V/state && chown 311:311 $V/state && chmod 755 $V/state
fi

# optional read-only boot probe (CEC timing), only if switched on
if [ -f $V/cecmon.on ] && [ -f $V/cecmon.sh ]; then
    $BB setsid /bin/sh $V/cecmon.sh </dev/null >/dev/null 2>&1 &
fi

# Wait for the DirectFB world vibe-ui joins as a slave. Its shared-memory
# file appears about 6 s into boot, before the stock engine (8-10 s), which
# is what we used to wait for; vibe-ui takes focus back if the engine's
# window takes it when it starts after us. /var/local/vibe/wait-engine
# brings back the old wait for the engine.
# Only on the first start of a boot: a later run of this script
# (restart-ui.sh) finds the world, and the engine is gone by then.
if [ ! -f /tmp/vibe-ui.started ]; then
    i=0
    if [ -f $V/wait-engine ]; then
        while [ -z "$(pidof hisenseUI)" ] && [ $i -lt 180 ]; do sleep 1; i=$((i+1)); done
        echo "hisenseUI=[$(pidof hisenseUI)] at uptime $(up) (wait-engine)"
    else
        while [ ! -e /dev/shm/shm_tmp/fusion.0 ] && [ $i -lt 360 ]; do sleep 0.5; i=$((i+1)); done
        echo "DirectFB world $([ -e /dev/shm/shm_tmp/fusion.0 ] && echo up || echo MISSING) at uptime $(up), hisenseUI=[$(pidof hisenseUI)]"
    fi
    : > /tmp/vibe-ui.started
fi

# Never give up: without vibe-ui there is no UI at all (POWER and volume
# still work, they are the system's). Back off 3, 6, 9 ... s, at most 60 s;
# a run that lasted 10 minutes counts as healthy and resets the back-off.
# The previous run's log is kept as last-vibe-ui.prev.log (a crash's log
# would otherwise be overwritten by the restart).
n=0
runs=0
while :; do
    off && { echo "off switch seen, stopping"; exit 0; }
    n=$((n+1))
    runs=$((runs+1))
    [ -f $V/last-vibe-ui.log ] && mv -f $V/last-vibe-ui.log $V/last-vibe-ui.prev.log
    t0=$(up); t0=${t0%.*}
    echo "start vibe-ui (run $runs) at uptime $(up)"
    ARGS=$(cat $V/ui.args 2>/dev/null)
    echo "  args: $ARGS"
    VIBE_BIN=vibe-ui $V/vibe-run.sh --timeout=0 $ARGS >/dev/null 2>&1
    rc=$?
    t1=$(up); t1=${t1%.*}
    echo "vibe-ui exited rc=$rc at uptime $(up)"
    [ $((t1 - t0)) -ge 600 ] && n=1
    d=$((n * 3)); [ $d -gt 60 ] && d=60
    sleep $d
done
