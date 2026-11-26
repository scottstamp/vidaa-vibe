# 10 — The SoC and the board

2026-11-26. TV read-only over SSH; public sources noted. Raw notes in
`research/soc1.sh`, `research/docs-dl/` (gitignored).

## Measured on the TV

| What | Value | How |
| --- | --- | --- |
| SoC | **MStar M7332** (device tree `arm,m7332`, model `M7332`); MediaTek sells it as **MT9602** (MBOOT build names `MT9602_U6NASAEARC…_US`) | `/proc/device-tree`, MBOOT strings |
| CPU | 4 × Arm Cortex-A53 (implementer 0x41, part 0xd03), run in 32-bit ARMv7 mode; NEON, VFPv4, LPAE, AES/SHA1/SHA2/CRC32 instructions | `/proc/cpuinfo` |
| RAM | `DRAM_LEN=0x60000000` (1.5 GB); Linux sees 1,207,804 kB (`MemTotal`), the rest is MI/video carve-outs | MBOOT env, `/proc/meminfo` |
| eMMC | 7,733,248 sectors (3.69 GiB, a "4 GB" part) | `/sys/class/block/mmcblk0/size` |
| GPU | Arm Mali (`mali_kbase` module) | `/proc/modules` |
| Wi-Fi / BT | MediaTek MT7663 over USB (`wlan_mt7663_usb`, `btmtk_usb_7663`) | `/proc/modules` |
| Kernel | Linux 4.19.79, built Sep 7 2023, Linaro GCC 4.9 | `/proc/version` |
| Bootloader env | `SECURITY=ON`, `UARTOnOff=off`, `console=ttyS0,115200` in bootargs | `MPOOL` (p2), read with `dd` |

## Public sources

- MediaTek's MT9602 page: Cortex-A53 64-bit quad up to 1.5 GHz, Mali-G52
  2EE MC1, DDR3 up to 2 GB, 4K60, HDMI 2.1a, Dolby Vision/HDR10+/AV1. It
  launched in 2020 in Motorola Android 10 TVs.
- Hisense service manual for the MT9602 chassis (2K EU models, not ours,
  same SoC family; `research/docs-dl/mt9602-service-manual.txt`):
  - Block diagram: "Linux: 4GB, Android: 8GB" eMMC. **The Android boards
    carry twice the eMMC** of ours. One HDMI port is labelled "debug".
  - Boot strap `CHIP_CONFIG` 3'b010 = "ARM boot from ROM; outer storage is
    eMMC".
  - **UART:** through the headphone jack with Hisense's USB-serial cable,
    after Factory menu → Options → Uart Switch → on (that is the
    `UARTOnOff` env). At power-on, Enter held gives the `<< MStar >>#`
    MBOOT prompt; `cu` there re-runs the USB upgrade.
  - **USB upgrade, user mode:** rename `usb_XXX.bin` to `MstarUpgrade.bin`
    alone in the stick's root, AC on while holding the TV's power key;
    about 8 minutes. Factory mode upgrades on AC on without the key.
  - **Unbrick (ISP):** MStar ISP_Tool 5.0.2 over I2C writes MBOOT to the
    eMMC, but only after "deciphering": the tool shows the chip's device
    ID, which is sent to MStar's security team for a per-chip password.
    So the hardware unbrick path depends on the vendor.
- Hisense sold MT9602 TVs with Android 11 (for example a 55A6H variant,
  chassis 55A53FEVS, per a third-party firmware listing). The A6 name
  covers both VIDAA and Android/Google TV boards, depending on the region.
- Community tools for MStar images (`mstar-bin-tool`: unpack/pack,
  extract AES and RSA keys from MBOOT) exist; on newer MStar builds with
  `SECURE_BOOT` the boot images are AES-encrypted and RSA-signed. Not yet
  checked against this TV's images.

## What it means for an Android build (inferred)

- Same SoC as Hisense's Android sets, but **not the same board**: 4 GB
  eMMC here vs 8 GB on Android boards, and 1.5 GB RAM. Android TV 11
  needs most of 4 GB for system/vendor alone; an Android partition layout
  would not fit as is.
- The bootloader trusts the AOSP test key (see 08); whether the Android
  variant's MBOOT and images are signed with the same keys, and whether
  this TV's first stage would accept another MBOOT, is unknown (MBOOT
  trace in progress).
- Android needs the vendor's TV HALs and kernel for this SoC; they exist
  only inside Hisense/MediaTek Android firmware, which we do not have.
