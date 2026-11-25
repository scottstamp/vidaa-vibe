#!/bin/sh
# Copy out/vibe, out/vibe-model and tv/vibe-run.sh to /var/local/vibe on the TV. Starts
# nothing. Run from Windows (Git Bash):
#   TV_HOST=192.168.2.152 native/deploy.sh
set -eu
cd "$(dirname "$0")"
KEYS=../server/keys
TV=${TV_HOST:?set TV_HOST}
SSH="ssh -o UserKnownHostsFile=$KEYS/tv_known_hosts -p 2222 -i $KEYS/tv_root_ed25519 root@$TV"
$SSH 'mkdir -p /var/local/vibe && chmod 755 /var/local/vibe'
# stream through ssh (no scp dependency); write to .new then rename
for b in vibe vibe-model vibe-ui vibe-gam; do
    $SSH "cat > /var/local/vibe/$b.new && chmod 755 /var/local/vibe/$b.new && mv /var/local/vibe/$b.new /var/local/vibe/$b" < out/$b
done
# scripts may be running (boot.sh, vibe-run.sh): bash reads them as it goes,
# so never rewrite one in place; write a new file and rename it over
for f in vibe-run.sh boot.sh hook-patch.sh powermon.sh restart-ui.sh debloat.sh cecmon.sh early.sh early-prepare.sh; do
    $SSH "cat > /var/local/vibe/$f.new && chmod 755 /var/local/vibe/$f.new && mv /var/local/vibe/$f.new /var/local/vibe/$f" < tv/$f
done
$SSH 'sync'
$SSH 'ls -la /var/local/vibe; md5sum /var/local/vibe/vibe /var/local/vibe/vibe-model /var/local/vibe/vibe-ui'
md5sum out/vibe out/vibe-model out/vibe-ui
