# 03 — System map

Measured 2026-11-25 from the root shell, stock UI running. Raw output:
`research/58a51huv/recon/ui-recon-1.out` and `-2.out` (gitignored).

## Storage

eMMC, partitions on `mmcblk0`. "verity" means the mount is a dm-verity
device: read-only and covered by verified boot.

| Mount | Device | FS | Mode | Size (KiB) | Used | Note |
| --- | --- | --- | --- | --- | --- | --- |
| `/` | verity (p20) | squashfs | ro | 29,568 | 100% | `/etc`, `/bin`, `/basic`, static `/dev` nodes |
| `/3rd` | verity | squashfs | ro | 76,800 | 100% | third-party apps |
| `/application` | verity | ext4 | ro | 86,608 | 74% | MediaTek middleware (`mi_daemon`) |
| `/vendor/lib` | verity | ext4 | ro | 111,808 | 84% | vendor libraries, DirectFB, Mali |
| `/vendor/lib/utopia` | verity | ext4 | ro | 34,192 | 91% | |
| `/usr/local` | verity | ext4 | ro | 232,752 | 77% | Hisense services, scripts, busybox |
| `/opt` | verity | ext4 | ro | 632,820 | 96% | UI engine, browser, factory UI bundle |
| `/config` | p26 | ext4 | ro, noexec | 16,048 | 45% | also at `/vendor/tvconfig/config` |
| `/hidata/model`, `/critical`, `/factory` | p36, p33, p34 | ext4 | ro | small | | |
| `/serialdata` | p40 | ext4 | ro | 147,072 | 27% | |
| **`/APPS`** | p41 | ext4 | **rw** | 595,136 | 22% | the live UI bundle, app data |
| **`/var/local`** | p39 | ext4 | **rw** | 337,232 | 15% | service state, our hook and tools |
| `/data` | p27 | ext4 | rw, nosuid, nodev | 78,544 | 12% | |
| `/cache` | p28 | ext4 | rw, noexec | 81,568 | 77% | |
| `/certificate` | p17 | ext4 | rw, noexec | 16,048 | 3% | |
| `/var/tvservice` | p35 | ext4 | rw | 12,016 | 3% | holds the factory-reset trigger: do not write |
| `/OAD` | p42 | ext4 | rw | 892,696 | 0% | OTA staging: never touch |
| `/dev/shm` | tmpfs | | rw | 603,900 | 1% | `/tmp` is a link to `/dev/shm/shm_tmp` |
| `/var/run`, `/var/log`, `/var/crash` | tmpfs | | rw | | | `/var/log` is 4 MiB |

Room for our own code: about 450 MB on `/APPS` and 280 MB on `/var/local`,
both executable, neither verified.

## Boot chain

From the scripts' text, with the points we observed marked.

1. `inittab` sysinit runs `/etc/init.d/rcS`; a getty respawns on `ttyS0` at
   115200.
2. `rcS` runs `rcS.utopia.linux`, or `linuxB` when `bootflag=1` (this unit
   boots with `bootflag=1`), then `/etc/rc.d/rc.local`. With `otaflag=170`
   on the kernel command line it runs `rc.ota` instead and skips `rc.local`.
3. `rc.local` starts the basic services, the boot animation, and exports
   **`HI_UI_HOME`**: `/APPS/channelB` if `runFlag` in
   `/var/local/appUpgrade/appRunTimeInfoFile.json` contains a 1, else
   `/APPS/channelA`.
4. Near its end `rc.local` runs **`/var/local/otaTest.sh &`** (observed: our
   hook starts at uptime 5.3 s, while `rc.local` is still running).
5. `/usr/local/scripts/start-hisense.sh` (172 lines; its caller was not
   traced) runs `/usr/local/bin/deploy`, `start-security.sh`,
   `start-network.sh`, and starts `superwatch`.
6. `/basic/app_man -c /basic/am_config` starts the applications, the UI
   engine among them. It inherits `HI_UI_HOME`.

## Processes

Parentage from `pstree`, 96 s into a boot.

