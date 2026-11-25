#!/bin/sh
# PC side: open vibe-ui screens through its dev hook (needs /var/local/vibe/dev
# on the TV) and save screenshots as PNG under research/shots/.
#   sh native/tools/tvshots.sh "home 0xf005" "picture 0xf062 0xf012"
# Each spec: name, then remote keys (hex DirectFB symbols) to press first.
S="ssh -o UserKnownHostsFile=server/keys/tv_known_hosts -p 2222 -i server/keys/tv_root_ed25519 root@${TV_HOST:-192.168.2.152}"
shot() { # name, keys...
  name=$1; shift
  cmds=""; for k in "$@"; do cmds="${cmds}key $k\n"; done
  timeout 30 $S "printf '${cmds}' > /tmp/vibe-cmd; chmod 666 /tmp/vibe-cmd; sleep 1.5; printf 'shot\n' > /tmp/vibe-cmd; sleep 1.5; cat /tmp/vibe-shot.raw; rm -f /tmp/vibe-shot.raw" > research/shots/$name.raw 2>/dev/null
  python native/tools/shot2png.py research/shots/$name.raw research/shots/$name.png 0.5 >/dev/null
  echo "$name done"
}
for spec in "$@"; do eval "shot $spec"; done
