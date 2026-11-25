#!/bin/sh
# Boot probe, read-only: when does CEC come up? Started by boot.sh if
# /var/local/vibe/cecmon.on exists. Samples CEC and ARC model keys for
# DURATION seconds and keeps the kernel's CEC/ARC lines (the ring rotates).
# Log: /var/local/vibe/cecmon.log (rewritten each boot).
DURATION=${DURATION:-90}
BB=/usr/local/bin/busybox
V=/var/local/vibe
R=$V/vibe-run.sh
gi() { VIBE_BIN=vibe-model $R get-int "kilby.sl2.i32.$1" 2>/dev/null | $BB sed -n 's/.* = \(-*[0-9]*\) .*/\1/p'; }
up() { $BB cut -d' ' -f1 /proc/uptime; }
exec > $V/cecmon.log 2>&1
echo "cecmon at uptime $(up), debloat.off=$([ -f $V/debloat.off ] && echo yes || echo no)"
K=/tmp/cecmon.dmesg
: > $K
while :; do
    u=$(up); u=${u%.*}
    [ "$u" -ge "$DURATION" ] && break
    echo "up=$(up) func=$(gi cec.functionality) arcact=$(gi cec.arc.activemode) arcdev=$(gi sound.arcdev.exist) spk=$(gi sound.speaker.arc.mode) vol=$(gi sound.main.volume)"
    dmesg | grep -i -E "cec|arc mode|hpd|5v" >> $K
    sleep 1
done
echo "--- kernel CEC/ARC/HPD lines, first seen (sorted)"
/bin/sort -u $K | $BB head -200
rm -f $K
sync
