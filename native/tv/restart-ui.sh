#!/bin/sh
# Restart the vibe-ui supervisor (boot.sh) so it runs the current script and
# flags. Run on the TV as root: sh /var/local/vibe/restart-ui.sh
# Stops the running boot.sh and vibe-ui, then starts boot.sh again detached.
BB=/usr/local/bin/busybox
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ "$pid" = "$$" ] && continue
    if grep -q "/var/local/vibe/boot.sh" $p/cmdline 2>/dev/null; then
        echo "stop boot.sh $pid"; kill $pid 2>/dev/null
    fi
done
P=$(pidof vibe-ui); [ -n "$P" ] && { echo "stop vibe-ui $P"; kill -TERM $P; }
sleep 3
$BB setsid /bin/sh /var/local/vibe/boot.sh </dev/null >/dev/null 2>&1 &
sleep 8
echo "vibe-ui=[$(pidof vibe-ui)]"
cat /var/local/vibe/boot.log
