# 04 — The UI stack

What draws the stock UI, where its code lives, and what that makes possible.
Measured 2026-11-25 from the root shell, read-only; nothing in the UI was
changed, restarted or navigated. Raw output:
`research/58a51huv/recon/ui-recon-1.out` to `-4.out` (gitignored).

Lines marked **(U9 sibling)** come from the alvidaa project's work on a
50A51HUV and a 55A7KAU, both VIDAA U9. They were not re-checked on this TV
and its bundle is laid out differently.

## The short version

- The whole stock UI is **one web app**: HTML, CSS and JavaScript, 70 MB,
  2,415 files, in `/APPS/channelA/hisenseUI`.
- It runs in **Cobalt 21 LTS**, a restricted browser engine, as the process
  `hisenseUI`.
- The bundle sits on **`/APPS`, which is writable and outside verified boot**.
- The firmware has a **second, empty slot** (`/APPS/channelB/hisenseUI`) and
  a one-character flag that chooses the slot at boot.
- A pristine copy of the bundle sits on the read-only `/opt` partition.
- The engine has a **WebDriver server** listening on loopback.

## The engine

| Fact | Value |
| --- | --- |
| Process | `/opt/hisenseUI/bin/hisenseUI --encoded_image_cache_size_in_bytes=15MB --url=file:///hisenseUI/index.html` |
| User | `UhisUI` (uid 311) |
| Started by | `app_man`, from the `[hisenseUI]` section of `/basic/am_config`: `power_on_start = true`, `wait_for_started = false`, `receive_focus = false`, `silent_mode = true` |
| Engine library | `/opt/hisenseUI/lib/libcobalt.so`, 35 MB, version string `21.lts.7`, built 2023-09-07 |
| Config that can be edited | none: the binary, the library and `am_config` are all on verified read-only partitions |
| URL mapping | `file:///hisenseUI/…` resolves under `$HI_UI_HOME`, which `rc.local` sets and `app_man` passes on. Now `/APPS/channelA` |
| State | `/var/local/hisenseUI/cobalt_home/`: `.starboard.<base64 of the URL>.storage` (121 KB) and `hisense_DB_V1.3.db` (221 KB), owner `UhisUI`, not readable by others |
| Fonts | `/opt/font/Cobalt_fonts/` (`HFMoBeSans`, `Roboto`) |
| Allowed cross-origin hosts | `/opt/hisenseUI/hisenseUI_config.json`, a `cors` list of 13 domains, `vidaahub.com` among them |
| Graphics | DirectFB (`mstar_mi` driver, `sawman` window manager), Mali; holds `/dev/fb0` and `/dev/fb1` open |
| Native reach | links `libapp-manager-client`, `libmodel-rpc-client`, `libchannellist-access`, `libbizAgent` and others; holds `/dev/binder`, 31 `/dev/mik/*` devices (the MediaTek kernel interface: `ir`, `disp`, `osd`, `video`, `audio`, `cec`, `tuner`…), and the fifos `fifo_am_read` / `fifo_am_write` to `app_man` |

`/opt/hisenseUI/startgui.sh` is a second, unused launcher (its line in
`start-hisense.sh` is commented out). It would start the engine with
`--remote_debugging_port=9666 --webdriver_port=4445 --csp_mode=disable --allow_http`.
Those switch names do not appear as strings in `libcobalt.so`; the 747 KB
launcher binary was not searched.

## The bundle

`/APPS/channelA/hisenseUI`, owner `UhisApp`, 70,268 KiB:

| Entry | Size (KiB) | What |
| --- | --- | --- |
| `index.html` | 383 bytes | a webpack shell: `static/css/app.css`, then `static/js/vendor.dll.js`, `vendor-app.js`, `app.js` |
| `static/` | 49,580 | `css`, `fonts`, `img`, `language`, `data`, `presetData`, and 381 files in `js` |
| `modules/` | 17,352 | `Account`, `AccuWeather`, `AirPlay`, `AllInputs`, `AppService`, `Art`, **`Launcher`** (the Home screen, 3,192 KiB), `Notification`, `Search`, `VirtualInput`, `WebApps` |
| `shared/` | 3,280 | two sounds |
| `versionInfo.json` | | `version` `6010223090501`, `versionName` `6.01.02.23.090501`, `otaVersion` `V0000.06.30E.N0823` |
| `process.env.json` | | `BRAND` `HIS`, `AREA` `NA`, `PLAT` `9602`, `BUILD_TARGET` `tv` |
| `var/local/` | | empty cache directories |

