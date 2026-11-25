# 06 — The native route: our own DirectFB client

Route D from [04-ui-stack.md](04-ui-stack.md): a native process that draws
and reads keys without the web UI. Measured 2026-11-25 from the root shell,
with the stock UI running. Raw output: `research/58a51huv/recon/native-recon-*.out`
and `vibe-*.out` (gitignored). The code is in [`native/`](../../native/).

## The short version

- Every graphical process on this TV, the stock UI among them, is a
  **DirectFB 1.4.2 client in one shared fusion world** (`/dev/fusion0`,
  session 0). A process of ours can join it as one more client. It gets
  its own window, drawn by the hardware, above or beside the stock UI.
  No framebuffer poking, no killing anything.
- **Keys reach DirectFB windows.** `systemserver` loads the IR, keypad and
  loopback input drivers. A window of ours that asks for focus gets it and
  receives remote keys.
- The vendor's DirectFB is **not ABI-compatible with upstream 1.4.2
  headers**. The interface tables have extra slots in the middle. The
  layout was recovered from the stripped library and is baked into our
  build (`native/tools/mstar_headers.py`). Upstream headers alone crash.
- Done: `vibe hello` showed a window over the stock UI for up to 20 s, got
  focus, received a remote key (EXIT) and exited cleanly; `hisenseUI` kept
  running (same pid before and after).

## The DirectFB world

| Fact | Value | Source |
| --- | --- | --- |
| Library | `/vendor/lib/directfb/libdirectfb-1.4.so.0.2.0`, banner `DirectFB 1.4.2`, MStar build 2023-09-07, ID `3207a83` | strings, runtime banner |
| Fusion | API 8.5, world 0. `/dev/fusion0`..`7` are `root:mi 0660`. The shm pool files `/tmp/fusion.0*` (`/tmp` = `/dev/shm/shm_tmp`) are `root:mi 0660` | `/proc/fusion/0/*`, `ls` |
| Master | fusionee pid 722, which no longer exists; the world outlives it | `/proc/fusion/0/fusionees`, `properties` |
| Members | `systemserver`, `tvmain`, `app_man`, `hisenseUI`, `hisense_browser`, `nfxdevsplash`, `airplaydemo`, `remoteservice` hold `/dev/fusion0` | `/proc/*/fd` |
| Graphics driver | `gfxdrivers/libdirectfb_mstar_mi.so` (the MStar MI OSD), `systems/libdirectfb_devmem.so` | `/proc/*/maps` |
| Window manager | `wm/libdirectfbwm_default.so` in every member. `directfbrc` says `wm = sawman`, but no sawman library is mapped | maps, `/etc/directfbrc` |
| Input drivers | loaded only by `systemserver`: `mstarir`, `mikeypad`, `mstar_linux_input`, `mstar_loopback_input` | maps |
| Netflix | `GIBBON_MAIN` draws with EGL/GLES on Mali without joining fusion. Its splash (`nfxdevsplash`) is a DirectFB client | maps, fds |

What the library reports to a client (`vibe probe`):

| Thing | Value |
| --- | --- |
| Screens | 6, all `MSTAR Screen (FBDev)` |
| Layer 0 | `FBDev Primary Layer`, 1920x1080 ARGB, level 0, options 0x2 |
| Layer 1 | same name and size, level −1 (hidden), options 0x8 |
| Layers 2, 3 | do not exist (`IDNOTFOUND`) |
| Input devices | `MSTARIR Device` (15), `mstarloopback` (16), `MTK TV KEYPAD` (0), and logical keyboard (17), mouse (1), joystick (2) and USB IR (3) devices |

The `directfbrc` layer sizes (960x540 for layers 0 and 1) are not what the
running world uses. The master configured both layers at 1920x1080.

`IDirectFBDisplayLayer::GetWindow(id)` answers OK for every id from 0 to 63,
with the same position, size and opacity. It cannot be used to list other
clients' windows.

## Joining as a client

`vibe` (C, dynamically linked, glibc 2.21 target, 14 KB) needs only
`libdirectfb-1.4.so.0` and libc, and the loader `/lib/ld-linux.so.3`. Before `DirectFBCreate` it:

1. drops to UhisUI's uid, gid and supplementary groups (measured from
   `/proc/<hisenseUI>/status`: 311:311 plus 18 groups, among them `mi`
   3010, `fb` 3013, `event` 3015, `mali` 3016), so nothing it creates is
   root's;
2. sets `session=0` and `force-slave`, so a missing world means waiting,
   never a second master that would reinitialise the hardware;
3. arms `alarm(timeout + 10)` as a last-resort kill.

The library prints a lot to stdout on init: an MStar banner, Utopia/MI init
messages, two failed opens of `/dev/fb2` (errno 19). None of it was fatal.

The environment is `hisenseUI`'s `LD_LIBRARY_PATH` and
`DIRECTFB_SESSION=0` (`native/tv/vibe-run.sh`).

## The vendor ABI

Calling the library through upstream 1.4.2 headers crashed `vibe`, and only
`vibe`. `EnumScreens` actually ran `CreatePalette`. That wrote a palette
pointer over the interface's `priv`, and every later call misbehaved.

How the real layout was recovered (`native/tools/vtables.py`):

- The code is Thumb-2 (disassemble with `--triple=thumbv7`). The library is
  stripped, but each `IDirectFB*_Construct` is exported.
- Each Construct fills the method table with a run of `ldr rX, [pc, #imm]`,
  `add rX, pc`, `str rX, [rThis, #off]`. That gives offset → function
  address.
- The vendor build tags functions with a `"_" + __FUNCTION__` string
  (`_IDirectFBSurface_FillRectangle`). Each function loads its own tag, which
  names it.

Result: every slot of every checked interface was named. Compared with
upstream 1.4.2 (`native/tools/check_abi.py`):

| Interface | Difference from upstream 1.4.2 |
| --- | --- |
| all (`DEFINE_INTERFACE`) | one extra word at offset 8, between `magic` and `AddRef` |
| `IDirectFBSurface` | two unknown slots before `Lock`, one after `Flip`; 15 appended (named ones: `Lock2`, `Lock3`, `Lock4`, `DrawString2`) |
| `IDirectFBDisplayLayer` | two unknown slots before `SetOpacity`; appended `SetRotation`, `GetRotation`, `SetStretchMode`, `SetHVScale`, `ForceFullUpdateWindowStack`, `SetHVMirrorEnable` and one unnamed |
| `IDirectFBWindow` | two unknown slots before `SetProperty`; `SetApplicationID`, `GetApplicationID` and five unnamed appended |
| `IDirectFB`, `Font`, `EventBuffer`, `Screen`, `InputDevice`, `Palette`, `DataBuffer` | only the common word; some appended slots |

With those insertions, all ten interfaces match the library with zero
mismatched slots. The inserted slots' signatures are unknown; the patched
header declares them as `_mstar_*` placeholders that must not be called.

Not measured: the **data structs** (`DFBEvent`, `DFBDisplayLayerConfig`,
descriptions). Values read back are plausible, so the leading fields line
up. But a description passed **by value** to an enumeration callback moved
the `callbackdata` argument: the `ctx` pointer arrived as garbage and
crashed `vibe`. So the vendor `DFBDisplayLayerDescription` is very likely
larger than upstream's. `vibe` gives every buffer the library fills 512
bytes of slack and never uses `callbackdata`.

## The first window

`vibe-run.sh --timeout=20 --layer=0 hello`, owner at the TV:

```
layer 0 is 1920x1080 ARGB
got focus
down symbol=0xf062 id=0xf600 code=174 mods=0x0
vibe: exit 0
```

- A 1440x810 window, `DWSC_UPPER`, double-buffered ARGB, centred on layer 0.
- `RequestFocus` gave it focus (a `DWET_GOTFOCUS` event).
- One key-down arrived: `DIKS_EXIT`, Linux code 174 (`KEY_EXIT`). `vibe`
  closes on EXIT, BACK or ESCAPE, set its window's opacity to 0, destroyed
  it and left the world.
- `hisenseUI` was pid 1206 before and after.

Confirmed by the owner at the TV: the window appeared over the stock UI and
responded to the remote, and afterwards the stock UI worked with the remote
as before.

## What this means for replacing the UI

The pieces a native UI needs exist and work from our side: a window, the
hardware blitter, TrueType text (the library's own FreeType font provider
loading `/opt/font/Cobalt_fonts/*.ttf`) and remote keys. What remains is
how this coexists with the stock UI, then how to start without it:

1. **Focus and key ownership.** Does `app_man` (which holds `fusion0`, and
   has per-app keymaps in `am_config`) move focus back to the stock UI, or
   route some keys (HOME, power, volume) elsewhere first?
2. **Stacking.** Which layer and stacking class the stock UI's window uses.
   Can a window of ours stay above it reliably, and can it hide the stock
   window instead of covering it?
3. **TV functions.** Inputs, volume, channels and settings sit behind
   `tvmain` and `systemserver` (the bundle links `libmodel-rpc-client`,
   `libapp-manager-client`, `libchannellist-access`). Not mapped.
4. **Starting at boot without `hisenseUI`.** `am_config` is read-only, so
   `app_man` will always start the engine. Options: let it start and cover
   or stop it later; or point slot B at a minimal page. To be designed with a
   way back first.
5. **Video.** HDMI is drawn under the OSD by the MI video path, not by the
   UI. Whether a window of ours must punch a transparent hole, as Cobalt's
   would, is not measured.

