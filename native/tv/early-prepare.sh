#!/bin/sh
# vidaa-vibe: build the trimmed app_man config that early.sh overlays.
# Run on the TV as root once, and again after a firmware update:
#   sh /var/local/vibe/early-prepare.sh
# Reads /basic/am_config (read-only, verified), writes
# /var/local/vibe/am_config.trim (power_on_start = false for the sections in
# SECTIONS, nothing else changed) and /var/local/vibe/am_config.md5
# ("<md5 of the original> <md5 of the trimmed copy>"). Starts nothing.
set -u
BB=/usr/local/bin/busybox
V=/var/local/vibe
SRC=/basic/am_config
OUT=$V/am_config.trim
# app_man's own boot starts we do not want (each is also stopped by
# debloat.sh if it runs anyway). Keep apm_bond, hisenseUI, LauncherService.
# (websdk stays: the browser bridge, the hook's disarm of last resort on a
# stock boot; debloat.sh stops it later on vibe boots. TTSPlayer and
# dialserver seem to be started by app_man regardless, and resmon and
# HiAvsService have no binary on this unit: listed anyway, harmless.)
SECTIONS="AirPlayBridge ota_upgrade hi_logreport_service resmon app_upgrade
HiAvsService mosquitto dialserver ai_manager vidaa_ai_service
TTSPlayer netflix masman mdharvester reggieserver"
EXPECT=15       # one power_on_start = true line in each of them

# a new copy has not been tested: a permanent switch must not pick it up
# unsupervised; test with early.arm first, then re-arm early.on
if [ -f $V/early.on ]; then
    mv -f $V/early.on $V/early.on.untested
    echo "early.on moved to early.on.untested: test the new copy with early.arm first"
fi

md5of() { set -- $($BB md5sum "$1" 2>/dev/null); echo "${1:-}"; }

# never build from the overlay itself
if grep -q " /basic/am_config " /proc/mounts; then
    echo "/basic/am_config is overlaid now; reboot without early.on/early.arm first"
    exit 1
fi

# section names may be written "[ name ]"; only "power_on_start = true" lines
# inside the listed sections change (factory_power_on_start does not match)
$BB awk -v list=" $(echo $SECTIONS) " '
    /^\[/ {
        name = $0
        sub(/^\[[ \t]*/, "", name)
        sub(/[ \t]*\][ \t]*$/, "", name)
        on = index(list, " " name " ") > 0
    }
    on && $0 == "power_on_start = true" { $0 = "power_on_start = false" }
    { print }
' $SRC > $OUT.new || { echo "awk failed"; exit 1; }

lines_src=$($BB wc -l < $SRC)
lines_new=$($BB wc -l < $OUT.new)
changed=$($BB awk 'NR == FNR { a[FNR] = $0; next } a[FNR] != $0 { c++ } END { print c + 0 }' $SRC $OUT.new)
echo "lines: $lines_src -> $lines_new, changed: $changed (expected $EXPECT)"
if [ "$lines_src" != "$lines_new" ] || [ "$changed" != "$EXPECT" ]; then
    echo "unexpected result; nothing installed"
    rm -f $OUT.new
    exit 1
fi
$BB diff $SRC $OUT.new

chmod 444 $OUT.new
mv -f $OUT.new $OUT
echo "$(md5of $SRC) $(md5of $OUT)" > $V/am_config.md5.new
mv -f $V/am_config.md5.new $V/am_config.md5
sync
echo "installed:"; ls -la $OUT $V/am_config.md5; cat $V/am_config.md5
