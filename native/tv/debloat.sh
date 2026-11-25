#!/bin/sh
# vidaa-vibe: stop the stock services this TV does not use, and keep the TV
# off Hisense's servers. Started by boot.sh as root, detached. Nothing here
# persists: a reboot without this script brings every service and the stock
# firewall back.
#
# Switch: /var/local/vibe/debloat.off   do nothing (checked at start and on
#         every pass; when seen, removes our firewall chain and exits)
#
# What it does:
#   1. stops the processes in STOP (by /proc/<pid>/status Name, which is the
#      first 15 characters of the program name), wrapper scripts first so
#      they cannot restart their child. app_man does not relaunch them
#      (measured with TTSPlayer). Passes repeat every 2 s up to 40 s of
#      uptime, every 5 s up to 2 minutes (some start late: the browser at
#      about 11 s, QDA at about 28 s), then every 60 s (in case something
#      starts them again, e.g. after standby).
#      app_man respawns some at once; those are frozen (SIGSTOP) the second
#      time they are seen, so they stay alive, unrespawned, using no CPU.
#   1b. once vibe-ui runs, stops the stock UI's own pieces (STOP_UI:
#      app_man, apmbond, hisenseUI, LauncherService); after that, anything
#      frozen is killed, since nothing respawns any more.
#   2. iptables chain VIBE_OUT in the nat table, first in nat OUTPUT:
#      - sends DNS queries (udp and tcp 53) for any name under the labels in
#        BLOCK_LABELS, matched on the DNS wire format (length byte + label),
#        to local port 9, where nothing listens, so they fail at once
#        ("connection refused"); every subdomain is covered, including names
#        the services build at run time;
#      - does the same with all non-loopback tcp/udp from the uids in
#        BLOCK_UIDS (the Hisense updaters and reporters), in case they use
#        fixed addresses.
#      Everything else, third-party services included, is untouched.
#      Why nat: the vendor's firewall scripts (security-firewall-start.sh,
#      firewall-eulaon.sh, run by start-security.sh and tvmain) flush the
#      filter table, and once did so 6 s after we had set our rules there.
#      They never touch nat. The chain is still checked on every pass.
#
# To bring a service back: take it out of STOP (and its uid out of
# BLOCK_UIDS if it needs the network), deploy, reboot. Section names and
# commands are in /basic/am_config; see docs/findings/03-system-map.md.
# Log: /var/local/vibe/debloat.log (rewritten each boot).
umask 022
BB=/usr/local/bin/busybox
V=/var/local/vibe
exec > $V/debloat.log 2>&1
up() { $BB cut -d' ' -f1 /proc/uptime; }
echo "debloat.sh at uptime $(up)"

# wrapper scripts before the programs they restart
STOP="restart_avs.sh avsservice HiAvsService TTSPlayer
run_browser.sh hisense_browser
GIBBON_MAIN nfxdevsplash
AirPlayApp airplaydemo airplay_bridge
QDA
app_upgrade ota_upgrade hi_logreport_se resmon
run_aiservice.s vidaa_ai_servic ai_manager
remoteservice remoteapp_biz_c mosquitto
dialserver dlna_service mdnsd
mdharvester masman reggieserver
websdk"

# The stock UI's own pieces: the app manager, the engine relauncher, the
# (blank) engine, the stock home screen's service. Neither keys nor standby
# and wake need them (tested with app_man frozen, then all four killed).
# Stopped only once vibe-ui runs, app_man first: it restarts some services.
# Nothing restarts them. (websdk, the browser file bridge, is in STOP: the
# browser it served is stopped too, so the bridge's disarm button is already
# out of reach; SSH and the switches in /var/local are the ways back.)
STOP_UI="app_man apmbond hisenseUI LauncherService"
UI_DONE=0

BLOCK_LABELS="vidaahub hismarttv hisense hisense-usa hisenseusa vidaa juhaokan"
# 304 ota_upgrade, 305 app_upgrade, 306 hi_logreport_service, 312
# LauncherService, 326 avsservice, 337 masman, 338 mdharvester,
# 339 reggieserver, 1032 QDA
BLOCK_UIDS="304 305 306 312 326 337 338 339 1032"

