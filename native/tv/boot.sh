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

# wait for app_man to have the stock engine up: the DirectFB world exists
# then. No settle delay: vibe-ui takes focus back if the engine's window
# takes it.
# Only on the first start of a boot: debloat.sh stops the engine once vibe-ui
# runs, so a later run of this script (restart-ui.sh) must not wait for it.
if [ ! -f /tmp/vibe-ui.started ]; then
    i=0
    while [ -z "$(pidof hisenseUI)" ] && [ $i -lt 180 ]; do sleep 1; i=$((i+1)); done
    echo "hisenseUI=[$(pidof hisenseUI)] at uptime $(up)"
    : > /tmp/vibe-ui.started
fi

n=0
while [ $n -lt 10 ]; do
    off && { echo "off switch seen, stopping"; exit 0; }
    n=$((n+1))
    echo "start vibe-ui (run $n) at uptime $(up)"
    ARGS=$(cat $V/ui.args 2>/dev/null)
    echo "  args: $ARGS"
    VIBE_BIN=vibe-ui $V/vibe-run.sh --timeout=0 $ARGS >/dev/null 2>&1
    echo "vibe-ui exited rc=$? at uptime $(up)"
    sleep $((n * 3))
done
echo "gave up after $n runs"
