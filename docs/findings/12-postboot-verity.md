# 12 — Verification after boot (dm-verity, `mtk_mount`)

2026-11-26. Traced on the PC: Ghidra decompile of `/bin/mtk_mount`, the
rootfs init scripts, the kernel from the factory image, `usrfs`/`optfs`
extracted from it. Full report and tools: `research/postboot-re/REPORT.md`
(gitignored). Tags: [code] read in code or vendor scripts, [meas]
measured, [str] strings only, [inf] inferred. Checked by hand: the
re-sign identity, slot A's mismatch, the read-only flags, `main()`
discarding the result.

## Who verifies what

| Partition | Set up by | vbmeta source | On the TV |
| --- | --- | --- | --- |
| `rootfs`/`rootfsB` | kernel, from MBOOT's `dm=` cmdline (`init/do_mounts_dm.c`) | vbmeta cmdline descriptor | `dm-0` "system" ← p20 |
| `3rd`, `MWLIB`, `APP`, `LIB`, `usrfs`, `optfs` | `/bin/mtk_mount`, called from `rcS.utopia.linux` (A, passes p15) or `…linuxB` (B, p16), picked by `bootflag=` | hashtree descriptors in vbmeta(B) | `dm-1`…`dm-6` |

## `mtk_mount` [code]

- It's AOSP's `fs_mgr_avb` plus libavb 1.0, built as a wrapper:
  `mtk_mount <dev> <mnt> <fs> <name> <vbmeta-dev> vbmeta [nodev|nosuid|noexec]`.
- **Trust anchor:** its own key check is the AOSP stub (accepts any key).
  What protects it is that it SHA-256s the vbmeta and compares that with
  `androidboot.vbmeta.digest`/`.size` on the cmdline. MBOOT puts the
  digest there after its compiled-in test-key check.
- It builds the dm-verity table from the descriptor. The mode is a
  **compiled-in "enforcing"** (`restart_on_corruption`); the cmdline's
  `veritymode` is not read. `ignore_zero_blocks` is always set; FEC is
  added only when `fec_size` ≠ 0.
- On success it sets the raw partition read-only at the block layer
  (`BLKROSET`). Measured: `ro=1` on p21–p24, p37, p38; `ro=0` on p20, p39.
- **It ignores its own result.** `main()` discards the AVB setup's
  return value and mounts whatever the record names. On success that's
  `/dev/dm-N`. On any failure it's the raw partition, mounted **plain,
  unverified**, and it exits 0. Failures that lead to a plain mount:
  - vbmeta unreadable, corrupt or badly signed;
  - digest mismatch with the cmdline;
  - header flags with the hash tree disabled;
  - no descriptor for that partition;
  - any device-mapper error.
- No writes and no flags; it logs only to the (disabled) serial console.
- The init scripts check no exit status. Without `mtk_mount` they fall
  back to plain `mount`.

## Failure shapes

1. **Descriptor or vbmeta problem → silent unverified mount.** The TV
   boots normally.
2. **Data doesn't match a valid descriptor → dm-verity error on the first
   read of that block → `kernel_restart`** (mainline 4.19 behaviour;
   [str] matches, the ARM code path wasn't disassembled). A bad block
   read at mount (superblock) gives a boot loop **with no counter**:
   nothing writes `bootflag`, so MBOOT's slot flip never fires. Only the
   USB rescue breaks it. A bad block in a rarely read file reboots the TV
   whenever that file is first read.
3. FEC: RS(255,253), 2 roots; it repairs single-byte errors before the
   error path runs.

## Slot A is not a fallback [meas]

Slot A's `vbmeta` (p15) describes an older `usrfs` (different salt and
root). The TV's own p15 fails the hash-tree check against today's
`usrfs`; p16 matches. OTAs rewrite the single-copy partitions in place,
and only `vbmeta`/`rootfs`/`KL`/`optee`/`armfw` alternate. **A flip to
slot A would itself boot-loop.**

## usrfs carries recovery [code]

`start-hisense.sh`, on `usrfs`, mounts `/var/local` and the data
partitions and loads Wi-Fi. Our hook also uses `usrfs`'s busybox and
iptables. **No `usrfs` means no `/var/local`, no hook, no SSH, no Wi-Fi
and no vibe-ui.** `optfs` is less critical: without it Jellyfin playback
(`/opt/mmlib`) is lost, but SSH survives. Neither gets an `e2fsck` at
boot.

## Other integrity checks: none [code/meas]

No IMA/EVM, no module signing, no fs-verity. SELinux is loaded and then
`setenforce 0`. No file-hash checker runs. `checkFiles.sh` compares file
lists only, for the EULA app upgrade. The OTA writer
(`libbspatchupdate.so`) hashes only what it writes.

## Re-signing [meas]

Re-signing the stock `vbmetaB` with AOSP's published `testkey_rsa4096.pem`
reproduces it **byte for byte**: Hisense signed with the public test
private key. A dry run in `research/postboot-re/dry/` modified `usrfs`,
rebuilt the tree with the same salt, dropped FEC, re-signed `vbmetaB`,
then verified the signature and every partition's tree on the PC.
`rootfs`, `usrfs` and `optfs` all verify against `vbmetaB`, using the
offset-aware `fwextract.py`. (Correction: `research/fwscan/fwparse.py`
ignores segment offsets; `usrfs` has a 30 MiB hole at 0xb400000.)

Layout: `usrfs` 246,415,360-byte ext4 (≈58.8 MB free), tree at 0xEB00000,
FEC after it; `optfs` 668,991,488 bytes (≈36 MB free). Both use sha1, 4K
blocks and rollback index 0. FEC can be regenerated only with AOSP
`libfec`; setting `fec_size = 0` is the simpler route.

## What this means for changing system files

- Changing `usrfs` and `vbmetaB` together through a custom USB bin works.
  But a power cut or mistake between the two writes is the boot-loop case.
- **Lowest-risk first step (proposed, not done):** flash only a re-signed
  `vbmetaB` without the `usrfs` hashtree descriptor (one 4 KB write).
  `mtk_mount` then mounts `usrfs` plain: verity off for `usrfs` only, with
  everything else unchanged. Later `usrfs` changes then need no vbmeta
  work. Editing it live (remount rw) would mean relaxing the
  "no remount" rule, and an interrupted write could still leave an
  unmountable `usrfs`.
- Never raise the rollback index (RPMB, irreversible). Never touch
  MBOOT/MBOOTBAK or the boot area. The USB rescue path has never been
  exercised on this TV.

## Open

Kernel `verity_handle_err` disassembly; who feeds the hardware watchdog
(a `usrfs`-less boot probably power-cycles); whether `mmc erase.p` leaves
0x00 or 0xFF; `/usr/local/bin/deploy`'s boot-time writes.