off() { [ -f $V/debloat.off ]; }

NAT="iptables -t nat"
DEAD_PORT=9     # nothing listens here: redirected traffic is refused at once

fw_remove() {
    while $NAT -D OUTPUT -j VIBE_OUT 2>/dev/null; do :; done
    $NAT -F VIBE_OUT 2>/dev/null
    $NAT -X VIBE_OUT 2>/dev/null
    # the filter-table chain of the first version, if a run left one
    while iptables -D OUTPUT -j VIBE_OUT 2>/dev/null; do :; done
    iptables -F VIBE_OUT 2>/dev/null
    iptables -X VIBE_OUT 2>/dev/null
}

fw_build() {
    $NAT -N VIBE_OUT 2>/dev/null
    $NAT -F VIBE_OUT
    for l in $BLOCK_LABELS; do
        hex="|$(printf '%02x' ${#l})|$l|"
        for p in udp tcp; do
            $NAT -A VIBE_OUT -p $p --dport 53 -m string --algo bm --icase \
                --hex-string "$hex" -j REDIRECT --to-ports $DEAD_PORT \
                || echo "rule failed: dns $p $l"
        done
    done
    for u in $BLOCK_UIDS; do
        for p in udp tcp; do
            $NAT -A VIBE_OUT -p $p ! -o lo -m owner --uid-owner $u \
                -j REDIRECT --to-ports $DEAD_PORT || echo "rule failed: uid $u $p"
        done
    done
    echo "VIBE_OUT (nat) built: $($NAT -S VIBE_OUT | $BB wc -l) lines"
}

# The whole chain and its hook in iptables-restore format: one process, one
# atomic commit (33 separate iptables runs took ~3 s at boot). The TV's
# iptables is a multi-call binary that acts as iptables-restore when run
# under that name. Declaring ":VIBE_OUT" creates the chain or empties it.
fw_rules() {
    echo "*nat"
    echo ":VIBE_OUT - [0:0]"
    for l in $BLOCK_LABELS; do
        hex="|$(printf '%02x' ${#l})|$l|"
        for p in udp tcp; do
            echo "-A VIBE_OUT -p $p --dport 53 -m string --algo bm --icase --hex-string \"$hex\" -j REDIRECT --to-ports $DEAD_PORT"
        done
    done
    for u in $BLOCK_UIDS; do
        for p in udp tcp; do
            echo "-A VIBE_OUT -p $p ! -o lo -m owner --uid-owner $u -j REDIRECT --to-ports $DEAD_PORT"
        done
    done
    echo "-I OUTPUT 1 -j VIBE_OUT"
    echo "COMMIT"
}

fw_ensure() {
    $NAT -C OUTPUT -j VIBE_OUT 2>/dev/null && return
    if fw_rules | (exec -a iptables-restore /usr/local/bin/iptables --noflush); then
        echo "VIBE_OUT (nat) restored, hooked at uptime $(up): $($NAT -S VIBE_OUT | $BB wc -l) lines"
        return
    fi
    echo "iptables-restore failed; building rule by rule"
    fw_build
    $NAT -I OUTPUT 1 -j VIBE_OUT && echo "VIBE_OUT hooked into nat OUTPUT at uptime $(up)"
}

# /proc/<pid>/status starts "Name:<tab><name>", then (Umask, then) "State:".
# read is a builtin, so a pass costs no forks (an awk per pid and name took
# over a minute). Sets n (name) and s (state letter, T = stopped).
name_of() {
    n=""; s=""
    { read -r _ n; read -r _k _v; [ "$_k" = "State:" ] || read -r _k _v; s=${_v%% *}; } \
        2>/dev/null < /proc/$1/status
}

# app_man respawns some services (avsservice, dialserver, ai_manager) within
# a second of their death. A name we have already killed once is frozen with
# SIGSTOP instead: it stays alive, so it is not respawned, and uses no CPU.
SEEN=" "