Scope, from the owner: the tuner (live TV, channels, EPG) is not used, only
the HDMI inputs. Starting at boot waits until the application runs
reliably.

## TV functions: the "kilby" model

Measured 2026-11-25. Raw output: `research/58a51huv/recon/source-recon-*.out`,
`vibe-model-*.out`. The bundle JS and vendor libraries this reads were copied
to `research/58a51huv/pulled/` (gitignored).

### How the stock UI does it

- The bundle calls a global `modeljs`, a binding compiled into
  `libcobalt.so` (`cobalt::hisense::modeljs_atsc::*`). Its namespaces
  (`source`, `channelSearch`, `hotel`, `parentlock`, `appControl`,
  `basicSetting`…) are thin wrappers over a key-value **model** whose keys
  look like `kilby.sl2.<type>.<path>`: `str`, `i32`, `vstr` (string
  vector), `vint32`, and `action` for commands.
- Changing input, in the bundle's `source/index.js`: `changeSourceTo(id)` →
  `source.setCurrentSource(id)` →
  `modeljs.source.sourceSwitch([Date.now(), 9, id])`. 9 is
  `SOURCE_SWITCH_REASON_SWITCH_SOURCE`. In the engine that is the action
  `kilby.sl2.action.source.switch` with three string arguments.
- The input list is `kilby.sl2.vstr.source.input.name`, groups of four:
  `id`, signal (`2` = signal present), display name, `Original`. The current
  input is `kilby.sl2.str.source.input.current`.
- Other keys seen in the bundle that matter for an HDMI-only UI (read in
  JS, not yet exercised): `kilby.sl2.i32.sound.main.volume`,
  `kilby.sl2.i32.sound.main.mute`, `kilby.sl2.vstr.source.input.name`,
  `kilby.sl2.action.source.input.rename`, `kilby.sl2.str.video.hdmi.info`,
  `kilby.sl2.str.sourcedection.hdmilinkstatus`, `kilby.sl2.cec.input.names`.

### Reaching it from native code

The model client is C++: `lomodelapi`, `lomodelrpc::ClientBroker`,
`frame::Process` over the `csp-lmq` transport, built with GCC 5 libstdc++
(cxx11 ABI). `tvmain`, `systemserver` and `hisenseUI` all map
`libmodel-rpc-client.so`. `libcobalt.so` itself only `dlopen`s vendor code.

`/usr/local/lib/libbizAgent.so` exports a **plain C facade** over the
`BizClient` singleton. Signatures recovered from its ARM code (it is ARM,
not Thumb):

| Function | Behaviour |
| --- | --- |
| `void *getInstance(void)` | `new BizClient`; starts the frame process (client name from the executable's basename) |
| `void releaseInstance(void **h)` | deletes it, sets `*h = NULL` |
| `int getIntValue(h, const char *key, int *out)` | `BizClient` vtable +0x20 |
| `int getStringValue(h, key, char *out)` | vtable +0x2c, then **`strcpy`** into `out`: no length |
| `int getVectorStringValue(h, key, char **out, int max)` | vtable +0x34; **`strcpy`** into each `out[i]`, at most `max` elements; returns 0 on success; the element count is not returned |
| `int setIntValue(h, key, const int *v)` | **pointer** to the value (`ldr r2,[r5]`); vtable +0x0c; returns 1 when the inner call returns 0. Works: set volume 9 → 10 |
| `int setStringValue(h, key, const char *)` | vtable +0x10. Not yet exercised |
| `int onAction(h, const char *key, unsigned *invoke_id, const char **argv, int argc)` | string arguments; prints them to stdout |

The handle is ignored by the getters, setters and `onAction` (they use the
singleton). `getStringValue` returned 1 on success, `getVectorStringValue` 0,
`onAction` 1.

`native/src/vibe-model.c` `dlopen`s the facade (so our binary stays plain C)
and runs as UhisUI's identity like `vibe`. Results:

```
current: 'HDMI3'
TV signal=0 'ANT/CABLE' | AVS signal=0 'AV' | HDMI1 0 | HDMI2 0 | HDMI3 signal=2
--yes switch-input HDMI2 -> r=1; current: 'HDMI2'; HDMI2 now signal=2
--yes switch-input HDMI3 -> r=1; current: 'HDMI3'
```

The owner watched the picture switch to HDMI2 and back on screen.
`hisenseUI` kept running (pid 1206 throughout). HDMI2 reported no signal
before the switch and a signal after, consistent with its device waking up
(by CEC, presumably; not checked).

Not yet known: whether the model enforces permissions by client name or
uid for other keys; whether anything else (the stock UI's own source
banner, CEC) reacts to a switch made this way; the volume path.

## Remote keys with our window focused

`vibe keys` (2026-11-25, 90 s, owner pressing keys): the same inset window
as `hello`, plus a global input event buffer (`CreateInputEventBuffer`,
`DICAPS_KEYS`, global) that sees every key DirectFB receives. Raw log:
`research/58a51huv/recon/vibe-keys-1.out`.

- **Every key pressed reached our window**, and the window never lost
  focus: arrows, OK, BACK, EXIT, MENU, HOME, VOLUME UP/DOWN, MUTE,
  CHANNEL UP/DOWN, digits, transport keys, and keys DirectFB has no name
  for. HOME did not take focus away.
- All keys came from input device 3 (`/dev/input/logic_usbir_xxxxxx`), not
  from the IR receiver (device 15). The remote in use reaches the TV through
  that logical device.
- **Some keys arrive twice.** BACK, and the unnamed `0xf070` and `0xf069`,
  each produced a second key-down/up from device 15 (`MSTARIR Device`) with
  `key_code` −1, right after the original. Something re-injects them. The
  app has to drop a repeat whose code is −1.
- **Volume and mute are handled by the system as well.** The model read
  volume 10, unmuted, before the test and volume 9, **muted**, after it, while
  our window held focus. So volume does not depend on the stock UI having
  focus.
- **The stock UI reacted to keys too.** The owner saw its native overlays
  (input picker, menu, home) open **above** our window. Keys pressed twice
  in the log are the owner closing those overlays again. So the stock UI gets
  keys whichever window has focus (not by DirectFB window focus), and its
  overlays stack above our `DWSC_UPPER` window.

| Key | DirectFB symbol | key_code |
| --- | --- | --- |
| UP / DOWN / LEFT / RIGHT | `0xf002` / `0xf003` / `0xf000` / `0xf001` | 103 / 108 / 105 / 106 |
| OK | `0xf00b` (`DIKS_OK`) | 352 |
| BACK | `0xf048` | 158 (+ a −1 duplicate) |
| EXIT | `0xf062` | 174 |
| MENU | `0xf012` | 139 |
| HOME | `0xf005` | 102 |
| VOLUME UP / DOWN | `0xf04c` / `0xf04d` | 115 / 114 |
| MUTE | `0xf04e` | 113 |
| CHANNEL UP / DOWN | `0xf046` / `0xf047` | 402 / 403 |
| `DIKS_TV` | `0xf027` | 377 |
| PLAY/PAUSE, STOP, REWIND, FAST FORWARD | `0xf050`, `0xf052`, `0xf059`, `0xf05a` | 164, 128, 168, 208 |
| digits 0–9 | `0x30`–`0x39` | 11, 2–10 |
| INPUT | `0xf068` | 616 |
| CC (closed captions) | `0xf069` | 617 (+ a −1 duplicate) |
| MTS (audio track) | `0xf070` | 624 (+ a −1 duplicate) |
| microphone (hold for voice) | `0xf0e5` | 741 |
| unlabelled shortcut key | `0xf0e8` | 744 |
| dash (`-`) | `0xf093` | 52 |

Names of the unnamed keys from the owner, by press order. The bundle's own key map (`KEYMAP` in `app.js`) uses different
numbers (VOLUME_UP 411, LAUNCHER 547, KEYPAD_INPUT 610, ALLAPP 615), so
Cobalt translates DirectFB keys before the page sees them.

## Stopping the stock UI: preparation

Read-only, 2026-11-25 (`research/58a51huv/recon/stopui-recon-1.out`).
`vibe-ui` showed its panel on HOME for a moment, then the stock launcher
opened above it. The stock UI acts on HOME itself whatever window has focus.
`am_config` has `input_event_source = inject`: `app_man` routes keys itself.

What stopping `hisenseUI` should and should not set off:

- `hisenseUI`'s section has `silent_mode = true`, and `app_man` contains
  the branch "process is silent mode, no need LaunchDefaultAp". Read in its
  strings, not observed.
- `app_man` also has "default process %s stopped, reboot" and "check
  default process after %s stop". The default process is looked up with
  `GAM_GetDefaultProcess`. Which process that is was not determined. The
  candidate is `dtv_app_mtk` (`/basic/dtv_svc`, `id = MTK_DTV`), which is
  configured but not in `application_context` and not running. If
  `hisenseUI` is the default, expect a reboot. That is harmless here: it is
  the way back anyway.
- `superwatch` is not a process watchdog. It is a command helper (logs,
  drop caches, fstrim, `reboot` on command) listening on the fifo
  `/var/run/superwatch`.
- **(U9 sibling)** `app_man` restarted the engine after a crash, not after
  SIGTERM.

The engine's context, for the record: `/opt/hisenseUI/bin/hisenseUI
--encoded_image_cache_size_in_bytes=15MB --url=file:///hisenseUI/index.html`,
cwd `/basic`, uid/gid 311 with 18 groups. It has effective capabilities
`0xa05000` (`NET_ADMIN`, `IPC_LOCK`, `SYS_ADMIN`, `SYS_NICE`) given by
`app_man` (uid 299, all capabilities), and SELinux context
`user_u:object_r:unconfined_process`.

**Way back, written before the first stop:** `reboot` over SSH. The boot
hook brings SSH back and `app_man` starts the stock UI as on every boot.
Starting the engine by hand is not the way back: it would lack the four
capabilities `app_man` gives it, and it must never be started as root.

## Stopping the stock UI: first attempt

2026-11-25, owner at the TV (`research/58a51huv/recon/stopui-test-1.out`):
`vibe-ui` started, then `kill -TERM` to `hisenseUI` (pid 1206).

- **`app_man` restarted it within 2 s** (new pid 13733, same parent, uid
  and capabilities). No reboot, nothing in `/proc/monitor/crashinfo`.
  `vibe-ui` kept running and was stopped cleanly afterwards. This differs
  from the U9 sibling, where SIGTERM was not followed by a restart.
- So killing the engine does not remove it. The supported way is
  `app_man`'s client API in `/usr/local/lib/libapp-manager-client.so`
  (`hs::apmclient::AppManagerClient`): `stopApp`, `startApp`,
  `suspendApp`/`resumeApp`, `requestFocus`/`releaseFocus`, `registerApp`
  (with an `AppInterface` callback object), `sendDfbKey`, `keyEventResp`,
  `getAppStatus`. It is C++ (GCC 5 cxx11 `std::string`,
  `boost::shared_ptr`). The vendor binaries say
  `GCC: (Linaro GCC 5.5-2017.10) 5.5.0`, which is the toolchain to match.
  `app_man` also knows a `stop_restart_list` (not mapped).

## app_man as a client: GAM

2026-11-25. Raw output: `research/58a51huv/recon/vibe-gam-info-1.out`,
`stopui-test-2.out`.

- `AppManagerClient::stopApp` in `libapp-manager-client.so` is the
  **app's** side: `app_man` calls it to stop an app that registered with it
  ("%s is not in m_appIntfMap"). It cannot be used to stop another app.
- MediaTek's own client library, `/basic/libgam_client.so`, is plain C over
  Android binder (`/dev/binder`, group `mi`). Prototypes come from its
  `__PRETTY_FUNCTION__` strings: `GAM_ClientInit(GAM_APPCTRL_HANDLER_t*,
  const char *name)`, `GAM_SendCmd(GAM_CLIENT_SEND_CMD_T*)`,
  `GAM_SendRawMsg(const char*)`, `GAM_GetDefaultProcess/GetFocusProcess/
  GetActiveProcess(char*)`, `GAM_LoadProcessList`,
  `GAM_GetProcessListCount/Element`, `GAM_GetProcessState(const char*,
  int*)`, `GAM_SetDisableHotkey/SetEnableHotkey(unsigned*, int)`,
  `GAM_GetInputEventWait(void*)`, `GAM_InjectInputEvent(void*, int)`.
- The vendor's sample native UI, **dockui** (`/3rd/bin/launcher.bin`,
  `[ dockui ]` in `am_config` with `window_ontop = true`, `keymap = true`;
  not started at boot), shows how the structs are used:
  `GAM_ClientInit(&{onSync(type,data,len), onAsync(type,data,len),
  "deprecated", "-", 1}, "dockui")`, then
  `GAM_SendCmd(&{src "dockui", dst "am", 0, cmd "started", para "dockui"})`.
  It takes its keys from `GAM_GetInputEventWait` and draws with DirectFB.
