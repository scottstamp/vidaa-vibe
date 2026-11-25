#!/bin/sh
# On the TV, as root: run /var/local/vibe/vibe with the environment the
# stock UI engine gets from app_man, as UhisUI's uid/gid/groups (measured
# from /proc/<hisenseUI>/status on 2026-11-25). vibe drops to that identity
# itself before it touches DirectFB, so nothing it creates is root's.
#   /var/local/vibe/vibe-run.sh probe
#   /var/local/vibe/vibe-run.sh --timeout=20 hello
#   VIBE_BIN=vibe-model /var/local/vibe/vibe-run.sh inputs
# Output goes to stdout and /var/local/vibe/last.log.
umask 022
export LD_LIBRARY_PATH=/usr/local/lib/:/lib:/basic/lib:/vendor/lib:/vendor/lib/appipc:/vendor/lib/utopia:/vendor/lib/utopia/mi_ipc_client:/vendor/lib/directfb
export DIRECTFB_SESSION=0
export HOME=/
cd /
VIBE_USER=${VIBE_USER:-311:311:229,233,301,305,401,711,712,713,714,715,901,916,1022,3010,3013,3015,3016,3018}
BIN=${VIBE_BIN:-vibe}
case "$BIN" in vibe|vibe-model|vibe-ui|vibe-gam) ;; *) echo "bad VIBE_BIN" >&2; exit 2;; esac
/var/local/vibe/$BIN --user="$VIBE_USER" "$@" > /var/local/vibe/last.log 2>&1
rc=$?
cat /var/local/vibe/last.log
exit $rc
