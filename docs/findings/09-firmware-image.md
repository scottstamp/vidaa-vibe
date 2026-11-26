# 09 — The official USB firmware image

2026-11-26, read-only (the image was parsed on the PC; the TV was only
read). The owner downloaded `usb_MTK_DEV_US_AP_EARC_4K.bin` (860,098,884
bytes, SHA-256 `0aa799ab…5e426a95`) into `research/recovery/`
(gitignored). Parser and checker: `research/fwscan/fwparse.py`,
`avbcheck.py`.

## Format

- MStar/Hisense USB upgrade. A text script at 0x1000 (510 lines, ends
  with `% <- this is end of script symbol`) of MBOOT commands, then
  lz4-compressed partition payloads that the script loads with
  `filepartload`.
- Bin info at 0x33441000 (= `REE_OFFSET_START` + `REE_OFFSET_LEN` in the
  header): product `MTK_DEV_US_AP_EARC_4K`, version `O.N0907`, market `US`.
- The last 4 bytes are a CRC-32 (zlib) of everything before them:
  `0x68f1ba74`, **matches** (the download is intact).

## Would MBOOT take it?

MBOOT (from our `dd` copy, strings only) has a USB path (`his_ustar`,
"check usb_xxx.bin file"): it lists `usb_*.bin` and `forcedupgrade_*.bin`
on the stick, reads each bin info, compares the product name with the
TV's, compares versions (a forced bin skips that), checks the full-file CRC
and the script CRC, then runs the script.

On the TV, MBOOT's env (in `MPOOL`, `mmcblk0p2`, read with `dd`):

| env | value | image |
| --- | --- | --- |
| `tv_serial_name` | `MTK_DEV_US_AP_EARC_4K` | product: same |
| `tv_sw_version` | `N.N0907` | version `O.N0907` |
| `panel_path` | `/config/panel/UD_VB1_8LANE_CSOT_URSA.ini` | script sets the same |
| `MS_RFS` | `root=/dev/mmcblk0p19 …` | script sets the same |

So the product check passes and the CRC passes. Whether `O.N0907` counts
as newer than `N.N0907` was **not traced** (MBOOT prints versions as
`%lld`; how it turns the strings into numbers is unread). If it does not,
MBOOT reports "The system version is the latest"; the `forcedupgrade_`
prefix exists for that case. How the USB path is started at boot (power-on
with the stick, a key held) was not traced either; the PDF names the TV
menu.

## Same build as the TV

- The image's `vbmeta` and `vbmetaB` are one file, **byte-identical to the
  TV's running `vbmetaB`** (SHA-256 `32c41a31…`; the TV's slot A `vbmeta`
  differs). vbmeta holds the hash-tree roots of `rootfs`, `LIB`, `MWLIB`,
  `APP`, `3rd`, `usrfs`, `optfs` and the hash of `KL`, so the image carries
  the same contents as the TV runs now.
- Checked directly for `rootfs`: its hash tree, recomputed from the image,
  equals vbmeta's root (`a9df5c0c…`). Its `/etc/rc.d/rc.local` has the
  same `/var/local/otaTest.sh` hook (line 106). The other partitions were
  not recomputed (pure-Python lz4 is slow); vbmeta equality covers them.
- So the root route ([02-root-access.md](02-root-access.md)) still works
  after flashing this image: the browser, its file bridge and `rc.local`
  are the build it was found on.

## What flashing it does (read from the script)

It is a full factory image, not an update:

- `mmc rmgpt` and re-creates the whole partition table, rewrites `MBOOT`,
  `MBOOTBAK` and eMMC boot partition 1 (a power cut here could leave a TV
  that does not start), both A/B slots, and every system partition.
- **Overwrites `varlfs` (`/var/local`)** with a near-empty factory copy
  (`flags.txt`, `remoteapp/`, `youtube/`, `upgrade_first_for_complete_deploy`):
  the root hook, dropbear, `vibe-ui` and its state, the slot B runtime file
  and the Jellyfin session go; the stock UI comes back.
- **Overwrites unit data with generic copies:** `serialdata`,
  `factorydata`, `criticaldata`, `servicedata`, `modeldata`, `certificate`,
  `tvconfig`; erases `eeprom_a`, `acfg`, `basic`, `data`, `cache`, `oad`.
  What of the unit's own data (serial number, MACs, keys, calibration) the
  TV re-creates afterwards is unknown. `demura` (panel uniformity data) is
  not written.

Not done: flashing, and backing up the unit partitions first (the obvious
step before ever using this image).
