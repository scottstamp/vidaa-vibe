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
then runs `vibe-ui` with no timeout and restarts it up to 10 times. Switches
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

### Way back (written before the change)

- Everything is in memory. A reboot without `debloat.sh` brings every
  service and the stock firewall back.
- `touch /var/local/vibe/debloat.off`: the running script removes
  `VIBE_OUT` within 60 s and exits; services come back at the next reboot.
- Firewall only, at once: `iptables -D OUTPUT -j VIBE_OUT`.
- One service back: take it out of `STOP` (and its uid out of
  `BLOCK_UIDS` if it needs the network), deploy, reboot.
- `rm /var/local/vibe/debloat.sh` removes the feature; `boot.sh` starts it
  only if the file exists.

## Way back

`vibe` and `vibe-model` change nothing persistent themselves. Their only
files are `/var/local/vibe/` (binaries, launcher, last log); remove that
directory to remove them. An input switch is undone by switching back
(`vibe-model --yes switch-input HDMI3`) or with the remote's input key. A running
`vibe` ends on its timeout, on EXIT/BACK, on SIGTERM, or by alarm 10 s
after its timeout.
