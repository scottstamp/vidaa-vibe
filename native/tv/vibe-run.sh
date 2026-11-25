#!/bin/sh
# On the TV, as root: run a vidaa-vibe binary with the environment the stock
# UI engine gets from app_man, as UhisUI's uid/gid/groups (measured from
# /proc/<hisenseUI>/status on 2026-11-25). The binary drops to that identity
# itself before it touches DirectFB or the model, so nothing it creates is
# root's.
#   /var/local/vibe/vibe-run.sh probe
#   VIBE_BIN=vibe-model /var/local/vibe/vibe-run.sh inputs
# Output goes to stdout and /var/local/vibe/last-<binary>.log: one log per
# binary, so a tool run never truncates the running UI's log.
umask 022
export LD_LIBRARY_PATH=/usr/local/lib/:/lib:/basic/lib:/vendor/lib:/vendor/lib/appipc:/vendor/lib/utopia:/vendor/lib/utopia/mi_ipc_client:/vendor/lib/directfb
export DIRECTFB_SESSION=0
export HOME=/
cd /
VIBE_USER=${VIBE_USER:-311:311:229,233,301,305,401,711,712,713,714,715,901,916,1022,3010,3013,3015,3016,3018}
BIN=${VIBE_BIN:-vibe}
case "$BIN" in vibe|vibe-model|vibe-ui|vibe-gam) ;; *) echo "bad VIBE_BIN" >&2; exit 2;; esac
LOG=/var/local/vibe/last-$BIN.log
/var/local/vibe/$BIN --user="$VIBE_USER" "$@" > $LOG 2>&1
rc=$?
cat $LOG
exit $rc
