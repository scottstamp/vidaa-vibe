# 11 — MBOOT, traced

2026-11-26, PC-side only (no TV access, nothing written). Disassembled and
decompiled from the backups in `research/unit-backup/2026-11-26/`
(Ghidra 12.2 headless + capstone; scripts, databases and the full
decompile in `research/mboot-re/`, gitignored). Primary object: `MBOOTBAK`
(p3), the running build (`bootflag`=1), U-Boot 2011.06 "Sep 07 2023",
`MBOT-1106.0.10.1426731`. Tags: [code] followed in disassembly, [meas]
measured on the files, [str] strings only, [inf] inferred. Spot-checked
by hand: bin-info offset, unused script-CRC word, env copies and CRCs.

## Layout

- Signed container [code/meas]: header 0x000–0x2f0 signed by a `CHUN`
  `SECURITY` block (0x100-byte, RSA-2048-sized signature at 0x300); two
  identical `SECURITY` blocks at 0x400/0x1400 (a "Customer Key Bank",
  byte-identical in the Feb and Sep builds); U-Boot body at 0x2400 loaded
  at **0x27400000**, **ARM A32**, self-relocating (embedded `rel.dyn` +
  `.dynsym`); body signed by a trailing `UBOT` `SECURITY` block; two JPEGs
  (AVB status screens) appended.
- Env [meas]: `MPOOL` (p2), two 64 KiB copies at partition offsets
  0x5E0000 and 0x5F0000, each CRC-32 + NUL-separated vars.
- 243 commands in the U-Boot command table (MStar/Hisense extras:
  `bootcheck`, `bootflag_set`, `readKL`, `bootKL`, `custar`, `ustar`,
  `usb_partial_upgrade_to_emmc`, `avbab`, `store_secure_info`, …).

## Boot flow [code]

- Not driven by `bootcmd` (a fallback only): three command stages; stage 2
  runs `bootcheck` (boot-mode decision), `readArmFw;readOptee`, `readKL`
  (AVB + load kernel), `bootKL` (`bootm`).
- No interactive console window on this unit: `UARTOnOff=off`, no
  `bootdelay`.

### `bootflag` (p4)

| Byte | Field | Here |
| --- | --- | --- |
| 0 | active slot (0 = A, 1 = B) | 1 |
| 1 | `verify_kernel_flag` (0x5a = this slot failed) | 0 |
| 2–3 | `verify_optee_flag` | 0 |
| 4 | `boot_ota_flag` (→ `otaflag=85` on the cmdline) | 0x55 |
| 10 | `boot_error_flag` | 0 |

Slot B is applied by name rewriting on **reads only**: `mmc read.p … KL`
reads `KLB` when the flag is 1; `mmc write.p … KL` always writes slot A.
`vbmeta`/`vbmetaB` and `rootfs`/`rootfsB` follow the flag. optee, armfw
and MBOOT/MBOOTBAK are chosen by the first stage.

### Failure handling [code]

- First stage reports through RIU registers (0x103380/0x103382) read by
  `sboot_verify_check`: 0x5a = slot A's MBOOT/optee/armfw failed → flag 1
  and reset; 0x5a00 = slot B failed → flag 0; 0x5a5a = both → USB rescue
  (`custar`), then console.
- AVB failure → red screen, then `mboot_verify_check`: **one** automatic
  slot flip (sets `verify_kernel_flag`, resets); second failure prints
  "Both system image verify failed, Run USB upgrade to rescue" → `custar`
  → console. Nothing erases or re-flashes on its own.

### Boot modes (`bootcheck`) [code]

IR, then keypad, then a hardware reset line, then env:
- **TV keypad: key `D` held ≥ 1.1 s at power-on → USB upgrade; key `C` →
  `setenv UARTOnOff on; saveenv`** (opens the serial console for good).
  Which physical button is `C`/`D` is in a flash table not yet decoded.
- IR: a 5-entry table in flash maps remote keys to recovery/USB/wipe
  modes; NEC head code 0xbf with repeat → USB upgrade. Table contents not
  decoded.
- env: `force_upgrade` (1–15) or `upgrade_mode=usb` → USB upgrade.
  `FactoryOpenState=On` (it is Off) runs the USB upgrade on every boot.

