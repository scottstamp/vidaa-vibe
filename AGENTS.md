# AGENTS.md — working on vidaa-vibe with an AI session

Briefing for an agent, or a person driving one. Read
[docs/findings/05-traps.md](docs/findings/05-traps.md) before touching the TV.

## What this repo is

One goal: replace the stock VIDAA UI on the owner's Hisense 58A51HUV with our
own code. The TV is rooted (key-only SSH on tcp/2222). Nothing of the stock UI
has been changed yet.

## Ground rules

- **DO NOT write anything to execute on boot**, keep the original OS UI
  in tact on boot so we can recover if anything goes wrong and the
  replacement causes a crash loop or similar. Once the project is in a
  stable state we will manually instruct you to create a boot hook.
- **Only this TV, on this LAN.** No DRM circumvention.
- **Every change has a way back, written down before the change.** The way
  back must not depend on the thing being changed: a UI experiment is
  recovered over SSH, never through the UI.
- **Do not lose SSH.** It is the recovery path for everything else. Never
  restart, replace or reconfigure the daemon without a second way in already
  running (see "Updating the daemon" in
  [02-root-access.md](docs/findings/02-root-access.md)).
- **Verified boot stays green.** Read-only partitions are not remounted, and
  raw eMMC (`/dev/mmcblk*`) is never written.
- **Never touch:** `/OAD` (OTA staging), `/var/tvservice/reset_working`
  (factory-reset trigger), `/var/local/oem` (root OEM deployer),
  `/APPS/pkgs/NewUpdatedIPK.txt` (root-exec trigger). Reasons are in the
  alvidaa repo, `tools/launcher-host/never-touch.json`.
- **The stock UI bundle in `/APPS/channelA/hisenseUI` is the fallback.** Do
  not edit it in place until there is a verified copy elsewhere and a tested
  way to restore it.
- **Never start the UI engine as root**, and never leave root-owned files in
  its state directories.
- **MQTT:** on this TV never send `sendkey`, never poll `gettvstate`, never
  send `authenticationcodeclose` to an unanswered dialog. A PIN-dialog loop
  follows and only a re-pair clears it.
- **Ask before anything the owner would see on screen.** Killing the UI
  engine, flipping the UI slot and rebooting all interrupt whoever is
  watching.
- **Keys and tokens stay in `server/keys/`** (gitignored).

## Working over SSH

- No PTY. Use one-shot commands, or pipe a script: `ssh … 'sh -s' < script.sh`.
  Never pass `-t`.
- `id`, `uname`, `tail`, `wc`, `df`, `netstat`, `stty`, `which` exist only as
  busybox applets: call `/usr/local/bin/busybox <applet>`. `sort` and `xargs`
  exist only in `/bin`. Not present at all: `tr`, `wget`, `curl`, `nc`,
  `readlink`, `uniq`, `cmp`, `strings`.
- `/bin/sh` is bash 3.2.
- A command that can block (reading a device, a fifo) needs its own
  watchdog: `read -t` did not time out on a tty device here.
- Files that the browser bridge created (`/var/local/otaTest.sh`,
  `/var/local/alvidaa-db.*`) are owned by `UhisWebsdk`. Rewrite them in place
  with `cat new > file`; replacing them makes them root-owned and breaks the
  bridge's no-op button, which is the disarm of last resort.

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
| `native/` | our own DirectFB client (`vibe`), build and deploy scripts, vendor-ABI tools; see its README |
| `research/58a51huv/recon/` | read-only recon scripts and their raw output (gitignored) |
