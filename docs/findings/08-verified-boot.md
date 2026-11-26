# 08 — Verified boot: what it covers and what it is signed with

2026-11-26, read-only. The owner asked whether a signing key had been left
on the TV.

## Measured

- `/proc/cmdline`: `verifiedbootstate=green`, `veritymode=enforcing`,
  `vbmeta.device_state=locked` (see [01-unit.md](01-unit.md)).
- `vbmeta` is `mmcblk0p15` (64 KB; `vbmetaB` is p16). It was read with
  `dd` (read only) and parsed on the PC (`research/avb/`, gitignored):
  - `AVB0`, libavb 1.0, `avbtool 1.1.0`, algorithm 2 = SHA256_RSA4096,
    rollback index 0, flags 0.
  - Public key (AVB blob) SHA-1 `2597c218aae470a130f61162feaae70afd97f011`.
    Its modulus is **byte-identical to AOSP's public test key
    `external/avb/test/data/testkey_rsa4096.pem`** (downloaded from
    android.googlesource.com and compared on the PC). The private half of
    that key is published in AOSP.
  - Ten descriptors: two kernel command lines (the root fs `dm=` verity
    line), a hash of `KL` (the kernel, 7.0 MB), and SHA-1 hash trees for
    `rootfs`, `3rd`, `LIB`, `MWLIB`, `APP`, `usrfs`, `optfs`. **No chain
    descriptors**: one key covers everything.
- On-device search for other private keys: regular files ≤ 8 MB on every
  mount except `/proc`, `/sys`, `/dev`, `/run`, `/tmp` and `/var/log`.
  The only "BEGIN … PRIVATE KEY" hits are parser strings (glib's
  `libgio`, `wpa_supplicant`, `hisenseUI`, `vuduapp`,
  `libhbbtv_module.so`), with no key body after them. `*.pem` files are CA
  bundles and certificates.

## What it means (inferred, untested)

- The bootloader (MBOOT) accepts `vbmeta` signed with the AOSP test key
  (it boots green and locked), so it trusts that public key. A changed
  protected partition could, in principle, be re-hashed and `vbmeta`
  re-signed with the public test key, and still boot green.
- **Not done and not planned.** It needs writing raw eMMC (`vbmeta` and
  the partition), which AGENTS.md forbids. A mistake could leave a TV
  that does not boot. How MBOOT behaves after a failed verification
  (fallback to the B slot, recovery, or a loop) is unknown, and so is
  whether `vbmetaB` / `bootflag` give a safe A/B path. Bind mounts from
  `/var/local` already cover the changes made so far, with no write to a
  protected partition.
- Open, if this is ever wanted: compare `vbmetaB` with `vbmeta`; find out
  what `bootflag` (p4) selects; read MBOOT's AVB handling (rollback,
  fallback).

## A/B and recovery (read-only, 2026-11-26)

- **Running slot: B.** `/proc/cmdline` has `bootflag=1`, `otaflag=85`,
  and `androidboot.vbmeta.device=179:16` (= `mmcblk0p16`, `vbmetaB`);
  `androidboot.vbmeta.digest` equals the SHA-256 of `vbmetaB` (4160
  bytes). `bootflag` (p4) starts `01 00 00 00 55 55 00 55`.
- **Only the boot chain is doubled:** `KL`/`KLB`, `optee`/`opteeB`,
  `armfw`/`armfwB`, `vbmeta`/`vbmetaB`, `rootfs`/`rootfsB`. Single copies:
  `LIB`, `MWLIB`, `APP`, `3rd`, `usrfs`, `optfs` (and the data
  partitions). Both vbmetas are signed with the test key (rollback index
  0), and both list hash trees for the single partitions, **with different
  root digests in A and B for every partition**. Either slot A belongs to
  older contents (the single partitions since overwritten by an OTA, so A
  may not verify any more) or the salt differs per build. Recomputing a
  hash tree would tell them apart; not done.
- MBOOT (read with `dd`, strings only) has `validate_vbmeta_public_key`,
  rollback indexes kept in **RPMB** (`read_/write_rollback_index`,
  `avb_load_info_from_rpmb`), an AVB status screen (`do_avbShowStatus`,
  `Load_avb_jpg`), `read_/write_bootflag_partition`, a recovery mode
  ("boot to recovery mode", `readKL recovery; bootKL`), boot modes chosen
  from the keypad (`get_boot_mode_from_keypad`), and **USB flashing from
  the bootloader** (`do_usb_partial_upgrade_to_emmc`, `ota_zip_check_usb`,
  `do_usb_bin_check`, `do_flash_bin`). Not traced: what it does after a
  failed verification (fall back to the other slot, the status screen,
  recovery), or which key press starts USB flashing.

So the A/B scheme is **not a safety net for the partitions that hold the
services** (`usrfs`, `optfs`): they exist once, and both slots check
them. A failed edit there would be undone only by re-flashing them, for
example through MBOOT's USB path with an official firmware image for this
model (not on hand). Changes limited to `rootfs` or `KL` could in principle
be tried on the inactive slot A, but slot A may not verify against
today's single partitions anyway.
