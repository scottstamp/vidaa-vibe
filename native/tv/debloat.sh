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

# DNS: the TV's manual-IP settings (network/cfg/manual-ip/dns1, from the
# rooting work) name a PC that no longer answers DNS for general names, and
# the network service writes them to resolv.conf at boot and on reconnects.
# Each pass puts these servers back. Rewritten in place: the file belongs to
# nwksrv. Switch: $V/dns.off (then the stock file returns at the next boot).
DNS="1.1.1.1 1.0.0.1"
RESOLV=/var/local/etc/resolv.conf
dns_ensure() {
    [ -f $V/dns.off ] && return
    local want cur s
    want=$(for s in $DNS; do echo "nameserver $s"; done)
    cur=$(cat $RESOLV 2>/dev/null)
    [ "$cur" = "$want" ] && return
    echo "$want" > $RESOLV && echo "uptime $(up) resolv.conf set to: $DNS (was: $(echo $cur))"
}

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

# Diagnostics (read-only): once, a vibe-ui boot could not draw because the
# shared DirectFB world kept a full-screen surface owned by a dead process
# that nothing had recorded (docs/findings/05-traps.md). proc_watch logs
# every process seen in the first 60 s of uptime, once each, to
# boot-procs.log, then checks /proc/fusion/0/refs at 60, 120 and 300 s for
# DirectFB objects whose owner is dead and names the owner from that list.
PROCLOG=$V/boot-procs.log
proc_line() {   # $1 pid: sets pl to "pid ppid uid name argv0", or empty
    pl=""; local nm="" pp="" ui="" k v a0=""
    while read -r k v; do
        case $k in
        Name:) nm=$v ;; PPid:) pp=$v ;; Uid:) ui=${v%%[!0-9]*}; break ;;
        esac
    done 2>/dev/null < /proc/$1/status
    [ -n "$nm" ] || return
    read -r -d '' a0 2>/dev/null < /proc/$1/cmdline
    pl="$1 $pp $ui $nm ${a0:--}"
}
fusion_check() {
    local line o
    [ -r /proc/fusion/0/refs ] || return
    while read -r line; do
        o=${line#*(}; o=${o%%)*}; o=${o// /}
        case $o in (''|*[!0-9]*) continue ;; esac
        [ -d /proc/$o ] && continue
        # the world's master (app_man, stopped on purpose) keeps its layer
        # objects and one screen surface: normal. Only surfaces and windows
        # are of interest.
        case $line in (*Surface*|*Window*) ;; (*) continue ;; esac
        echo "uptime $(up) fusion object of dead pid $o: ${line#*) }  [$(grep "^$o " $PROCLOG | $BB head -n 1)]"
    done < /proc/fusion/0/refs
}
proc_watch() {
    local seen=" " pid u
    echo "# pid ppid uid name argv0 (first seen at uptime)" > $PROCLOG
    while :; do
        u=$(up)
        for p in /proc/[0-9]*; do
            pid=${p#/proc/}
            case "$seen" in *" $pid "*) continue ;; esac
            proc_line $pid
            [ -n "$pl" ] || continue
            seen="$seen$pid "
            echo "$pl @$u" >> $PROCLOG
        done
        u=${u%.*}
        [ "$u" -ge 60 ] && break
        sleep 1
    done
    for t in 60 120 300; do
        u=$(up); u=${u%.*}
        [ "$u" -lt $t ] && sleep $((t - u))
        echo "uptime $(up) fusion check" >> $PROCLOG
        fusion_check >> $PROCLOG
    done
}

# The stock browser (started by app_man at about 12 s) lived until about
# 20 s: the main loop is busy in ui_pass (sleeps, reap) when it appears. A
# browser process that draws a full-screen DirectFB surface in that window
# and dies after the world's master is gone leaves the surface behind, and
# vibe-ui can then allocate nothing (suspected cause of the 2026-11-26
# incident, 05-traps.md). browser_watch kills it as soon as it appears:
# a /proc scan every 0.5 s (0.16 s each, measured) up to 40 s of uptime;
# the main passes cover anything later.
BROWSER="run_browser.sh hisense_browser"
browser_watch() {
    local u pid
    while :; do
        for p in /proc/[0-9]*; do
            pid=${p#/proc/}
            name_of $pid
            case " $BROWSER " in
            *" $n "*) [ -n "$n" ] && kill -KILL $pid 2>/dev/null \
                && echo "uptime $(up) browser watch: killed $n:$pid" ;;
            esac
        done
        off && return
        u=$(up); u=${u%.*}
        [ "$u" -ge 40 ] && return
        sleep 0.5
    done
}

if off; then echo "debloat.off present, not running"; fw_remove; exit 0; fi
proc_watch &
browser_watch &
# the filter-table chain of the first version, if a run left one
while iptables -D OUTPUT -j VIBE_OUT 2>/dev/null; do :; done
iptables -F VIBE_OUT 2>/dev/null; iptables -X VIBE_OUT 2>/dev/null
fw_ensure
while :; do
    if off; then echo "debloat.off seen at uptime $(up), removing VIBE_OUT, exiting"; fw_remove; exit 0; fi
    stop_pass
    ui_pass
    fw_ensure
    dns_ensure
    u=$(up); u=${u%.*}
    # every 2 s while the late starters appear (the browser ~11 s, started
    # by systemserver, not app_man: the early overlay cannot stop it), then
    # every 5 s to 2 minutes, then every minute
    if [ "$u" -lt 40 ]; then sleep 2; elif [ "$u" -lt 120 ]; then sleep 5; else sleep 60; fi
done
