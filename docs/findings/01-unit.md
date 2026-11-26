# 01 — The unit

Measured 2026-11-25 on the owner's TV, from pages in the TV browser and then
from the root shell. Unit identifiers (MAC addresses, serial, device id, host
key fingerprint) are left out on purpose; they are in the gitignored
`server/` reports and `server/keys/tv_known_hosts`.

## Identity

| Field | Value | Source |
| --- | --- | --- |
| Model | `58A51HUV` (serial type `58A5HU`) | `Hisense_GetModelName()`, `Hisense_GetSerialType()` |
| Serial number | in `server/recon-report-unit-identity.json` (gitignored) | `kilby.sl2.str.hisfactory.materiel.serial` (the stock UI's `getModelSerialNumber`); `factorySrv.ini serialnum` and `critical.ini BoardSerialNo` are empty on this unit |
| Firmware | `V0000.06.30N.N0907` | `Hisense_GetFirmWareVersion()` |
| VIDAA generation | U6 (`U06.30`) | `Hisense_GetOSVersion()`, `/usr/local/vidaa_version` |
| SoC | MediaTek MT9602 | `Hisense_GetChipSetName()` |
| Platform string | `hisense_mt5870_4k_us_AP_EARC_linuxDB` | `/etc/profile` |
| Region / country | `US` / `CAN` | bridge getters |
| Build date | 2023-09-07 | kernel banner |
| Panel | 1920x1080 | TV browser `screen` |

## Runtime

| Thing | Value | Source |
| --- | --- | --- |
| Kernel | Linux `4.19.79+`, SMP PREEMPT, armv7l | `uname -a` |
| CPU ABI | 32-bit ARM, hard-float userland | static armv7 musl binaries run |
| libc | glibc 2.21 | `/lib/libc-2.21.so` |
| RAM | 1,207,804 kB total, about 660 MB available with the stock UI up | `/proc/meminfo` |
| Swap | 603,132 kB | `free` |
| Shell | bash `3.2.57(3)-release` as `/bin/sh` and `/bin/bash` (links into `/openbox/bin`) | `ls -la`, `$BASH_VERSION` |
| busybox | `/usr/local/bin/busybox`, 181 applets | `busybox --list` |
| iptables | `v1.8.4 (legacy)` at `/usr/local/bin/iptables` | `iptables -V` |
| UI engine | Cobalt `21.lts.7` (`/opt/hisenseUI/lib/libcobalt.so`), built 2023-09-07 | version string, WebDriver `/status` |
| TV browser | Chromium `88.0.4324.182` (`/opt/webengine/bin/hisense_browser`) | user agent |
| Graphics | DirectFB 1.4 with the `mstar_mi` driver, Mali GPU (`/dev/mali0`), two framebuffers | UI engine's mapped files, `/proc/fb` |
| SELinux | present, **permissive** (`enforce` reads 0, at 5 s and at 95 s) | `/sys/fs/selinux/enforce` |
| Verified boot | `verifiedbootstate=green`, `veritymode=enforcing`, `vbmeta.device_state=locked` | `/proc/cmdline`, after every change so far |
| PTYs | none usable | see [05-traps.md](05-traps.md) |

## Tools on the TV

Present as files: `bash`, `sh`, `tar`, `gzip`, `base64`, `md5sum`, `sed`,
`awk`, `grep`, `head`, `find`, `sort`, `xargs`, `dd`, `du`, `free`, `stty`,
`mknod`, `top`, `pidof`, `ps`, `ifconfig`, `mount`, `strace` (all `/bin`);
`iptables`, `iperf`, `mosquitto`, `wpa_supplicant`, `unzip`, `screenshot`,
`inputkeyevent`, `superwatch` (`/usr/local/bin`); `gdb` (`/opt/bin`); `ldd`,
`killall` (`/usr/bin`).

Busybox applets only (`/usr/local/bin/busybox <applet>`): `id`, `uname`,
`tail`, `wc`, `df`, `netstat`, `timeout`, `setsid`, `which`, `pstree`,
`hexdump`, `zcat`, `xz`, `vi`.

Absent, as a file and as an applet: `wget`, `curl`, `nc`, `telnetd`, `tftp`,
`sshd`, `tr`, `readlink`, `uniq`, `cmp`, `strings`, `sha256sum`, `openssl`,
`pgrep`.

What we added: `/var/local/tvtools/dropbearmulti` (Dropbear 2026.94: `dropbear`,
`dropbearkey`, `scp`).

Not run, existence only: `screenshot`, `inputkeyevent`, `gdb`, `strace`.

## Accounts

83 entries in `/etc/passwd`. Root's shell is `/usr/local/scripts/rootlogin.sh`
(a USB-key gate), `television`'s is `login.sh`, every other account has
`false`. There is no `/etc/shells`. Services run under their own accounts:

| uid | Account | Runs |
| --- | --- | --- |
| 0 | root | `init`, `superwatch`, the boot scripts, our hook |
| 299 | (unnamed in `ps`) | `app_man`, the app manager |
| 301 | `UhisApm` | `systemserver`, `apmbond` |
| 303 | `UhisTvm` | `tvmain` |
| 304 | `UhisOta` | `ota_upgrade` |
| 305 | `UhisApp` | `app_upgrade`; owns the UI bundle |
| 311 | `UhisUI` | `hisenseUI`, the UI engine |
| 312 | `UhisLauncher` | `LauncherService` |
| 313 | `UhisBrowser` | `hisense_browser` |
| 314 | `UhisWebsdk` | `websdk`, the browser's file bridge |
| 324 | `UhisRemote` | `remoteservice` |