# one scan of /proc over the names in $1
scan_kill() {
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        name_of $pid
        [ -n "$n" ] || continue
        if [ "$s" = "T" ]; then             # frozen earlier
            # with app_man gone nothing respawns: end it properly
            [ $UI_DONE = 1 ] && case " $1 " in *" $n "*)
                kill -KILL $pid 2>/dev/null && echo "  frozen $n:$pid killed" ;; esac
            continue
        fi
        case " $1 " in
        *" $n "*)
            case "$SEEN" in
            *" $n "*) kill -STOP $pid 2>/dev/null && frozen="$frozen $n:$pid" ;;
            *) kill -TERM $pid 2>/dev/null && killed="$killed $n:$pid" ;;
            esac ;;
        esac
    done
}

WRAPPERS=$(for n in $STOP; do case $n in (*.sh|*.s) echo $n ;; esac; done)
ALL=$(echo $STOP)

stop_pass() {
    killed=""; frozen=""
    scan_kill "$(echo $WRAPPERS)"     # wrappers first: they restart their child
    scan_kill "$ALL"
    [ -n "$frozen" ] && echo "uptime $(up) frozen (came back):$frozen"
    [ -z "$killed" ] && return
    echo "uptime $(up) stopped:$killed"
    for k in $killed; do SEEN="$SEEN${k%%:*} "; done
    reap
}

# kill what was TERMed and is still there after 3 s
reap() {
    sleep 3
    for k in $killed; do
        pid=${k#*:}
        name_of $pid
        [ "$n" = "${k%%:*}" ] && { kill -KILL $pid 2>/dev/null; echo "  $k needed SIGKILL"; }
    done
}

ui_pass() {
    [ $UI_DONE = 1 ] && return
    [ -n "$(pidof vibe-ui)" ] || return
    # vibe-ui may start before the stock engine: wait for the engine (up to
    # a minute of uptime), so it is stopped, not missed
    u=$(up); u=${u%.*}
    engine=$(pidof hisenseUI)
    [ -z "$engine" ] && [ "$u" -lt 60 ] && return
    # early.sh's overlay worked if app_man got as far as starting the engine:
    # confirm it, so the next boot overlays again (an unconfirmed boot makes
    # the next one skip the overlay)
    # (only while the overlay is mounted: a marker from an earlier boot is
    # early.sh's to judge, at the next boot)
    if [ -n "$engine" ] && [ -f $V/early.pending ] && grep -q " /basic/am_config " /proc/mounts; then
        rm -f $V/early.pending
        $BB fsync $V
        echo "uptime $(up) early overlay confirmed (hisenseUI $engine started)"
        echo "$(date) uptime $(up): confirmed (hisenseUI started)" >> $V/early.history
    fi
    killed=""; frozen=""
    scan_kill "app_man"
    sleep 2
    scan_kill "$STOP_UI"
    [ -n "$killed" ] && { echo "uptime $(up) stock UI stopped:$killed"; reap; }
    UI_DONE=1
    # later passes stop them again if they ever come back; not added to
    # SEEN, so they are killed, never frozen (a frozen engine would keep
    # its window)
    ALL="$ALL $STOP_UI"
}

if off; then echo "debloat.off present, not running"; fw_remove; exit 0; fi
# the filter-table chain of the first version, if a run left one
while iptables -D OUTPUT -j VIBE_OUT 2>/dev/null; do :; done
iptables -F VIBE_OUT 2>/dev/null; iptables -X VIBE_OUT 2>/dev/null
fw_ensure
while :; do
    if off; then echo "debloat.off seen at uptime $(up), removing VIBE_OUT, exiting"; fw_remove; exit 0; fi
    stop_pass
    ui_pass
    fw_ensure
    u=$(up); u=${u%.*}
    # every 2 s while the late starters appear (the browser ~11 s, started
    # by systemserver, not app_man: the early overlay cannot stop it), then
    # every 5 s to 2 minutes, then every minute
    if [ "$u" -lt 40 ]; then sleep 2; elif [ "$u" -lt 120 ]; then sleep 5; else sleep 60; fi
done
