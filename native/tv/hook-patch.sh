#!/bin/sh
# Run on the TV as root: add the vidaa-vibe block to the boot hook, or take
# it out again. In place, so the files keep their UhisWebsdk owner.
#   HOOK_STEP=check   show what would change, write nothing
#   HOOK_STEP=apply   back up the current hook to otaTest.prev.bak, insert
#                     the block before 'echo "--- main done"', syntax-check,
#                     write in place
#   HOOK_STEP=revert  write otaTest.prev.bak back over otaTest.sh (in place)
#   HOOK_STEP=early-check|early-apply|early-revert
#                     the same for the two early.sh lines in the hook's first
#                     stage (marked vidaa-vibe-early); early-apply keeps a
#                     copy in /var/local/vibe/hook/otaTest.before-early and
#                     does not touch otaTest.prev.bak
set -u
BB=/usr/local/bin/busybox
H=/var/local/otaTest.sh
P=/var/local/otaTest.prev.bak
W=/var/local/vibe/hook
mkdir -p $W

make_new() {
    if grep -q "vidaa-vibe" $H; then echo "already patched"; return 1; fi
    grep -q '^echo "--- main done"' $H || { echo "anchor line not found"; return 1; }
    $BB awk '
      /^echo "--- main done"/ {
        print "# vidaa-vibe: start the native UI supervisor if installed and not switched off"
        print "if [ -f /var/local/vibe/boot.sh ] && [ ! -f /var/local/vibe/off ]; then"
        print "  $BB setsid /bin/sh /var/local/vibe/boot.sh </dev/null >/dev/null 2>&1 &"
        print "  echo \"vibe: boot.sh started\""
        print "fi"
      }
      { print }' $H > $W/otaTest.new
    /bin/sh -n $W/otaTest.new || { echo "syntax check FAILED"; return 1; }
    echo "--- diff (old vs new line counts: $($BB wc -l < $H) vs $($BB wc -l < $W/otaTest.new))"
    $BB diff $H $W/otaTest.new
    return 0
}

# The early lines: in the hook's first stage (rc.local's own call, ~5.3 s,
# before it detaches), right after it logs its start time, start early.sh in
# the background. Both lines carry the marker, so taking them out is a filter.
EARLY_ANCHOR='  cat /proc/uptime >> "$L"'
md5of() { set -- $($BB md5sum "$1" 2>/dev/null); echo "${1:-}"; }

# a hook we are about to install must still start dropbear: `sh -n` alone
# passes an empty or cut-off file
hook_sane() {
    /bin/sh -n "$1" || { echo "syntax check FAILED: $1"; return 1; }
    grep -q -x 'start_db' "$1" || { echo "no start_db call in $1"; return 1; }
    return 0
}

# write $1 over the hook in place (keeps the UhisWebsdk owner), verify, and
# put $2 (the copy from before) back on any mismatch
install_hook() {
    cat "$1" > $H
    sync
    if [ "$(md5of $H)" != "$(md5of "$1")" ]; then
        cat "$2" > $H
        sync
        echo "WRITE MISMATCH: hook restored from $2 ($(md5of $H))"
        return 1
    fi
    return 0
}

early_new() {
    if grep -q "vidaa-vibe-early" $H; then echo "early lines already in"; return 1; fi
    n=$(grep -c -x -F "$EARLY_ANCHOR" $H)
    [ "$n" = 1 ] || { echo "early anchor found $n times (want 1)"; return 1; }
    $BB awk -v anchor="$EARLY_ANCHOR" '
      { print }
      $0 == anchor {
        print "  # vidaa-vibe-early: overlay the trimmed app_man config before app_man reads it"
        print "  [ -f /var/local/vibe/early.sh ] && $BB setsid /bin/sh /var/local/vibe/early.sh </dev/null >/dev/null 2>&1 &  # vidaa-vibe-early"
      }' $H > $W/otaTest.early.new || { echo "awk failed"; return 1; }
    old=$($BB wc -l < $H); new=$($BB wc -l < $W/otaTest.early.new)
    [ "$new" -eq $((old + 2)) ] || { echo "line count $old -> $new (want +2)"; return 1; }
    # the new hook minus our two lines must be exactly the old hook
    grep -v "vidaa-vibe-early" $W/otaTest.early.new > $W/otaTest.roundtrip
    [ "$(md5of $W/otaTest.roundtrip)" = "$(md5of $H)" ] || { echo "new hook is not old + 2 lines"; return 1; }
    hook_sane $W/otaTest.early.new || return 1
    echo "--- diff (line counts: $old vs $new)"
    $BB diff $H $W/otaTest.early.new
    return 0
}

case "${HOOK_STEP:-}" in
early-check)
    ls -la $H; md5sum $H
    early_new
    ;;
early-apply)
    early_new || exit 1
    cat $H > $W/otaTest.before-early    # our copy of the hook as it is now
    sync
    [ "$(md5of $W/otaTest.before-early)" = "$(md5of $H)" ] || { echo "backup mismatch; nothing written"; exit 1; }
    install_hook $W/otaTest.early.new $W/otaTest.before-early || exit 1
    ls -la $H; md5sum $H $W/otaTest.before-early $W/otaTest.early.new
    hook_sane $H && echo "installed hook: syntax OK, start_db present"
    ;;
early-revert)
    grep -q "vidaa-vibe-early" $H || { echo "no early lines in the hook"; exit 0; }
    cat $H > $W/otaTest.with-early
    sync
    grep -v "vidaa-vibe-early" $H > $W/otaTest.noearly.new || { echo "grep failed; nothing written"; exit 1; }
    old=$($BB wc -l < $H); new=$($BB wc -l < $W/otaTest.noearly.new)
    [ "$new" -eq $((old - 2)) ] || { echo "line count $old -> $new (want -2); nothing written"; exit 1; }
    hook_sane $W/otaTest.noearly.new || { echo "nothing written"; exit 1; }
    if [ -f $W/otaTest.before-early ] && [ "$(md5of $W/otaTest.noearly.new)" != "$(md5of $W/otaTest.before-early)" ]; then
        echo "note: result differs from otaTest.before-early (the hook changed since); writing the filtered hook"
    fi
    install_hook $W/otaTest.noearly.new $W/otaTest.with-early || exit 1
    ls -la $H; md5sum $H
    ;;
check)
    ls -la $H $P; md5sum $H $P
    make_new
    ;;
apply)
    make_new || exit 1
    cat $H > $W/otaTest.before          # our own copy as well
    cat $H > $P                          # prev.bak = hook as it is now
    sync
    cat $W/otaTest.new > $H              # in place
    sync
    ls -la $H $P; md5sum $H $P $W/otaTest.before $W/otaTest.new
    /bin/sh -n $H && echo "installed hook syntax OK"
    ;;
revert)
    grep -q "vidaa-vibe" $P && { echo "prev.bak is itself patched; refusing"; exit 1; }
    [ -s $P ] || { echo "prev.bak is empty; refusing"; exit 1; }
    cat $P > $H
    sync
    ls -la $H; md5sum $H $P
    ;;
*)
    echo "set HOOK_STEP=check|apply|revert"; exit 2 ;;
esac
