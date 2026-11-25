#!/bin/sh
# Run on the TV as root (sh -s < install.sh), after the bundle files have
# been streamed to /var/local/vibe/slotb/. Two steps, each explicit:
#
#   SLOTB_STEP=stage  copy the blank bundle into /APPS/channelB/hisenseUI,
#                     owned UhisApp:UGhisApp like slot A. Changes nothing
#                     that runs: the flag still selects slot A.
#   SLOTB_STEP=flip   back up appRunTimeInfoFile.json, then set runFlag to
#                     "1" in place. Takes effect at the next boot.
#   SLOTB_STEP=unflip restore the backed-up file in place (the way back).
#
# Never touches slot A. See docs/findings/06-native-route.md, "Slot B".
set -u
BB=/usr/local/bin/busybox
SRC=/var/local/vibe/slotb
DST=/APPS/channelB/hisenseUI
J=/var/local/appUpgrade/appRunTimeInfoFile.json
BAK=/var/local/vibe/backup/appRunTimeInfoFile.json.orig
ORIG_MD5=d5338bffc0faf7dc8431d06032b683ec
umask 022

case "${SLOTB_STEP:-}" in
stage)
    [ -f $SRC/index.html ] && [ -f $SRC/versionInfo.json ] || { echo "missing $SRC files"; exit 1; }
    [ -d $DST ] || { echo "no $DST"; exit 1; }
    for f in index.html versionInfo.json process.env.json README; do
        [ -f $SRC/$f ] && cat $SRC/$f > $DST/$f
    done
    chown -R UhisApp:UGhisApp $DST
    chmod 751 $DST
    chmod 664 $DST/*
    sync
    ls -la $DST
    md5sum $DST/*
    ;;
flip)
    [ "$(md5sum $J | $BB cut -d' ' -f1)" = "$ORIG_MD5" ] || { echo "runtime file is not the recorded original; stop"; md5sum $J; exit 1; }
    [ -f $DST/index.html ] && grep -q 9000000000000 $DST/versionInfo.json || { echo "slot B not staged"; exit 1; }
    mkdir -p /var/local/vibe/backup
    [ -f $BAK ] || cat $J > $BAK
    [ "$(md5sum $BAK | $BB cut -d' ' -f1)" = "$ORIG_MD5" ] || { echo "backup md5 wrong; stop"; exit 1; }
    $BB sed 's/"runFlag": "0"/"runFlag": "1"/' $BAK > /var/local/vibe/backup/appRunTimeInfoFile.json.new
    grep -q '"runFlag": "1"' /var/local/vibe/backup/appRunTimeInfoFile.json.new || { echo "edit failed"; exit 1; }
    sync                                 # the backup must be on flash before the flip
    cat /var/local/vibe/backup/appRunTimeInfoFile.json.new > $J      # in place: owner/mode kept
    sync
    ls -la $J; cat $J; md5sum $J
    echo "rc.local would choose: $(grep -r runFlag $J | grep 1 >/dev/null && echo channelB || echo channelA)"
    ;;
unflip)
    [ -f $BAK ] || { echo "no backup at $BAK"; exit 1; }
    # never restore a backup that is not the recorded original (a backup
    # once came back empty after a mains power cut)
    [ "$(md5sum $BAK | $BB cut -d' ' -f1)" = "$ORIG_MD5" ] || { echo "backup md5 wrong; stop"; md5sum $BAK; exit 1; }
    cat $BAK > $J
    sync
    ls -la $J; md5sum $J
    echo "rc.local would choose: $(grep -r runFlag $J | grep 1 >/dev/null && echo channelB || echo channelA)"
    ;;
*)
    echo "set SLOTB_STEP=stage|flip|unflip"; exit 2 ;;
esac