- `native/src/vibe-gam.c` does the same `ClientInit` as "vibe". It works
  as UhisUI's identity:
  - **default process: `apm_bond`** (so stopping `hisenseUI` does not hit
    "default process stopped, reboot");
  - focus and active process: `NotFound` (with the stock UI up);
  - 45 processes listed, state 2 for running ones, 4 for stopped, 10 for
    `netflix` and `QDA` (probably background).
- `GAM_SendCmd({vibe → am: stop=hisenseUI})` returned 0, and `app_man`
  **did stop `hisenseUI`**, but a new one was running within 2 s (pid
  13733 → 15048). It was relaunched again.
- `apmbond` (the default process, `hs::apmbond`) relaunches the engine:
  `RemoteAppWrapper::notifyAppEventMessage` runs `hisenseUI` "silent" on the
  event message `start hisenseUI` (beside handlers for Netflix, YouTube,
  DIAL, CEC on/off, low memory). Who sends that event after a stop was not
  traced. `app_man` itself also has "in stop command will start %s" and
  "Start self after stop" settings.

## Slot B: a blank stock UI

Plan (2026-11-25, owner approved): keep `hisenseUI` running, since
`app_man`/`apmbond` relaunch it, but boot it from slot B with an empty,
transparent page. Its home screen, overlays and key handling are the
bundle's JavaScript, so they disappear with it. Mechanics, read
2026-11-25 (`research/58a51huv/recon/slotb-recon-1.out`):

- `rc.local`: `grep runFlag appRunTimeInfoFile.json | grep 1` → non-empty
  means `HI_UI_HOME=/APPS/channelB`. The file is
  `/var/local/appUpgrade/appRunTimeInfoFile.json`, `UhisApp:UGhisApp
  0640`, md5 `d5338bffc0faf7dc8431d06032b683ec` with `"runFlag": "0"`.
- `deploy.sh` `updateHisenseUIForOTA`: for the active slot, if its
  `versionInfo.json` has no version, or the factory tarball's version
  (`6010223090501`) is `-ge` it, the slot is deleted and the factory bundle
  re-extracted. Our slot carries version `9000000000000`. When the routine
  runs is still not traced (OTA only, on the U9 sibling).
- `/usr/local/bin/app_upgrade` (cloud UI updates) also reads `runFlag`. If
  it installs a stock UI and flips the flag, the stock UI returns. That is
  not destructive.
- The stock `app.css` sets no background on `html`/`body`. The blank page
  sets `background: transparent` on both. **(U9 sibling)** a wholesale
  `index.html` replacement there gave a gray screen.
- The engine's storage file is keyed by URL, which stays
  `file:///hisenseUI/index.html`. The blank page does not use storage, so
  the stock Home layout in it is left alone.

Done 2026-11-25: slot B staged with `native/slotb/install.sh`
(`SLOTB_STEP=stage`), then flipped (`SLOTB_STEP=flip`). `runFlag` is now
`"1"`, owner and mode are unchanged, the new md5 is
`8d16f20c0e506fa4d6a338a659e94812`, and the backup at
`/var/local/vibe/backup/appRunTimeInfoFile.json.orig` matches the original md5.

**First boot from slot B** (owner power-cycled, 2026-11-25):
- The owner saw the HDMI picture with no Hisense UI, and no remote key
  brought up any Hisense element. No gray screen.
- From the shell at uptime 55 s: `hisenseUI` (pid 1160) had
  `/APPS/channelB/hisenseUI/index.html` open, and its pid was unchanged 6 s
  later, so no restart loop. Slot B's files matched the staged md5s, and
  `verifiedbootstate=green`.
- Memory in use: about 485 MB, against about 1.0 GB with the stock UI.
- `vibe-ui` then started and got focus. Owner test: see below.

**`vibe-ui` as the only UI** (owner test, 2026-11-25): our input panel
works, input switching works, and all buttons reach `vibe-ui`. Volume keys
showed our bar but **the volume no longer changed**: with the stock page gone,
nothing acted on them. (Before, the stock UI's `modeljs.sound.setMainVolume`
did it; the system does not act on volume keys by itself.) The fix is to set
the model value: `setIntValue("kilby.sl2.i32.sound.main.volume", &v)` took
effect (9 → 10, read back 10). `kilby.sl2.i32.sound.arcdev.exist` is 0: no
ARC audio device, so the TV speakers are in use. `vibe-ui` now sets volume
and mute itself.

**Way back, written before the flag is flipped:**
1. Over SSH: `cat /var/local/vibe/backup/appRunTimeInfoFile.json.orig >
   /var/local/appUpgrade/appRunTimeInfoFile.json` (in place, so owner and
   mode stay `UhisApp` `0640`). Check the md5 above.
2. Reboot. `HI_UI_HOME` is `/APPS/channelA` again; slot A was never touched.
3. Slot B's files can stay. Remove them with
   `rm -rf /APPS/channelB/hisenseUI/*` if wanted.
If the TV came up without SSH, which is not expected because SSH does not
depend on the UI, the hook's recovery in [02-root-access.md](02-root-access.md)
applies.

## Starting vibe-ui at boot

Plan (2026-11-25). The boot hook gets one guarded block, inserted just
before its last line, after dropbear and its watchdog are already started:

```sh
# vidaa-vibe: start the native UI supervisor if installed and not switched off
if [ -f /var/local/vibe/boot.sh ] && [ ! -f /var/local/vibe/off ]; then
  $BB setsid /bin/sh /var/local/vibe/boot.sh </dev/null >/dev/null 2>&1 &
  echo "vibe: boot.sh started"
fi
```

