#!/bin/sh
# vidaa-vibe boot supervisor. Started by the boot hook (/var/local/otaTest.sh)
# after SSH is up, as root, detached. Starts and keeps vibe-ui running.
#
# Switches, checked at start (and between restarts):
#   /var/local/vibe/off     do not start vibe-ui (create over SSH)
#   /var/local/vibe-off     same, in a place the browser bridge can write
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

# wait for app_man to have the stock engine up: the DirectFB world exists then
i=0
while [ -z "$(pidof hisenseUI)" ] && [ $i -lt 90 ]; do sleep 2; i=$((i+1)); done
echo "hisenseUI=[$(pidof hisenseUI)] at uptime $(up)"
sleep 5

n=0
while [ $n -lt 10 ]; do
    off && { echo "off switch seen, stopping"; exit 0; }
    n=$((n+1))
    echo "start vibe-ui (run $n) at uptime $(up)"
    VIBE_BIN=vibe-ui $V/vibe-run.sh --timeout=0 >/dev/null 2>&1
    echo "vibe-ui exited rc=$? at uptime $(up)"
    sleep $((n * 3))
done
echo "gave up after $n runs"
