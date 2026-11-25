#!/bin/sh
# Record what the TV does around a POWER press (stock UI test). Read-only.
# Run detached:  busybox setsid sh /var/local/vibe/powermon.sh &
# One line every ~3 s for DURATION seconds to /var/local/vibe/powermon.log:
# wall clock, uptime (a jump between wall clock and uptime = suspend),
# screen state, standby mode, wake-up reason, pids of the UI pieces.
DURATION=${DURATION:-900}
BB=/usr/local/bin/busybox
LOG=/var/local/vibe/powermon.log
R=/var/local/vibe/vibe-run.sh
gi() { VIBE_BIN=vibe-model $R get-int "$1" 2>/dev/null | $BB sed -n 's/.* = \(-*[0-9]*\) .*/\1/p'; }
exec > $LOG 2>&1
echo "powermon start $(date) uptime $($BB cut -d' ' -f1 /proc/uptime)"
echo "--- dmesg PM lines at start"; dmesg | grep -i -E "PM:|suspend|resume|standby|wakeup" | $BB tail -n 10
t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt $DURATION ]; do
    echo "$(date +%s) up=$($BB cut -d' ' -f1 /proc/uptime) screen=$(gi kilby.sl2.i32.system.enum.screen.state) stbmode=$(gi kilby.sl2.i32.system.pwr.standby.mode) wake=$(gi kilby.sl2.i32.system.wakeup.reason) ui=[$(pidof hisenseUI)] apm=[$(pidof apmbond)] tvmain=[$(pidof tvmain)]"
    sync
    sleep 2
done
echo "--- dmesg PM lines at end"; dmesg | grep -i -E "PM:|suspend|resume|standby|wakeup" | $BB tail -n 30
echo "powermon end $(date)"
sync