`native/tv/boot.sh` waits for `hisenseUI` (so the DirectFB world exists),
then runs `vibe-ui` with no timeout and restarts it up to 10 times (since
2026-11-25: forever, backing off 3 s, 6 s, … up to 60 s, reset after a run of
10 minutes; the previous run's log is kept as `last-vibe-ui.prev.log`). Switches
it checks:

| File | Effect | Who can create it |
| --- | --- | --- |
| `/var/local/vibe/off` | `vibe-ui` not started (the hook also skips `boot.sh`) | SSH |
| `/var/local/vibe-off` | `vibe-ui` not started | SSH, or the browser bridge (`/var/local` is world-writable) |
| `/var/local/vibe-stock` | restores the original `appRunTimeInfoFile.json` (stock UI from slot A at the next boot) and does not start `vibe-ui` | SSH, or the browser bridge |

The patch is applied by `native/tv/hook-patch.sh`: in place, with
`otaTest.prev.bak` first rewritten to the hook as it was, and `sh -n` run
before and after.

Applied 2026-11-25 (`HOOK_STEP=apply`): the hook is now 5,445 bytes, md5
`ae7649020d9d98c189ae26e5a143a2ec`, and still `UhisWebsdk`.
`otaTest.prev.bak` = the previous hook, md5 `9a6b8a0c5714950642e17ec275f31774`
(also kept as `/var/local/vibe/hook/otaTest.before`). `sh -n` passed on the
installed file. Not yet through a boot.

**Way back, written before the hook is touched:**
- Over SSH: `HOOK_STEP=revert sh /var/local/vibe/hook-patch.sh` (writes
  `otaTest.prev.bak` back over the hook, in place), or simply
  `touch /var/local/vibe/off`.
- Without SSH: the browser bridge can write `/var/local/vibe-off` or
  `/var/local/vibe-stock`. Its no-op button still disarms the whole hook, as
  before.
- SSH does not depend on the block: it starts before it, and the block only
  starts a background shell.

## First boot with vibe-ui started by the hook

2026-11-25, owner power-cycled after switching to HDMI2 with `vibe-ui`:
- `boot.sh` ran at uptime 8.2 s, `hisenseUI` was up at 8.4 s, and `vibe-ui`
  started at 13.4 s. SSH was fine, and the hook log shows "vibe: boot.sh
  started" before "--- main done". Verified boot green.
- **The TV restored the last input (HDMI2) by itself**: no UI code is needed
  for that.
- **POWER did nothing.** `vibe-ui` received it (`0xf00f`, code 116, i.e.
  `DIKS_POWER`) and ignored it. As with volume, the stock UI's JavaScript
  used to act on it: `modeljs.system.SwitchOffTv([1])` (in the MixBar
  "Power Off" item and the voice handlers). In `libcobalt.so`,
  `cobalt::hisense::modeljs_atsc::System::SwitchOffTv` uses the model key
  **`kilby.action.system.switch.off.tv`**. There is no `sl2` there, unlike
  most keys. Next to it in the same family are
  `kilby.sl2.action.system.factory.reset` and `...clear.user.data`, so
  action names are never guessed.
- **Tried, and stranded the display:** `vibe-ui` sent
  `switch.off.tv ["1"]` on POWER. The TV went into standby, but **Linux
  kept running** (SSH up, uptime unbroken, `vibe-ui` still alive): a fast
  standby. `pwr.standby.mode` = 1, `enum.screen.state` = 0
  (`SCREEN_OFF`). Every later POWER press still reached `vibe-ui`, which
  sent power-off again. With `vibe-ui` stopped, POWER **did not wake the
  display**, so the stock page's JavaScript or something it drives does
  the wake-up. Setting `kilby.sl2.i32.system.enum.screen.state` to 1 returned
  1 but read back 0. The engine knows only two `kilby.action.*` keys
  (`system.switch.off.tv`, `system.notify.clean.msg`): there is no "switch on"
  action. Recovery was a mains power cycle. POWER handling in `vibe-ui` is
  disabled again until the wake path is mapped.

## Incident: emptied runtime file (2026-11-25)

Going back to slot A for the stock-power test, `SLOTB_STEP=unflip` copied
`/var/local/vibe/backup/appRunTimeInfoFile.json.orig` over the runtime file.
The backup had been **0 bytes** since one of the earlier mains power cuts:
it was created and never synced. `unflip` did not check its md5, so
`appRunTimeInfoFile.json` became 0 bytes. (`rc.local` reads an empty file
as slot A, so a boot would still have worked, but `app_upgrade`/`deploy`
read it too.)

Repaired within minutes: the original was rebuilt byte for byte from the
recon output (276 bytes, md5 `d5338bffc0faf7dc8431d06032b683ec`, verified
before writing), written in place (owner `UhisApp`, mode 0640 kept) and
synced. The backup was restored the same way. Every other file written that
day was intact: hook (`ae76…`), `prev.bak` (`9a6b…`), slot B, binaries.
Files rewritten in place survived the cuts; only newly created files were
lost. That is ext4 delayed allocation: truncate-and-rewrite gets an early
flush, new files do not.

Changes: `install.sh`, `hook-patch.sh` and `boot.sh` now `sync` after every
write and check a backup's md5 (or size) before restoring from it.

## How the stock UI does standby (observed)

2026-11-25: back on slot A, `vibe-ui` off, `native/tv/powermon.sh` running.
Owner pressed POWER, waited 30 s, pressed POWER (raw:
`research/58a51huv/recon/powermon-stock-1.out`).

- **Stock standby is also a fast standby.** Uptime ran on (41 → 102 s) with
  wall clock in step, and every process stayed up. `enum.screen.state` went
  1 → 0 at about 60 s and back to 1 at about 87 s. `pwr.standby.mode` stayed 1.
- On wake the kernel logged "Panel Driver STR resume Done" and the panel
  power-on timings (86.8 s).
- No JS in the bundle handles the POWER key itself: the key goes to the
  native side (`app_man` → `apm_bond`). The JS only listens to STR
  messages from a native `guiService.addSTRListener`
  (`suspending`/`suspend`/`resuming`/`resume`) and sends nothing back.
- The stock SDK ends its init with `system.uiLoadFinish(1)` →
  `modeljs.system.setGuiLoopFinished(1)` → model key
  **`kilby.sl2.i32.system.gui.loop.finished`** (read in `libcobalt`). It is
  1 with the stock UI up. A blank slot B page never sets it. The working
  guess: the power service treats the UI as not loaded and does not resume.
  `vibe-ui` now sets it to 1 at start.

## Rebooting from SSH

`sync; /bin/reboot` (`/bin/reboot` → `/basic/mtk_reboot`: honours
`/tmp/no_reboot`, then calls `reboot()`) restarts the TV with the same effect
as a mains cycle for everything here: `rc.local` re-reads the slot flag, the
hook starts SSH and `boot.sh`. It is gentler on the flash than cutting mains.
On 2026-11-25 SSH and `vibe-ui` were back as on a cold boot. Afterwards the
wake reasons list held an `rc5` (remote) entry, but the owner pressed nothing:
the TV came back by itself.
`/usr/local/bin/reboot` (Hisense: panel and backlight off, writes a "reboot
flag", used by the factory-reset script) was not used.

With `vibe-ui` setting it, `gui.loop.finished` read 0 before and 1 after,
which confirms that a blank page leaves it unset.

**POWER works with `gui.loop.finished = 1`** (2026-11-25, slot B, `vibe-ui`,
owner test, raw `research/58a51huv/recon/powermon-vibe-1.out`): standby
(screen 1 → 0 at uptime 346 s) and wake (→ 1 at 366 s), both from the
remote, with Linux and every process up throughout, exactly as on stock.
`vibe-ui` **did not receive the POWER key** at all. Once the UI reports
itself loaded, the system takes POWER before any window does. Our app does
nothing for power; it only sets the flag at start.

## Sleep timer: what happens at expiry

Test (2026-11-25): Sleep timer set to 15 min from vibe-ui at uptime 157.9 s
(`timerfunctions.hotelmode.sleep.timer` = 15; the stock UI uses the same
key, `modeljs.timerfunc.setHotelmodeSleepTimer`). The TV did not turn off.
Syslog: at 997.9 s (one minute early) `tvmain`'s timer service logged
`notifyGuiMessage Value '2'` and set
`kilby.sl2.i32.timerfunctions.suppress.key.global.action` = 1; afterwards
the timer read 0, `timerfunctions.switch.off.notify` 2, and the suppress
flag stayed 1. Nothing powered off: `tvmain` only tells the UI.

In the stock bundle the UI does the switch-off: its "Mask" (closing
curtain) fades the volume to 0 over the curtain's duration and then calls
`modeljs.system.SwitchOffTv([4])`, i.e. `kilby.action.system.switch.off.tv`
— the action that stranded the display earlier (above, while
`gui.loop.finished` was still 0). The voice handler uses a different route
for "power off": `appC.sendAppMessage("TvsApm",
'{"action":"poweroff",...}')`, a message to the app manager in
`systemserver`. Not mapped yet: how `appC.sendAppMessage` reaches it, and
what the suppress flag suppresses (the stock UI presumably clears it; with
it left at 1, the remote, POWER and volume kept working, owner).

`switch.off.tv` again, now with `gui.loop.finished` = 1 (2026-11-25):
`vibe-model --yes action kilby.action.system.switch.off.tv 4` at uptime
1509.7 s; `enum.screen.state` 1 → 0 by 1511 s; the owner pressed POWER and
the TV woke (screen 1 at 1523 s, panel "STR resume Done" at 1519.7 s),
`vibe-ui` still running. So the earlier strand was the missing
`gui.loop.finished`, not the action.

vibe-ui now does what the stock UI did, with its own deadline (tvmain's
`switch.off.notify` stays 2 and the suppress flag stays 1 after use, so
they cannot mark the next expiry): setting the timer in Sleep timer arms
`now + minutes`; a timer found set at start counts from start. In the last
minute a toast counts down ("Turning off in N s · any key cancels"); any key
but POWER cancels (timer key set to 0, "Sleep timer off"); at 0 it sends
`switch.off.tv` with reason 4 (the stock sleep-timer reason) and sets the
timer key to 0. Standby (`enum.screen.state` 0) before then cancels it, so
the action is never sent to a TV already off. Tested with the timer key set
to 2 and `vibe-ui` restarted: run 1 unattended, "last minute", "switching
off", `switch.off.tv [4] -> r=1`, screen off, POWER woke it; run 2, a key
in the last minute cancelled it and the key read 0 (owner saw the
countdown). (`arg[0]=4` in the log is printed by `libbizAgent.so` itself.)

## Custom input names

Rename → input → "Custom name…" opens an on-screen keyboard (digits, a–z
with one-shot Shift, `' - . &`, Space, Delete, Done; the remote's digit keys
type digits; BACK cancels). Up to 15 characters, the longest preset's
length. Done with nothing typed restores the input's own id. The field
starts with the current name if it is neither the id nor a preset. Uses the
same `source.input.rename` action as the presets.

## The UI, and registering with app_man

2026-11-25. `vibe-ui` grew a Home screen (Inputs and Settings rows), a
Picture panel and an input rename list. The owner tested all of them and
they work. Keys and actions were taken from `libcobalt.so` with
`native/tools/modeljs_keys.py`:

| What | Model key or action | Arguments, values |
| --- | --- | --- |
| picture mode (read) | `kilby.sl2.i32.video.enum.picture.adjustment` | 0 Dynamic, 1 Standard, 2 Cinema Night, 3 Cinema Day, 4 Game, 5 Sport, 6 Calibrated (11 IMAX) |
| picture mode (set) | action `kilby.sl2.action.video.set.picture.adjustment` | `[mode, "1", "1"]` (stock `SetPictureAdjustment([e,1,1])`) |
| backlight, brightness, contrast | `kilby.sl2.i32.video.backlight` / `.brightness` / `.contrast` | 0..100 |
| colour, sharpness | `kilby.sl2.i32.video.colour.intensity` / `.sharpness` | 0..100 |
| rename an input | action `kilby.sl2.action.source.input.rename` | `[sourceId, newName]` |

Registering with `app_man` (GAM, `native/src/gam.c`, `vibe-ui --gam` and
`--gam-name=`):
- As "vibe" and as "dockui" (a name in `am_config`), `GAM_ClientInit` and
  `GAM_SendCmd({am: started})` both returned 0. **Nothing else changed:**
  neither name entered `app_man`'s process list (still the 45 apps of
  `application_context`), focus and active process stayed `NotFound`, and
  no callbacks arrived. `app_man` tracks only processes it started itself,
  and `am_config` is read-only.
- While connected, POWER also reached `vibe-ui`'s window. Standby and wake
  still worked (owner test).
- Registration is therefore **off** (empty `/var/local/vibe/ui.args`): it
  adds a half-known client and nothing we need. Keys and focus come through
  the DirectFB window, POWER through the system, and the app shortcut keys
  do nothing (owner).

## Drawing the UI

2026-11-25. DirectFB here fills only hard-edged rectangles. `vibe-ui` locks
its window surface (`DSLF_WRITE`), draws shapes in software (`gfx.c`:
anti-aliased rounded rectangles, gradients, glows; ARGB not premultiplied),
unlocks, then draws text with DirectFB. Labels are centred with the font's
ascender and descender. The owner saw the result on the panel; it looks
right.

- A full redraw (Home: full-screen scrim, six cards, glow) takes 160–180 ms
  on this CPU (first frame about 490 ms). Fine for key-driven redraws, too
  slow for animation.
- Reading the window surface back for a screenshot: `Lock(DSLF_READ)`
  returned a buffer of zeros (not even DirectFB's own text);
  `Lock(DSLF_READ | DSLF_WRITE)` returned the drawn frame.
- The panel shows translucent fills lighter than their alpha suggests (in
  a photo the desktop showed through 0xc8 cards strongly), partly camera and
  partly unmeasured. Fills were made more opaque (cards 0xeb, panels 0xf5,
  Home scrim 0xc0→0xf2).

## Starting before the stock engine (2026-11-25, to be tested at boot)

The DirectFB world's shared-memory file `/dev/shm/shm_tmp/fusion.0` is
created about 6 s into boot (its mtime against boot time), before `boot.sh`
starts (~8 s) and before `hisenseUI` (8.1–9.9 s on the boots measured).
`boot.sh` now waits for that file instead of `hisenseUI`. `debloat.sh` stops
the stock UI pieces only once `hisenseUI` exists (or after 60 s of uptime),
so an engine that starts after `vibe-ui` is still stopped; and it kills
them on later passes instead of freezing them (a frozen engine would keep
its window). `vibe-ui` takes focus back if the engine's window takes it.
Unknown until a boot: whether the engine starting after us disturbs the
layer or our window.

Way back: `touch /var/local/vibe/wait-engine` brings back the old wait for
`hisenseUI`. It only matters at boot, so it takes effect at the next
reboot.

First boot with it (2026-11-25): `boot.sh` 8.67 s (a late boot), world
found 8.99 s, `vibe-ui` 9.55 s; `hisenseUI` was already up (pid seen in the
same line), so no gain this time; the engine and the other stock UI pieces
were stopped at 18.3 s as before, `websdk` at 12.2 s. HDMI3 locked 2.2 s after
start, under the notice's 3 s, so no false "No signal". On boots where
the engine comes later the gain is up to ~1.8 s; not yet measured.

## More settings: HDMI format, picture reset, no-signal standby

From the stock bundle and `libcobalt.so` (research agent, checked by hand
where it matters), 2026-11-25:

| Setting | Stock JS call | Model | Values | Shown when |
| --- | --- | --- | --- | --- |
| HDMI format (Game page) | `video.setHdmiFormat` → `modeljs.video.setHdmiSignalFormat` | `kilby.sl2.i32.video.enum.hdmi.signal.format` | 0 Standard, 1 Enhanced, 2 Auto (this TV: 1) | `video.4k.mode.supported` = 1; the stock UI also greys it out unless an HDMI input is showing: one value, not per input |
| Reset picture mode (Picture page) | `picture.resetDefaultPictureSettings` → `modeljs.video.ResetDefaultPictureSettings([1])` | action `kilby.sl2.action.video.reset.picture.settings` `["1"]` | — | always; the stock UI asks to confirm. Not `...reset.picture.settings.shopmode.onTimer` (store mode) |
| No-signal standby (Sleep page) | `system.setAutoStandbyNoSignal` → `modeljs.system.setAutoStandby` | `kilby.sl2.i32.system.nosignal.autostandby` | 0 Off, 1 "15 minutes" (this TV: 1) | `system.display.panel.type` = 0 (this TV: 0) |

Like the sleep timer, no-signal standby is counted by the stock UI, not by
`tvmain`: 14 minutes without signal on panel type 0 (`noSignalFunTime`,
LiveTV.js), then the standby dialog (60 s), then `SwitchOffTv`. The stock
UI's switch-off reasons (`app.js` `MODELJS_DEFINE`): `SWITCH_OFF_TV_TIMER`
2 for the sleep timer, `SWITCH_OFF_TV_AUTO` 3 for no-signal and auto sleep,
`SWITCH_OFF_TV_CURTAIN` 4. vibe-ui's sleep timer had used 4 (it worked);
it now uses 2, and no-signal standby 3.

vibe-ui: no-signal standby uses the "No signal" tracking (current input,
not the tuner): after 14 minutes it starts the same last-minute countdown
as the sleep timer ("No signal: off in N s"); a key restarts the 14
minutes ("Staying on"), a signal or another input cancels it. Picture reset
asks for a second OK within 4 s. Settings rows shrink to fit (Picture has 7
now). Auto sleep (`timerfunctions.auto.sleep`, 3 h / 4 h, also counted by
the UI) is not offered. Not yet tested on the TV: the no-signal countdown
end to end, the picture reset, an HDMI format change.

## Earlier than app_man: can services be kept from starting? (research)

2026-11-25, a read-only investigation (five search angles over the boot
scripts, `app_man`, the other early daemons, the loader and writable
symlinks, kernel/hotplug; each candidate checked for timing and for
effect/safety). Raw result: `research/58a51huv/recon/early-hooks-workflow.json`
(gitignored).

Timing, corrected: `/proc/boottime` runs about 2.5 s ahead of
`/proc/uptime`. On the uptime clock the hook (`otaTest.sh`) starts at
5.26 s (`/var/local/alvidaa-hook.log`: "stage0 5.26"), dropbear at about
5.8 s; our vibe-ui block sits at the hook's end and runs at about 8.6 s.
`app_man` creates `/tmp/fifo_am_read` (and `/dev/shm/shm_tmp/fusion.0`
appears in the same second) about 7 s after boot (ctime against `btime`,
whole seconds); per the disassembly it reads `/basic/am_config` once, after
its DirectFB setup and that fifo. So `app_man` very likely masters the
DirectFB world `vibe-ui` joins: it must keep starting.

No writable file that runs before the hook was found within the rules:
`/config/*_profile` (run by rcS as root) and `Customization3RDFunc.ini`
(per-service `bEnabled=false` in `app_man`) are on mmcblk0p26 (read-only,
would need a remount); the vendor path that writes there goes through
`/var/local/oem` (never touch). FactoryOpenState lives in the bootloader
environment (excluded). A library in `/var/local/lib` for `apmbond` loads
after the hook and a failure reboots the TV. `/tmp/UpgradeProgressOn`
stalls `apmbond`. Stand-ins that exit 2 change nothing (no `start_self`;
the respawns come from systemserver's keep-alive). `hotelmode.ini` and
`HI_UI_HOME` do not help.

What does work, in principle (untested): our hook already runs before both
config reads, so it can bind-mount edited copies of two read-only config
files (a bind mount over a file; the verified partition is not remounted
or written):

1. `/basic/am_config`, read by `app_man` at about 6–7 s: `power_on_start =
   false` for the unused services stops `app_man`'s own boot starts
   (Netflix, ota_upgrade, app_upgrade, hi_logreport_service, resmon,
   HiAvsService, mosquitto, ai_manager, vidaa_ai_service, AirPlayBridge,
   masman, mdharvester, reggieserver; TTSPlayer and dialserver may be
   started regardless, per the disassembly). `apm_bond` and `hisenseUI`
   must stay. A malformed copy makes `app_man` exit ("PM init failed"): no
   DirectFB world, no `vibe-ui` that boot (SSH unaffected). The bind must
   come early in the hook (~5.3 s), not in `boot.sh` (~8.7 s).
2. `/usr/local/opt/etc/data/apm/application-config-us.ini`, read by
   systemserver's app manager at about 10.06 s: its `[auto_start_4k]`
   list starts the browser (~11 s), remoteservice, remoteapp_biz_client,
   dlna_service, avsservice (~21.7 s). Its `[hotkey]` section must stay
   byte-identical (`tvmain` reads it on wake). Read once.

Not explained: what starts QDA and Netflix_Splash. Both files are read once,
so a mount after the read changes nothing until the next boot.

### Lever 1 in practice: `early.sh` (way back written before the change)

Files: `native/tv/early-prepare.sh` builds `/var/local/vibe/am_config.trim`
on the TV from `/basic/am_config` (15 `power_on_start = true` lines set to
false: AirPlayBridge, ota_upgrade, hi_logreport_service, resmon,
app_upgrade, HiAvsService, mosquitto, dialserver, ai_manager,
vidaa_ai_service, TTSPlayer, netflix, masman, mdharvester, reggieserver;
nothing else changes; checked byte for byte against an independent build)
and records both md5s in `am_config.md5`. `websdk` stays on (the bridge,
the hook's disarm of last resort on a stock boot; `debloat.sh` stops it
later on vibe boots). Per the review, TTSPlayer and dialserver seem to be
started by `app_man` regardless, and resmon and HiAvsService have no binary
here: about 11 of the 15 lines matter. Re-running `early-prepare.sh` moves
`early.on` to `early.on.untested`, so a new copy is first tested with
`early.arm`.

`native/tv/early.sh` is started in the background by two lines in the
hook's first stage (marked `vidaa-vibe-early`, ~5.3 s). It bind-mounts the
copy over `/basic/am_config` before `app_man` parses it (~6.5 s). It acts
only when armed: `/var/local/vibe/early.arm` (one boot; deleted before
mounting) or `/var/local/vibe/early.on` (every boot); never with an off
switch (`vibe/off`, `vibe-off`, `vibe-stock`, `vibe/debloat.off`), never
without `boot.sh`, `debloat.sh` and `vibe-ui` (nothing could confirm),
never if either md5 differs (e.g. after a firmware update).

Crash guard: before mounting it writes `early.pending` and fsyncs it (and
its directory; ~10 ms, measured), because `app_man` reboots without a sync
if `apm_bond` stops. `debloat.sh` deletes the marker once `vibe-ui` runs
and `hisenseUI` has appeared while the overlay is mounted. Any boot that
finds the marker treats the last overlaid boot as failed: it renames
`early.on` to `early.on.failed`, deletes `early.arm`, records it in
`early.history` and mounts nothing. So a bad overlay costs one boot with
either switch, heals without SSH, and stays off until re-armed by hand.
Nothing is written to a verified partition; the bind mount ends at reboot.

Checked without a reboot (2026-11-25, `app_man` not running):
`/bin/mount --bind` of the copy over `/basic/am_config` worked (rc 0;
`/proc/mounts`: `/dev/mmcblk0p39 /basic/am_config ext4 rw,...`; the file
read as the copy), `/bin/umount` restored the original md5. The change was
reviewed adversarially before going into the hook (four lenses, skeptic
per finding); fixes: the marker before the mount, disarm on failure, the
marker checked before anything else, confirm only while mounted, and
`hook-patch.sh` early steps that check the old hook + 2 lines round trip,
the `start_db` call, and verify the write (restoring on mismatch).

First boot with it (2026-11-25, `early.arm`, owner present). Hook lines
applied with `early-apply` (hook md5 ae76… → b8c1…, owner kept,
`otaTest.prev.bak` 9a6b… untouched). `early.log`: started 5.41 s, marker on
disk 5.54 s, overlaid 5.56 s (hook stage0 5.36 s). `debloat.sh`'s first
pass (8.56 s) found only TTSPlayer and dialserver (started by `app_man`
regardless, as the review said) and `websdk` (kept on); Netflix and its
splash, ota_upgrade, app_upgrade, hi_logreport_service, mosquitto,
ai_manager, vidaa_ai_service, AirPlay, masman, mdharvester and
reggieserver never started. Confirmed at 11.69 s (`hisenseUI` started,
`early.history`). The boot also ran earlier: `boot.sh` 7.93 s, `vibe-ui`
8.02 s, stock UI pieces stopped 14.23 s (previous boots 8.7–9.0 /
9.5–9.9 / 18.3 s; one boot, less CPU contention is the likely reason).
The browser still started (systemserver's autostart, lever 2) and was
stopped at 22.7 s. SSH up, MemAvailable 899 MB.

Then (owner checked the UI): `early.on` set, so the overlay applies every
boot. Lever 2 (systemserver's autostart list) is not used: with `app_man`
stopped at ~14 s, the services it lists for ~21.7 s are never started
anyway, which leaves only the browser (~11 s, about 12 s of Chromium after
`vibe-ui` is up), not worth touching a file that also holds the power-key
map. Instead `debloat.sh` now passes every 2 s up to 40 s of uptime, so the
browser is stopped within about 2 s of starting.

Way back:
- Stop using it: `rm /var/local/vibe/early.on /var/local/vibe/early.arm`
  (next boot; the current boot's mount is harmless, `app_man` read the
  file once).
- Take the hook lines out: `HOOK_STEP=early-revert sh
  /var/local/vibe/hook-patch.sh` (in place, keeps the UhisWebsdk owner,
  checks the result and restores on mismatch; `otaTest.prev.bak` is not
  involved). Our copy of the hook before the change:
  `/var/local/vibe/hook/otaTest.before-early`.
- Remove everything: also `rm /var/local/vibe/early.sh
  /var/local/vibe/am_config.trim /var/local/vibe/am_config.md5
  /var/local/vibe/early.on.failed /var/local/vibe/early.on.untested`.

## To revisit: unused remote buttons

The shortcut button next to MENU, MTS, CC, the mic button, the dash key and
the app buttons (Netflix and others) all reach `vibe-ui` (logged as
`key 0x…`) and do nothing. The owner will decide what they should do
through use (2026-11-25); nothing is assigned until then.

## Drawing speed

The keyboard felt slow (owner). Measured with `VIBE_TIMING=1`: 400 ms a
frame, of which 370 ms shapes. Three causes, fixed:

- The locked DirectFB surface is display memory, slow to read, and every
  blended pixel reads it. The same frame drawn by `native/tools/gfx_bench.c`
  in ordinary RAM took ~60 ms. vibe-ui now draws shapes into a RAM buffer
  and copies it to the surface row by row (writes only).
- `gfx_shadow` drew one rounded outline per pixel of spread (22–28 passes
  over each edge band); now one pass by distance, same falloff.
- `over()` did five divisions per blended pixel; now one (exact /255 by
  shift-add).

Also, `poll_inputs` compared input lists with `memcmp` over an unzeroed
stack buffer, so every poll (1 s while a menu is open) looked like a change
and forced a full redraw; keys waited behind it. Now zeroed.

Result: keyboard frame ~112 ms (shapes 71, text 14, flip ~25 with
`DSFLIP_WAITFORSYNC`), down from ~400 ms, and no redraws without a cause.

Partial frames, for focus moves, typing and settings value changes
(`dirty == 2`): the RAM buffer keeps the whole last frame; a measuring pass
with an empty clip (`struct canvas` has an optional clip box now) records
the new focus rectangle (`focus_fill`, the focused settings row, the
keyboard's text field when typing); only old focus ∪ new focus is drawn
again, and the surface gets that plus the previous frame's region (the
window is double buffered). Text is drawn with the surface clip set to the
same area, so it is never blended twice. A list that scrolls, Shift on the
keyboard, or anything else redraws in full. Measured: keyboard focus move
15–31 ms, typing ~65 ms (field and key in one bounding box), Home and
settings moves similar; full frames ~105–115 ms. Screenshots after a series
of partial frames (the dev "shot" is now a partial frame too) showed no
leftover focus glow and no doubled text.

## More settings, and volume with a soundbar

2026-11-25. New pages, with keys taken from `libcobalt.so`
(`tools/modeljs_keys.py`):

| Page | Row | Key | Values |
| --- | --- | --- | --- |
| Sound | Output | `kilby.sl2.i32.sound.speaker.arc.mode` | 1 TV speakers, 2 ARC, 3 optical (0 off, 4 Bluetooth, 5 WiSA not offered) |
| Sound | eARC | `kilby.sl2.i32.sound.earc.switch.value` | 0/1 |
| Sound | Sound mode | `kilby.sl2.i32.sound.enum.adjust` | 0 Standard, 1 Speech, 2 Late Night, 3 Music, 4 Film, 5 Sport |
| Game | Game mode | `kilby.sl2.i32.video.game.mode.enable` | 0/1 |
| Game | Low latency | `kilby.sl2.i32.video.enum.instant.game.response` | 0 off, 1 on, 2 auto (shown if `...instant.game.response.supported` is 1). ALLM's own methods do not exist in this build; the bundle tags this one `PICTURE_GAME_ALLM_ENABLE` |
| Sleep timer | Turn off after | `kilby.sl2.i32.timerfunctions.hotelmode.sleep.timer` | minutes: 0, 15, 30, 60, 90, 120 (stock list) |

Device present on ARC: `kilby.sl2.i32.sound.arcdev.exist` (1 with the
owner's eARC soundbar connected). The TV switched its output to ARC by
itself when the soundbar was plugged in.

**Volume with the soundbar** (owner and shell, 2026-11-25):
- The system forwards the remote's VOLUME and MUTE keys to the soundbar
  over CEC by itself, and `kilby.sl2.i32.sound.main.volume` then mirrors
  the level the soundbar reports. This soundbar moves 3 per press.
- Writing `main.volume` in ARC mode does **not** move the soundbar: the
  model echoes the written value (9 → 10) and the soundbar stays where it
  is (owner, by ear). The stock UI's comment agrees: "SetVolume do not
  work in ARC mode".
- So in ARC mode (`speaker.arc.mode` = 2 and `arcdev.exist` = 1)
  `vibe-ui` writes nothing for volume or mute. It shows the bar and polls
  every 0.1 s for 1.5 s after a key. The owner confirmed: no flicker, mute
  and unmute work. With TV speakers it sets the values itself, as before.

## Stopping unused services, and keeping off Hisense's servers

The owner's choice (2026-11-25): stop every stock service this TV does not
use (casting, AirPlay, RemoteNow, voice, Netflix and browser preloads,
updaters, log and usage reporting, ACR), keep the internet for third-party
services, and keep the TV off Hisense's servers with updates fully off.

The model's own switches were already off when read:
`system.upgrade.autofirmwareupgrade` 0, `system.upgrade.systemappautoupgrade`
0, `acr.switch` 0 (all `kilby.sl2.i32.`). We do not rely on them alone.

`native/tv/debloat.sh`, started once by `boot.sh`:

- stops the processes in its `STOP` list and stops them again if they come
  back (every 5 s for the first 2 minutes of uptime, every 60 s after);
- adds the iptables chain `VIBE_OUT` at the top of `OUTPUT`. It refuses DNS
  queries for any name under `vidaahub`, `hismarttv`, `hisense`,
  `hisense-usa`, `hisenseusa`, `vidaa` and `juhaokan`, matched on the DNS
  wire format, so subdomains built at run time are covered. It also refuses
  non-loopback traffic from the updater and reporter uids (304, 305, 306,
  312, 326, 337, 338, 339, 1032). Kernel matches `string` and `owner` exist
  (`/proc/net/ip_tables_matches`); `iptables` is 1.8.4 legacy.

Measured with the script started by hand at uptime about 83 minutes:

- Killed and stayed dead: Netflix and its splash, the browser, QDA,
  TTSPlayer, AirPlay, `ota_upgrade`, `hi_logreport_service`, `mosquitto`,
  RemoteNow, `dlna_service`, `mdnsd`, `mdharvester`, `masman`,
  `reggieserver`, `vidaa_ai_service`. `app_upgrade` came back once, then
  stayed dead.
- `app_man` respawns `restart_avs.sh` (and with it `avsservice`),
  `dialserver` and `ai_manager` within about a second, every time. The
  script freezes those with SIGSTOP the second time it sees them; frozen,
  they stay alive, are not respawned, and use no CPU.
- MemAvailable went from 642 MB to 792 MB.
- In the first minute, `VIBE_OUT` refused 68 DNS queries under `vidaahub`,
  32 under `hismarttv` and 16 under `hisense`. Third-party names still
  resolve (`github.com` through 1.1.1.1).
- The first version forked an `awk` per (name, process) pair, about 7,500
  forks a pass. One pass took over a minute. It now reads `/proc/<pid>/status`
  with the shell builtin `read`.
- Bash 3.2 cannot parse `case … in *.sh) …` inside `$( )`; write `(*.sh)`.

First boot with the script started by `boot.sh`: `boot.sh` at 7.98 s,
`debloat.sh` at 13.48 s, `VIBE_OUT` in place at 14.30 s, all services
stopped or frozen by 23.9 s, MemAvailable 855 MB right after boot. The 5.5 s
between `boot.sh` and `debloat.sh` was `boot.sh` grepping every
`/proc/*/cmdline` for an already-running copy; it now uses a marker,
`/tmp/vibe-debloat.started` (tmpfs, gone at reboot). `ota_upgrade` and
`mosquitto` ignore SIGTERM; the SIGKILL follow-up missed them because
`name_of` overwrote the loop variable `k` (fixed), so they were frozen
instead. Standby and wake, twice, with the frozen processes: worked, and
nothing came back (owner, 2026-11-25). Owner also checked keys, inputs,
volume and settings.

Second boot, with the marker: `boot.sh` 7.84 s, `debloat.sh` 7.87 s,
`hisenseUI` seen 8.09 s, `vibe-ui` started 8.10 s (was 13.6 s, and about
12.5 s with the old 5 s sleep). Its model read took 2.8 s at that point of
the boot (0.6 s on an idle TV), so keys work from about 11 s. The SIGKILL
follow-up now catches `ota_upgrade`, `mosquitto` and Netflix. `VIBE_OUT` was
hooked at 9.2 s and again at 15.3 s: something in the stock boot flushed
`OUTPUT` in between (not identified), and the next pass put it back. So the
block has a gap of up to one pass (5 s) early in boot.

Hostnames came from the binaries and caches (`grep -a` over `/usr/local`,
`/basic`, `/opt`, `/3rd`, `/var/local`): `api.hismarttv.com` and
`domain.vidaahub.com` in `app_upgrade`, `geo-bas` and
`vidaa-base-auth(-eu).vidaahub.com` in `tvmain`, `time-bas.vidaahub.com` and
`na.hismarttv.com` in `LauncherService`, `iot-voice`/`vidaa-voice` in
`avsservice`, and about 40 `*.vidaahub.com` names in the browser's caches.
`domain.vidaahub.com` looks like a service directory, so names can be built
at run time; hence matching whole domains, not a hosts file. (`/etc/hosts` is
a link to `/tmp/hosts`, writable, if a hosts file is ever wanted.)

Known side effect, not yet observed: the TV's clock may come from
`time-bas.vidaahub.com`; with it blocked, the clock may drift or be unset.
Nothing in vibe-ui shows the time, and the sleep timer counts minutes.

### app_man in the key path?

Test (2026-11-25): `app_man` (pid 724) frozen with SIGSTOP for 20 s
(uptime 98.6–118.6 s) while the owner pressed HOME, UP, DOWN and BACK.
Every key reached `vibe-ui` and the menu reacted normally (owner). So keys go
from `systemserver` through DirectFB to the focused window without `app_man`,
despite its `input_event_source = inject` setting. Second test: `app_man` frozen
for 40 s (uptime 200–240 s) while the owner pressed POWER, waited, pressed
POWER again. `system.enum.screen.state` went 1 → 0 at about 212 s and back
to 1 at about 227 s; dmesg shows the panel's STR resume at 224.3 s; the owner
saw standby and wake work. So standby and wake do not need `app_man` either
(`vibe-ui` leaves POWER to the system, with `gui.loop.finished` = 1). Script:
`research/scratch/amstop.sh`, log `/var/local/vibe/amstop.log`.

Third test, the kill (`research/scratch/amkill.sh`), uptime 327–347 s: SIGTERM
to `app_man` ended it alone; its children (`apmbond`, `hisenseUI`, `websdk`,
`LauncherService`) lived on, reparented. SIGTERM ended `apmbond`, `hisenseUI`
and `LauncherService` too; the frozen `avsservice` took SIGKILL. Nothing
came back over the next minutes. `vibe-ui`, `tvmain`, `systemserver` and
`websdk` kept running. MemAvailable 897 MB (it was 642 MB with everything
stock running). The owner then checked HOME, settings, input switching,
volume (the soundbar went 9 → 15 → 9) and standby and wake: all worked.

So `debloat.sh` now stops these four (`STOP_UI`) once `vibe-ui` runs, with
`app_man` first, and then kills anything still frozen. `boot.sh` waits for
`hisenseUI` only on its first run of a boot (marker
`/tmp/vibe-ui.started`), so `restart-ui.sh` does not wait for an engine that
is gone. Slot B stays as it is: with `debloat.off` the blank engine runs again
and still draws nothing.

### CEC and the soundbar after boot (same with stock services)

The owner saw CEC respond only about 30 s after the picture: the soundbar
ignores volume and switching to HDMI2 does not wake the Google TV box until
then. `native/tv/cecmon.sh` (started by `boot.sh` while
`/var/local/vibe/cecmon.on` exists; read-only) sampled `cec.functionality`,
`cec.arc.activemode`, `sound.arcdev.exist`, `sound.speaker.arc.mode` and
`sound.main.volume`, and kept the kernel's CEC/ARC lines:

| | ours (debloat on) | stock services (`debloat.off`) |
| --- | --- | --- |
| kernel `Set ARC mode: 0->1`, eARC mode | 10.3 s | 10.7 s |
| `speaker.arc.mode` 1 → 2 | 26.9 s | between 22.7 and 30.4 s |
| soundbar's audio formats registered, `arcdev.exist` = 1 | 31.3 s | 31.9 s |

`cec.functionality` is 1 from 8 s. So the 20 s wait is the TV's eARC/CEC
handshake with the soundbar, done by `tvmain` and the MI layer; our changes
do not cause it, and the owner found it, if anything, slower with stock
services. Until the soundbar is found the TV treats its speakers as the
output: volume keys change the TV speaker volume (vibe-ui wrote 9 → 11 then).

The TV queues CEC key presses while the link is down or busy. On the
stock-services boot the owner pressed VOL+ three times (soundbar 12 → 18),
then VOL- about 20 times over 20 s with no effect, then the soundbar went
18 → 21 → 25 → 21 → 18 → 15 → 12: queued presses delivered late (owner's
reading). vibe-ui sends nothing to the soundbar in ARC mode, so this is the
system's CEC path. Not seen on boots with debloat on.

Why it takes 20 s: the syslog `/var/log/log.txt` (and `.1`, 512 KB each,
`television` owner) has `tvmain`'s CEC trace with timestamps (`hscec.cpp`,
threads `CecMonitor`, `EarcMonitor`, `CEC message thr`, `CEC ping thread`).
On the boot of 2026-11-25 09:37 (debloat on):

- 9.9 s `performStarting`, 10.2 s `setEArcSwitchValue(1)`, 11.0 s
  "current mode is wait eARC established". The soundbar (logical address 5,
  "Philips B4000", on HDMI1 = the ARC port) answers at once and from 16.4 s
  sends `<Initiate ARC>` every ~0.6 s.
- The TV queues every reply (`<Report ARC Initiated>` 0xc1, `<Set System
  Audio Mode>`, …) in one send queue, but its send thread is suspended on
  every HDMI hotplug ("HOTPLUG EVENT_Begin detected so suspend cec msg
  thread"). `cecsetHotPlug` runs per input, staggered: port 1 at 13.6 s,
  port 2 at 20.9 s, port 3 at 25.9 s, with suspend/resume bursts around
  each. Sends ran at 0–1 per second against about 5 received; the queue
  grew from 12 (13 s) to 138 (28 s).
- 28.3 s, right after the last hotplug burst: `handleCECAudioDeviceNotify`
  connect=1 active=1, `currentEARCStatus = 0`, ArcMode 2, ARC unmuted. eARC
  never established; the link is plain ARC.

So the delay is the staggered hotplug plus a CEC send queue flooded by the
soundbar's retries, behind an eARC wait that this soundbar never satisfies.
The queue also explains the owner's late, replayed volume presses. Test with eARC off (`sound.earc.switch.value` read back 0, set by the owner
in vibe-ui's Sound page), next boot: no change. `arcdev.exist` became 1
between 26.6 and 31.1 s, as before, and at 34.9 s the send queue still held
123 messages, with the owner's volume presses (`<User Control Pressed>`
0x44, `<User Control Released>` 0x45) queued behind them. So the eARC wait
is not the cause; the staggered per-port hotplug and the flooded send queue
are, and both are inside `tvmain` (verified partition, not changeable).
After a power cut or a reboot, expect about 30 s before CEC works. The
syslog rotated past the boot lines by 35 s on that boot: CEC traffic fills
both 512 KB files fast, so read it early.

Who sends the volume keys to the soundbar: `systemserver` itself. Its IR
reader (`bspirkey.cpp`) hands each press to `key-event-dispatcher.cpp`,
which asks `apm.cpp:isCECHotKey()` (true when `m_iCecState` 1,
`m_iARCExits` 1, `m_iArcMode` 2) and then "Post hot key to cec": `tvmain`
queues `<User Control Pressed>` 0x44 / `<Released>` 0x45. This happens
before and apart from the DirectFB event vibe-ui gets, so vibe-ui cannot
hold presses back once the soundbar is found. Before it is found the check
is false: nothing is forwarded, and vibe-ui used to change the TV speakers'
volume instead.

vibe-ui now remembers that a soundbar is normally connected
(`/var/local/vibe/state/soundbar`, "1"/"0", owned by UhisUI; `boot.sh`
creates the directory). It is set to 1 when the output is the soundbar, to 0
when a soundbar is present but another output is chosen, or after a minute
with no ARC device once past 120 s of uptime. While it is 1 and the
soundbar is not connected, VOL+/VOL-/MUTE are dropped with a "Sound system
connecting…" notice. For 45 s after a connection made in the first 150 s of
uptime, presses go through (they must) with a "Sound system starting, volume
may lag" notice.

Boot test (2026-11-25): "soundbar expected: 1 (remembered)" at start; two
VOL+ presses before the link were dropped; soundbar found at 29.8 s; about
ten presses between 31 and 65 s showed nothing at first, then the soundbar
replayed them from 67.7 s to 74.4 s (9 → 12 → 15 → 12 → 9 → 12 → 15 → 12 →
9). The 45 s notice window (to 74.8 s) matched the replay.

From standby the soundbar connects at once and responds within a couple of
seconds of wake (owner), so this only matters after a cold boot. Decision
(owner): leave `tvmain` alone; replacing it, even if possible, risks a
state we cannot recover from.

### Who wiped the block, and the move to the nat table

The stock firewall is four scripts in `/usr/local/scripts`, all on the
verified partition, all filter-table only:

- `security-firewall-start.sh` (run by `start-security.sh` at boot):
  `iptables -F`, `iptables -X`, recreates the four `FIREWALL_*` chains,
  hooks `INPUT` (and `OUTPUT` when the EULA is off), then runs
  `firewall-init-subrule.sh` in the background.
- `firewall-eulaon.sh` / `firewall-eulaoff.sh` (run by `tvmain`, and by
  `euladpp-reset.sh`): `iptables -F OUTPUT`, `iptables -F INPUT`, re-add the
  `FIREWALL_IN_*` jump. They take `/tmp/firewall.lock` and log to syslog with
  tag `firewall`.

Any of them wipes a filter-table `VIBE_OUT` hook (seen once at about 15 s
of boot), and the `INPUT` flush also drops the hook's SSH accept rule, which
the hook puts back within 15–30 s (`otaTest.sh`'s loop; open SSH sessions
survive through the `ESTABLISHED` accept in `FIREWALL_IN_*`).

None of them touches the nat table, so `VIBE_OUT` now lives there: nat
`OUTPUT` → `VIBE_OUT`, which REDIRECTs the matching DNS queries (udp/tcp 53,
same wire-format label match) and all non-loopback tcp/udp of the blocked
uids to local port 9, where nothing listens, so they fail at once. Tested
live: `api.hismarttv.com` and `geo-bas.vidaahub.com` fail in under a second,
`github.com` resolves; after `iptables -F OUTPUT` (what `firewall-eulaon.sh`
does to OUTPUT) the block still holds. The nat table was empty before, the
TV has no global IPv6 address, REDIRECT is in
`/proc/net/ip_tables_targets`. Boot test (2026-11-25): `debloat.sh`
at 8.0 s, nat `VIBE_OUT` hooked at 10.9 s and never again (no wipe); `tvmain`
ran its EULA firewall setup at 9.7–9.8 s ("set firewall by eula" in
syslog), before our hook. Filter `INPUT` held the SSH rule. Still not tested:
the uid rules with a live process (the blocked services are dead within
13 s). Building the chain took about 3 s at boot (33 `iptables` runs). It is now
one `iptables-restore --noflush` commit, chain and hook together (the TV's
`iptables` is multi-call: `exec -a iptables-restore`); 0.08 s from script
start on an idle TV. At boot (2026-11-25): `debloat.sh` at 9.27 s, chain
hooked at 9.71 s (0.44 s; was 2.9 s). That boot ran about a second later
overall (`boot.sh` 8.49 s, `hisenseUI` seen 9.86 s, `vibe-ui` 9.95 s);
boot-to-boot spread so far is about 1 s.

### Way back (written before the change)

- Everything is in memory. A reboot without `debloat.sh` brings every
  service and the stock firewall back.
- `touch /var/local/vibe/debloat.off`: the running script removes
  `VIBE_OUT` within 60 s and exits; services come back at the next reboot.
- Firewall only, at once: `iptables -t nat -D OUTPUT -j VIBE_OUT` (since the
  move to the nat table; the first version used the filter table).
- One service back: take it out of `STOP` (and its uid out of
  `BLOCK_UIDS` if it needs the network), deploy, reboot.
- The stock UI's pieces (`app_man`, `apmbond`, `hisenseUI`,
  `LauncherService`) come back with everything else: `debloat.off`, reboot.
- `rm /var/local/vibe/debloat.sh` removes the feature; `boot.sh` starts it
  only if the file exists.

## Way back

`vibe` and `vibe-model` change nothing persistent themselves. Their only
files are `/var/local/vibe/` (binaries, launcher, last log); remove that
directory to remove them. An input switch is undone by switching back
(`vibe-model --yes switch-input HDMI3`) or with the remote's input key. A running
`vibe` ends on its timeout, on EXIT/BACK, on SIGTERM, or by alarm 10 s
after its timeout.
