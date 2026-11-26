# AGENTS.md — working on vidaa-vibe with an AI session

Briefing for an agent, or a person driving one. Read
[docs/findings/05-traps.md](docs/findings/05-traps.md) before touching the TV,
and [docs/findings/06-native-route.md](docs/findings/06-native-route.md) for
how the replacement works.

## What this repo is

One goal: replace the stock VIDAA UI on the owner's Hisense 58A51HUV with our
own code. The TV is rooted (key-only SSH on tcp/2222). The TV is used for its
HDMI inputs only: no live TV, none of the built-in apps.

## Where it stands

`vibe-ui`, our native DirectFB program, is the UI, started at every boot:

1. `rc.local` runs the hook `/var/local/otaTest.sh`; its vidaa-vibe block
   starts `/var/local/vibe/boot.sh` (`native/tv/hook-patch.sh` adds or
   removes the block).
2. `boot.sh` starts `debloat.sh`, waits for the stock engine, then runs
   `vibe-ui` and restarts it whenever it exits (backing off up to 60 s;
   the previous run's log is kept as `last-vibe-ui.prev.log`).
3. The stock engine boots from slot B, a blank transparent page
   (`runFlag` 1 in `/var/local/appUpgrade/appRunTimeInfoFile.json`), so it
   draws nothing.
4. `debloat.sh` stops the stock services the TV does not use, then, once
   `vibe-ui` runs, `app_man`, `apmbond`, `hisenseUI` and `LauncherService`.
   It also keeps the TV off Hisense's servers (nat-table chain `VIBE_OUT`)
   while leaving the internet open for third-party services.

`systemserver` and `tvmain` stay: they handle the remote, power, picture,
sound and CEC. The stock bundle in `/APPS/channelA` is untouched.

Jellyfin (owner-tested; record in
[docs/findings/07-jellyfin.md](docs/findings/07-jellyfin.md), summary at
its top): a Jellyfin card on vibe-ui's Home opens a panel laid out like
the web client (Home/Favourites/Search tabs, shelves, poster grids, detail
page). Playback runs in a child, `vibe-play`, through Hisense's HMP
library. It attaches to MI, switches the source to `DMP` before playing,
and switches back to the input after. If the child dies, vibe-ui goes
input → DMP → input to get the picture back.

Ways back (all over SSH, each documented in 06-native-route.md):

| Switch | Effect |
| --- | --- |
| `touch /var/local/vibe/off` (or `/var/local/vibe-off`) | `vibe-ui` not started at the next boot |
| `touch /var/local/vibe/debloat.off` | nothing stopped, no Hisense block, from the next boot (the running script lifts the block within 60 s) |
| `touch /var/local/vibe/jf.off` | no Jellyfin card on Home, from the next vibe-ui start (nothing of the Jellyfin code runs until the card is opened) |
| `touch /var/local/vibe-stock` | at the next boot, `boot.sh` restores the original runtime file (md5-checked) and starts nothing; the stock UI is back from the boot after |
| `HOOK_STEP=revert sh /var/local/vibe/hook-patch.sh` | takes our block out of the hook |
| `touch /var/local/vibe/splash.stock` | the stock VIDAA boot animation again from the next boot (ours is `/var/local/vibe/splash`, bind-mounted by `early.sh`) |
| `touch /var/local/vibe/jf.force-transcode` | test only: every Jellyfin play asks the server for its HLS conversion (remove it after) |

## Ground rules

- **Only this TV, on this LAN.** No DRM circumvention.
- **Every change has a way back, written down before the change.** The way
  back must not depend on the thing being changed: a UI experiment is
  recovered over SSH, never through the UI.
- **Do not lose SSH.** It is the recovery path for everything else. Never
  restart, replace or reconfigure the daemon without a second way in already
  running (see "Updating the daemon" in
  [02-root-access.md](docs/findings/02-root-access.md)).
- **The boot path must keep working.** A change to `boot.sh`, `debloat.sh`
  or `vibe-ui` is deployed and tried with `restart-ui.sh` first; the
  switches above must keep working. Deploys write `.new` and rename, never
  rewrite a running script in place.
- **Verified boot stays green.** Read-only partitions are not remounted, and
  raw eMMC (`/dev/mmcblk*`) is never written.
- **Leave `tvmain` alone** (owner's decision), even if it could be replaced:
  it runs inputs, picture, sound and CEC, and a broken one may not be
  recoverable.
- **Never touch:** `/OAD` (OTA staging), `/var/tvservice/reset_working`
  (factory-reset trigger), `/var/local/oem` (root OEM deployer),
  `/APPS/pkgs/NewUpdatedIPK.txt` (root-exec trigger). Reasons are in the
  alvidaa repo, `tools/launcher-host/never-touch.json`.
- **The stock UI bundle in `/APPS/channelA/hisenseUI` is the fallback.** Do
  not edit it in place until there is a verified copy elsewhere and a tested
  way to restore it.
- **`kilby.action.system.switch.off.tv` only with
  `system.gui.loop.finished` = 1** (vibe-ui sets it at start). Without it,
  the action left the TV in a standby nothing woke from. vibe-ui sends it
  only at sleep-timer end; POWER is left to the system.
- **Never guess model action names.** The `system.*` family includes
  factory reset and clearing user data.
- **Volume never above 15.** The owner sits next to the soundbar.
- **Never start the UI engine as root**, and never leave root-owned files in
  its state directories (`vibe-ui` runs as UhisUI, uid 311; its state is in
  `/var/local/vibe/state`, owned by it).
- **MQTT:** on this TV never send `sendkey`, never poll `gettvstate`, never
  send `authenticationcodeclose` to an unanswered dialog. A PIN-dialog loop
  follows and only a re-pair clears it. (The broker is stopped now; the rule
  stands if it comes back.)
- **Ask before anything the owner would see on screen**: rebooting,
  restarting the UI while they use it, opening menus through the dev hook,
  switching inputs. Reboots (`sync; /bin/reboot`) come back on their own.
- **Keys and tokens stay in `server/keys/`** (gitignored).
- **Never sign out of Jellyfin from a test.** Signing out revokes the token
  on the server, and tests run with a copy of the UI's session; it happened
  once and the owner had to sign in again. `jfui_test` sets `JFUI_TEST`,
  which makes the panel refuse.
- **No thread-local variables in TV binaries** (`__thread`,
  `_Thread_local`, stb_image's defaults). With zig they add a dependency on
  `ld-linux-armhf.so.3`, which this TV lacks: the binary does not start,
  and vibe-ui would loop in boot.sh. `build.sh` fails on such a binary.

## Building and deploying

- Build in WSL: `wsl.exe -e bash native/build.sh` (zig, armv7 hard-float,
  glibc 2.21). WSL cannot reach the TV.
- Deploy from Windows: `TV_HOST=192.168.2.152 native/deploy.sh`. It copies
  binaries, scripts and `native/fonts/` (Noto Sans, OFL) and starts nothing.
  vibe-ui falls back to the stock Roboto if a font is missing.
- Apply: `sh /var/local/vibe/restart-ui.sh` on the TV (restarts `boot.sh`
  and `vibe-ui`; `debloat.sh` runs once per boot).
- The TV's address is DHCP-assigned; `server/tv` defaults to an old one, so
  set `TV_HOST`.
- Screens: `/var/local/vibe/dev` turns on the dev hook (`/tmp/vibe-cmd`:
  `key 0xHEX`, `shot`); `native/tools/tvshots.sh` drives it from the PC.
  Frame timing: run `restart-ui.sh` with `VIBE_TIMING=1`.

## Working over SSH

- No PTY. Use one-shot commands, or pipe a script: `ssh … 'sh -s' < script.sh`.
  Never pass `-t`.
- `id`, `uname`, `tail`, `wc`, `df`, `netstat`, `stty`, `which`, `cut` exist
  only as busybox applets: call `/usr/local/bin/busybox <applet>`. `sort` and
  `xargs` exist only in `/bin`. Not present at all: `tr`, `wget`, `curl`,
  `nc`, `readlink`, `uniq`, `cmp`, `strings`, `od`.
- `/bin/sh` is bash 3.2: a `case` pattern inside `$( )` needs its leading
  `(`.
- `/usr/local/bin/iptables` is multi-call: `exec -a iptables-restore …`.
- A command that can block (reading a device, a fifo) needs its own
  watchdog: `read -t` did not time out on a tty device here.
- Files that the browser bridge created (`/var/local/otaTest.sh`,
  `/var/local/alvidaa-db.*`) are owned by `UhisWebsdk`. Rewrite them in place
  with `cat new > file`; replacing them makes them root-owned and breaks the
  bridge's no-op button. (With vibe-ui in charge the browser and `websdk`
  are stopped, so that button is out of reach; it matters again only if the
  stock UI comes back.)
- Write anything with backslashes or quotes to a file first (or edit with a
  tool): heredocs and `sed` replacement strings have broken C strings here
  more than once. Python one-off edits did too: a `
` became a real
  newline, and non-ASCII text came out double-encoded ("Â·" for "·"). Use
  raw strings or C escapes, and after an edit check the source for
  `Ã[-¿]Â` byte pairs.

## Findings discipline

Every discovery goes into `docs/findings/` the same hour: what was measured,
how, and what is still unknown. A claim that was read in a script but not
observed says so. Raw output goes to `research/` (gitignored), never into a
tracked file if it quotes vendor code or carries the unit's identifiers.

## Map

| Path | What |
| --- | --- |
| `docs/findings/` | the record |
| `server/tv` | root SSH wrapper (`TV_HOST`, `TV_NOPTY`) |
| `server/keys/` | SSH key, pinned host key, pairing material (gitignored) |
| `server/vidaa_server.py`, `server/pair.py` | alvidaa's launcher server and MQTT pairing, copied as they were |
| `native/src/` | `vibe-ui` and the tools `vibe`, `vibe-model`, `vibe-gam` |
| `native/src/jf*.c`, `http.c`, `vibe-play.c`, `vibe-jf.c` | Jellyfin: API (`jf.c`), panel (`jfui.c`), images (`jfimg.c`), subtitles (`jfsub.c`), the player child, a command-line client |
| `native/splash/`, `native/tools/make-splash.py` | our boot animation (740x300 PNG frames + `animation.cfg`), deployed to `/var/local/vibe/splash` |
| `native/src/vibe-sem.c` | posts a named semaphore (GUICLOSE ends the stock boot animation; not needed with our frames) |
| `native/fonts/` | Noto Sans (OFL), deployed to `/var/local/vibe/fonts`; `*.ttf` is binary in `.gitattributes` |
| `native/tv/` | the scripts deployed to `/var/local/vibe` (`boot.sh`, `debloat.sh`, `restart-ui.sh`, `hook-patch.sh`, probes) |
| `native/slotb/` | the blank page for slot B and its installer |
| `native/tools/` | vendor-ABI tools, screenshots, `gfx_bench`, `jfui_test` (the Jellyfin panel without DirectFB, over SSH) |
| `research/` | recon scripts, raw output, screenshots (gitignored) |
