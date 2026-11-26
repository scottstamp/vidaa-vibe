# 13 — Plan: our own flashable image (parked)

2026-11-26. **Not started.** The owner wants it eventually (Vibe built in,
no Hisense services/apps) but is not ready to flash anything yet. This is
the agreed order of work for when it resumes. Evidence:
[09-firmware-image.md](09-firmware-image.md), [10-soc.md](10-soc.md),
[11-mboot.md](11-mboot.md), [12-postboot-verity.md](12-postboot-verity.md);
full reports in `research/mboot-re/REPORT.md` and
`research/postboot-re/REPORT.md`, tools in `research/postboot-re/`
(gitignored).

## What is possible

- **Can be ours (re-signed with AOSP `testkey_rsa4096`, boots green):**
  everything vbmeta covers — `KL`, `rootfs`, `usrfs`, `optfs`, `APP`,
  `LIB`, `MWLIB`, `3rd`. MBOOT trusts the key by `memcmp` against a
  compiled-in copy (no eFuse); re-signing the stock `vbmetaB` reproduces
  it byte for byte.
- **Cannot be ours:** `MBOOT`, `MBOOTBAK`, the eMMC boot area, `optee`,
  `armfw` (Hisense-signed, checked by the first stage). Never write them.
- **Must stay vendor:** kernel and modules, MI/utopia libraries, DirectFB,
  `tvmain`, `systemserver`, Wi-Fi, `/opt/mmlib` (HMP, for Jellyfin). The
  image is "stock minus Hisense services/apps, plus Vibe", not a new OS.
- Android TV: not on this board (4 GB eMMC vs 8 GB on Hisense's Android
  boards; no vendor Android HALs on hand). See 10.

## Hard rules for any flash

1. Only through MBOOT's USB path (`forcedupgrade_*.bin`, format in
   `research/mboot-re/REPORT.md` §5.5); never raw eMMC writes from Linux.
2. Never: `mmc rmgpt`, `MBOOT`/`MBOOTBAK`/`mmc write.boot`, a rollback
   index above 0, the unit data partitions, `avbab`, `store_secure_info`.
3. Write `vbmetaB` by that exact name (the `B` suffix is only added on
   reads of `KL`).
4. Partitions do not grow: `usrfs` ext4 image stays 246,415,360 bytes
   (≈58.8 MB free inside), `optfs` 668,991,488 (≈36 MB free). Hash tree
   with the same salt; FEC regenerated with AOSP `libfec` or dropped
   (`fec_size = 0`).
5. Verify on the PC before every flash: `vbverify.py` (signature),
   `hashtree.py verify` for **every** partition against the new vbmeta,
   `avbdump.py` (dm table), and the re-sign anchor (no-edit re-sign is
   byte-identical to stock).
6. Unit backup first: `research/unit-backup/2026-11-26/` (md5-checked).
   The stock factory image `research/recovery/usb_MTK_DEV_US_AP_EARC_4K.bin`
   is the last-resort rescue and wipes `/var/local` and unit data.

## Order of work (each step proves the next one's safety net)

1. **Prove the USB path harmlessly.** A tiny `forcedupgrade_*.bin` whose
   script only runs `setenv UARTOnOff on; saveenv`. Proves MBOOT accepts
   our files and opens the serial console (headphone-jack cable, see 10)
   for every later step. Needs: the trigger (keypad `D` held ≥ 1.1 s at
   power-on; which physical button is `D` is still undecoded — table at
   `keypad_offset=0x17AE0` in flash, dumpable from the backups).
2. **Prove the rescue.** Confirm the forced USB upgrade starts from the
   TV's button with a stick inserted; ideally get the serial console
   working too.
3. **Verity off for `usrfs` only.** Flash just a re-signed `vbmetaB`
   without the `usrfs` hashtree descriptor (4 KB). `mtk_mount` then
   mounts `usrfs` plain; a mismatch there can no longer boot-loop.
   Check: `/proc/mounts` shows `/dev/mmcblk0p37 on /usr/local`.
4. **Build the real image.** Stripped `usrfs`/`optfs` with Vibe baked in
   (hook-independent start, root that does not depend on `/var/local`),
   verity back on and signed, one USB file. Keep `/var/local` for state.

## Open before step 1

- Keypad `C`/`D` and IR boot-mode tables (which buttons/keys).
- Whether `mmc erase.p` leaves 0x00 or 0xFF.
- Who feeds the hardware watchdog (a `usrfs`-less boot probably
  power-cycles).
- What `/usr/local/bin/deploy` writes at boot (matters when stripping).
- AGENTS.md rules to revisit with the owner first: "never write raw eMMC"
  (USB-path flashing is a different thing, but should be written down),
  "verified boot stays green", remounting read-only partitions.