| Process | Parent | User | Role |
| --- | --- | --- | --- |
| `/application/mi_daemon_sys.bin`, `cpu_audio`, `svcmgr` | init | `mi` | MediaTek middleware: video, audio, tuner |
| `/usr/local/bin/tvmain -D` | init | `UhisTvm` | TV service |
| `/usr/local/bin/systemserver -D` | init | `UhisApm` | system service; holds the IR and keypad input devices |
| `/usr/local/bin/superwatch` | init | root | watchdog; what it watches was not mapped |
| `/basic/VoldService`, `MountService`, `NetworkService`, `SystemService` | init | own accounts | storage and network plumbing |
| **`/basic/app_man`** | init | uid 299 | application manager; parent of everything below |
| **`/opt/hisenseUI/bin/hisenseUI`** | `app_man` | `UhisUI` | the UI engine (Cobalt) |
| `/usr/local/bin/LauncherService` | `app_man` | `UhisLauncher` | native side of the Home screen; role not mapped |
| `/usr/local/bin/websdk` | `app_man` | `UhisWebsdk` | browser file bridge, listens on 9009 and 9888 |
| `hisense_browser` | `app_man` | `UhisBrowser` | Chromium 88, started at boot |
| `netflix` (`GIBBON_MAIN`), `nfxdevsplash` | `app_man` | `netflix` | Netflix, preloaded |
| `remoteservice`, `remoteapp_biz_client` | `app_man` | `UhisRemote` | RemoteNow (phone app) |
| `mosquitto` | `app_man` | `UhisMqtt` | MQTT broker for RemoteNow |
| `dialserver`, `dlna_service`, `airplaydemo`, `airplay_bridge` | `app_man` | own accounts | casting |
| `app_upgrade`, `ota_upgrade` | `app_man` | `UhisApp`, `UhisOta` | updaters |
| `HamService`, `QDA`, `hi_logreport_service`, `avsservice` (through `restart_avs.sh`), `TTSPlayer`, `ai_manager`, `mdnsd` | `app_man` | own accounts | ads/analytics, logging, voice, picture AI, mDNS; not examined |
| `masman`, `mdharvester`, `reggieserver`, `wpa_supplicant`, `tee-supplicant` | init | own accounts | not examined |
| `dropbear`, `otaTest.sh detached` | init (after `setsid`) | root | ours |

About 1.0 GB of 1.2 GB RAM is in use with all of that running.

## Network listeners

`netstat -tlnp`, 96 s into a boot.

| Port | Bound to | Process | LAN can reach it |
| --- | --- | --- | --- |
| 2222 | all | `dropbear` (ours) | yes, by our rule |
| 3366, 3367 | all | `dialserver` | yes |
| 7000 | all | `airplaydemo` | yes |
| 36669 | all | `mosquitto` (TLS, client cert) | yes |
| 38400, 39500 | LAN address | `dlna_service` | yes |
| 9009, 9888 | all | `websdk` | no, firewalled |
| 4445 | IPv6 wildcard | `hisenseUI` (WebDriver) | no, firewalled |
| 4635 | IPv6 wildcard | `airplaydemo` | yes |
| 10001, 36668 | loopback | `mosquitto` | |
| 12321 | loopback | `systemserver` | |
| 2121, 2122, 2123, 3823, 1969 | loopback | `masman`, `tvmain`, `mdharvester`, `tvmain`, `hi_logreport_service` | |

## Firewall

`iptables -S`. Policy is ACCEPT on all chains; `INPUT` holds our rule and one
jump:

```
-A INPUT -p tcp -m tcp --dport 2222 -j ACCEPT        (ours)
-A INPUT -j FIREWALL_IN_EULAON
```

`FIREWALL_IN_EULAON`, in order: accept loopback destinations, packets marked
`0x64`, ICMP and tcp/36669; drop tcp 22, 23, 1025, 4444, 4445, 9222, 9536,
9666, 10001, 10485, 12321; accept a list of service ports (3366, 3367,
4635–4637, 4990, 5000, 5001, 6000, 6030, 7000, 7236, 8085, 8086, 9080, 9226,
12345, 13452, 22321, 26642, 38400, 39500, 44642, and a UDP list); accept
related/established; drop everything else.

So a new LAN-facing port needs one ACCEPT ahead of that jump. The vendor
scripts did not rebuild `INPUT` after uptime 24 s in the boot observed.
`OUTPUT` is not filtered while the EULA is accepted
(`FIREWALL_OUT_EULAON` is defined and unattached).

## Not mapped

- What starts `start-hisense.sh` and `app_man`.
- What `superwatch` supervises and what it does when a process dies.
- `LauncherService`'s role and protocol.
- How `app_man` decides focus and key routing between apps.