The biggest scripts in `static/js` name what the bundle covers: `app.js`
(788 KB), `Setting.js` (263 KB), `HiMedia.js`, `ACR.js`, `VidaaPlus.js`,
`RemoteNow.js`, `Keyboard.js`, and one file each for `LiveTV`, `EPG`, `PVR`,
`FTE` (first-time setup), `MixBar`, `Miracast`, `DLNA`, `USB`, `Voice`,
`StoreMode`, `AppControl`, `Emanual`, `AIPQ`. Settings, live TV, input
switching, the on-screen keyboard and first-time setup are all in here: the
Home screen is one module of eleven.

No manifest of hashes or signature was found in the bundle. Whether anything
verifies the bundle before loading it is **not known**; the alvidaa work found
no engine-side check **(U9 sibling)**.

Other UI data, outside the bundle:

| Path | What |
| --- | --- |
| `/APPS/hisenseUI_shared_data/launcher/` | `allAppsInfo.txt`, `appsSeqInfo.json`, `hiddenApps.json`, `launcherShortCut.json`: the Home screen's app list and order |
| `/APPS/hisenseUI_applications/UI/` | per-feature state: `launcher`, `notification`, `weather`, `art`, ad client id |
| `/APPS/websdk/Appinfo.json` | installed web apps (our `vidaa-home` tile is here) |

## Two slots and a factory copy

`/var/local/appUpgrade/appRunTimeInfoFile.json` (owner `UhisApp`):

```json
{ "version": "1.0.0",
  "0": { "runFlag": "0",
         "pathA": "/APPS/channelA/hisenseUI",
         "pathB": "/APPS/channelB/hisenseUI",
         "backFile4ota": "/opt/hisenseUI/bin/content/data/web/html/hisenseUI.tar.gz",
         "ignoreVersion": "" } }
```

- `rc.local` greps the `runFlag` line for a `1`: found means
  `HI_UI_HOME=/APPS/channelB`, otherwise `/APPS/channelA`. (Read in the
  script, and `HI_UI_HOME=/APPS/channelA` observed in the environment of
  `app_man` and `hisenseUI`.)
- `/APPS/channelB/hisenseUI` exists and is empty.
- `backFile4ota` is a 43 MB tarball of the bundle on verified read-only
  storage: a pristine copy that cannot be damaged.
- At boot `security-dac-partition-apps.sh` runs
  `chown -R UhisApp:UGhisApp` and `chmod 750` on both `/APPS/channelA` and
  `/APPS/channelB`.
- The vendor's `deploy.sh` has an "update hisenseUI after OTA" routine. It
  reads the factory tarball's version and the active slot's
  `versionInfo.json`; if the active slot has no version, or the factory
  version is greater than or equal to it, it **deletes the active slot and
  re-extracts the factory bundle**. What calls that routine, and when, was
  not traced. The slot's directory date (2023-12-23) says it has not run on
  an ordinary boot. **(U9 sibling)** agrees: OTA wipes the channel, normal
  boots do not.

Nothing has been written to either slot, and the flag has not been touched.

## Input

| Fact | Value |
| --- | --- |
| Kernel devices | `event1` "MStar Smart TV IR Receiver" (the remote), `event0` "MTK TV KEYPAD" (the buttons on the set) |
| Who reads them | `systemserver` holds both. `hisenseUI`, `tvmain`, `LauncherService`, `remoteservice`, `app_man` and `systemserver` all hold `/dev/mik/ir` |
| `/dev/uinput` | exists, root only |
| Vendor tools | `/usr/local/bin/inputkeyevent` and `/usr/local/bin/screenshot` exist; not run |
| Inside the page **(U9 sibling)** | keys arrive as key events with `keyCode`: arrows 37–40, ENTER 13, HOME 36, LAUNCHER 547 |
| Injection **(U9 sibling)** | writing to `/dev/input` and `uinput` did not reach the UI there |

How a key press gets from `systemserver` to the page on this TV was not
traced.

## Display

- `fb0` is 1920 wide with a 2160 virtual height, `fb1` 1920 by 3240, both
  32-bit.
- `/etc/directfbrc`: layers 0 and 1 are 960x540 ARGB, layer 2 is 1920x1080
  ARGB. Which layer the UI engine draws on, and at what resolution, was not
  measured.
- The WebDriver status page reports the platform as `DirectFB`.

## WebDriver

The engine listens on tcp/4445 (IPv6 wildcard; `127.0.0.1` works too). The
firewall drops 4445 from the LAN, so it is reachable only from the TV itself.