## AVB [code/meas]

- MBOOT verifies **only `KL`** itself. The hash trees of `rootfs`, `LIB`,
  `MWLIB`, `APP`, `3rd`, `usrfs`, `optfs` are only turned into the
  kernel's `dm=` verity line, mode `restart_on_corruption` — enforced in
  Linux (`mtk_mount`), after MBOOT's fallback is out of reach. (Settles
  the inference in 09.)
- Trusted key: `validate_vbmeta_public_key` is a `memcmp` against a
  compiled-in 1032-byte blob = AOSP `testkey_rsa4096`. **No eFuse/OTP
  involved.** A vbmeta re-signed with the public test key verifies green.
- Lock state: locked unless env `devicestate=unlock` and an RPMB word is 0
  (here: locked). Rollback indices in RPMB, both 0; written only when
  locked and an image's index differs → **a vbmeta with a higher rollback
  index would permanently lock out every older image.**

## First stage (`mmcblk0boot0`)

- Plaintext: RSA-2048-sized blob + exponent 65537, DDR/eMMC init running
  from SRAM at 0x1fc00000, two ~35 KB encrypted/compressed blobs [meas].
  MBOOT's code proves the first stage verifies MBOOT/MBOOTBAK, optee and
  armfw [code]; the algorithm and key are inside the opaque part
  (blocked: needs a UART log, ROM dump or vendor tools).
- **MBOOT, MBOOTBAK and the eMMC boot area are untouchable**: signed with
  Hisense's key; a bad copy in both slots leaves nothing to run the USB
  rescue (brick without external eMMC access).

## USB upgrade: what it accepts [code/meas]

Corrects 09: any `*.bin` in the stick's root is considered, not only
`usb_*`.
1. Bin info at **`filesize − 0x144`**: product (must `strcmp`-equal env
   `tv_serial_name`), version at +0x20, market at +0x40.
2. Version: `<L>.<L><MMDD>` → year·10⁶ + month·10⁴ + day·100 + hour
   (letters A…Z as numbers). `O.N0907` (13,090,714) > `N.N0907`
   (13,090,713): the official image counts as newer. A **`forcedupgrade_`**
   name prefix skips the version test.
3. Text header at 0x300 (`REE_OFFSET_START`/`LEN`), script 512-aligned,
   ≤ 64 KiB, ≥ 1 command, `%` terminator.
4. **CRC-32 (zlib) of the whole file in the last 4 bytes — the only
   integrity check.** `check_script_crc` is used only by the OAD/network
   paths (the word it would read is `0xFFFFFFFF` here [meas]).
5. No signature, no encryption. Each script line runs as a U-Boot command
   with full privilege.

So a small custom `forcedupgrade_*.bin` can run any bootloader command:
write chosen partitions only, `bootflag_set 0|1`, `setenv UARTOnOff on;
saveenv`, or `usb_partial_upgrade_to_emmc <file> <part>` (raw write, no
checks). It cannot make an edited verified partition pass AVB; that still
needs rebuilt hash trees and a test-key-signed vbmeta.

OTA zip / recovery: **dead on this model** (`misc` and `recovery`
partitions do not exist).

## Irreversible hazards [code]

1. Writing MBOOT/MBOOTBAK/boot area (`mbup`, `mmc write.boot`,
   `upgrade_mboot`, `createMbootBak`, the factory script) — never.
2. `mmc rmgpt` (the factory script's first action).
3. RPMB rollback index bumps (any vbmeta with index > 0).
4. `avbab` device-state change (RPMB + erases userdata).
5. `store_secure_info` (key/signature provisioning in MPOOL).
6. `his_eraseE2KeyArb` — runs a check every boot; gated off here
   (`eraseE2Key_SerialSentry=FALSE`).
7. `cleanallenv` / default env (MACs, panel path, serial name) —
   restorable only from the MPOOL backup.

No eFuse write in MBOOT itself; OP-TEE (`SecureInit`) not analysed.

## Open

First-stage algorithm/key and eFuse dependence; whether `UARTOnOff=off`
gates UART RX physically; keypad and IR table contents (dumpable from the
backups); Customer Key Bank contents; OP-TEE.