```
GET /status   -> {"sessionId":null,"status":0,"value":{"build":{"time":"Sep  7 2023 (14:12:04)","version":"21.lts.7"},"os":{"name":"DirectFB; Linux x86_64"}}}
GET /sessions -> {"sessionId":null,"status":0,"value":[]}
```

Only those two read-only requests were sent. No session was created. What a
session can do here (navigate, run script, send keys, screenshot) is
**untested**. On the U9 sibling this port did not listen at all.

The DevTools port 9666 does not listen.

## The native API the bundle uses

**Not mapped.** This is the largest gap.

- The bundle's JavaScript mentions only 15 `Hisense_*` names (the functions
  the TV *browser* exposes), so that is not its main interface.
- `window.Hisense` and `window.VidaaApiService` appear as globals.
- No `h5vcc` (Cobalt's stock extension namespace) reference was found.
- The engine library carries names such as `HisenseService`,
  `VidaaVoiceControl`, `HisenseUpgradeMode`.
- **(U9 sibling)** There the bundle has an `sdk.js` exposing services by
  name: `app-service` (`getCurrentShowApps`, `getAppInfoByAppId`),
  `sdk.apc.start` to launch an app, `all-inputs` (`getAllInputsData`,
  `changeSourceTo`), `setting.open`. This bundle has no `sdk.js`; whether
  the same services exist under `vendor-app.js` is unknown.

## What Cobalt will and will not run

From the alvidaa work **(U9 sibling)**, to be re-checked here:

- No `<canvas>`, no `<iframe>`. SVG and icon fonts did not paint; PNG data
  URIs and plain `div`s did.
- Tags after `</html>` are dropped.
- An XHR that triggers a CORS preflight deadlocked the engine.
- The engine starts about 5 s into boot and the network comes up at 25–35 s.
  A `<script>` that fails to load is not retried.
- Compiled JavaScript is cached by URL: a changed file at the same URL can
  be served stale until the cache directories are cleared.
- A root-owned `.starboard.*.storage` file breaks the engine. Recreating the
  file loses the Home layout.
- `app_man` restarts the engine after a crash, not after a SIGTERM. A
  manual restart has to reuse the engine's captured environment and its own
  user.
- Replacing `index.html` wholesale gave a gray screen there. Replacing one
  route's component inside the running app worked.

Upstream Cobalt documents a restricted subset of HTML, CSS and Web APIs.
Which parts of that apply to this build was not measured.

## Routes this opens

None of these has been tried. Ordered by how little they disturb.

| Route | What it is | Way back | Biggest unknown |
| --- | --- | --- | --- |
| A. Drive the engine | open a WebDriver session from the root shell and point the running engine at our page | restart the engine or the TV | whether a session can navigate, and what a foreign page may call |
| B. Slot B | put a bundle in `/APPS/channelB/hisenseUI`, set `runFlag` to `1`, reboot | set `runFlag` back to `0` over SSH, reboot; slot A is untouched | whether anything besides `rc.local` reads the flag; the cached-script trap |
| C. Component swap in slot A | replace one module (say `modules/Launcher`) and keep the rest stock | restore the file from the factory tarball | this bundle's module boundaries; proven only on U9 |
| D. Our own process | draw with DirectFB beside or instead of the engine | kill it | `am_config` cannot be edited; focus, layers and keys are `app_man`'s. **Tried: a window and keys work, see [06-native-route.md](06-native-route.md)** |

B is the one the firmware was built for, and it leaves the stock UI intact
one flag away. The first test of it that risks nothing visible is an exact
copy of slot A in slot B: if the TV boots into an identical UI from
`channelB`, the mechanism works.

Two things follow from the vendor scripts for any bundle of ours in a slot:
give it a `versionInfo.json` whose `version` is higher than the factory
bundle's, or the OTA routine will replace it; and expect its owner and mode
to be reset at every boot.

Whichever route: SSH does not depend on the UI, so a black screen is
recoverable from the PC.

## Open questions, in the order they block work

1. What is the bundle's interface to native code, and can a page that is not
   the stock bundle call it?
2. Does a WebDriver session work, and can it navigate to a `file://` or
   `https://` page of ours?
3. Does anything other than `rc.local` read `runFlag`? Does `app_upgrade`
   rewrite `appRunTimeInfoFile.json`?
4. How do keys reach the page, and which keys never do (power, volume)?
5. Who draws the volume bar, the input banner and the "no signal" screen:
   the bundle or a native layer?
6. What does `superwatch` do when `hisenseUI` is killed?
7. Which DirectFB layer and resolution does the engine render at?
