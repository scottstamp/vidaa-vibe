# 07 — Jellyfin client

The goal: a native Jellyfin client in vibe-ui, controlled with the remote.
Started 2026-11-25 overnight, read-only on the TV (no reboots, nothing on
screen). Raw output: `research/jellyfin/` (gitignored). Code: `native/src/jf.c`,
`http.c`, `vibe-jf.c`.

## Short version

- **jellyfin-desktop cannot be ported.** It is Qt 6, QtWebEngine
  (Chromium) and mpv. The TV has none of them, glibc 2.21 is too old for
  them, and there is no X11/Wayland or usable GL compositor, only DirectFB 1.4.
  Its value is jellyfin-web plus mpv, so we take the API and the device
  profile ideas from it and the other official clients, and write the rest.
- **The TV can decode nearly everything in hardware.** The vendor
  GStreamer in `/opt/mmlib` has Hisense sink elements that feed the
  hardware decoder with encoded streams: H.264, HEVC, VP9, AV1, MPEG-2 and
  MPEG-4 video; AAC, AC-3, E-AC-3, DTS, FLAC, Opus, Vorbis and MP3 audio.
  The demuxers for MKV, MP4, TS, HLS and DASH are there too. Most libraries
  should **direct play**, with no server transcoding.
- **Networking works from the TV.** libcurl 7.x is on the TV. Both of the
  owner's Jellyfin servers answer from the TV: "storage" at
  192.168.2.8:8096 and "fin" at 192.168.2.31:8096, both 10.11.11 with Quick
  Connect on. Discovery works too, with a workaround (below).
- **Built and tested tonight:** `jf.c` covers discovery, Quick Connect,
  password sign-in, browsing, playback negotiation and progress reports.
  `vibe-jf` is a command-line client for jf.c. Tested on the TV: discovery,
  the server check, and Quick Connect up to the code. The steps after
  sign-in need the owner to approve a code.
- **Playback path, from reverse engineering only:** Hisense's `HMP*` API
  in `libhmplayer.so`, used the way the stock browser uses it for URL
  playback. `vibe-play` implements it. It loads and resolves every symbol
  on the TV as the UI user (`--probe`). It has never played anything:
  that needs the screen, so the first run needs the owner.
- **Built:**
  - the panel in vibe-ui: a Jellyfin card on Home leads to server
    choice, Quick Connect, Home (continue watching, next up, libraries),
    lists, item detail, and a player with an on-screen display;
  - `jfui_test`, which drives the panel without DirectFB. Over SSH it got
    as far as the Quick Connect code.

## What the TV has

| Piece | Where | Notes |
| --- | --- | --- |
| SoC | MediaTek MT9602 (MStar lineage, `utopia`, `mi_*`) | Jenkins paths in the vendor libraries say `9602EUU6` |
| GStreamer 1.16 | `/opt/mmlib` (libs, `bin/gst-launch-1.0`, `gst-inspect-1.0`, `gst-play-1.0`), plugins in `/opt/mmlib/gstreamer-1.0` | full base, good and bad sets, plus Hisense elements |
| Hardware sinks | `libgstessink.so`: `videoessink`, `audioessink`, `subtitleessink` | properties `backendId` ("browser uva backend id"), `writeDataFunc`, `async`, `sync`, `render-delay` (2.2 s default) |
| Hisense player | `/usr/local/lib/libhmplayer.so` (`HMPInit`/`HMPCreate`/`HMPSetURI`/`HMPOpen`/`HMPPrepare`/...); below it `/opt/mmlib/libgsthsplayer.so` and `libesplayerplugin.so` (`HS_Esplayer*`) | used by the Opera browser's media backend `/opt/webengine/opera_dir/uva_backends/libhisense-uva-backend.so` |
| Older MStar MM SDK | `/vendor/lib/utopia/libmmsdk*.so`, `libmmp.so`, `libstagefrighthw.so` | not used here, it seems; noted only |
| FFmpeg | `libavformat` 58.45, `libavcodec` 58.91 (`/usr/local/lib`, `/opt/lib`) | tvmain maps them; software only |
| libcurl | `/usr/local/lib/libcurl.so.4` (7.x) | needs `/basic/lib` for `libssl.so.1.0.0`; vibe-ui's `LD_LIBRARY_PATH` already has it |
| JSON, images | `libcJSON.so`, libpng, libjpeg on the TV | we build in our own copies (cJSON, stb_image) so the PC build works too |
| Fonts | `/opt/font/Cobalt_fonts/Roboto-*.ttf` | already used by vibe-ui |
| RAM | 1.2 GB; ~200 MB free with everything running | posters must be decoded small and cached with care |

Encoded caps accepted by `videoessink` (gst-inspect, measured):
`video/x-h264`, `video/x-h265`, `video/x-vp8`, `video/x-vp9`, `video/x-av1`,
`video/mpeg` (1/2/4), `video/x-divx`, `video/x-xvid`, `video/x-wmv`,
`video/x-cavs`, `video/x-gst-fourcc-avs2`, RealVideo, FLV, and others.
`audioessink`: `audio/mpeg` (MP1/2/3, AAC raw/ADTS/LATM), `audio/x-ac3`,
`audio/x-eac3`, `audio/x-ac4`, `audio/x-dts`, `audio/x-flac`,
`audio/x-opus`, `audio/x-vorbis`, `audio/x-wma`, PCM, A-law/mu-law,
`audio/x-gst-fourcc-mhm1` (MPEG-H). **No TrueHD**: such files get the audio
transcoded, with the video copied.

How to reproduce (read-only, shows nothing):

```sh
export LD_LIBRARY_PATH=/opt/mmlib:/usr/local/lib:/vendor/lib:/vendor/lib/utopia:/basic/lib
export GST_PLUGIN_PATH=/opt/mmlib/gstreamer-1.0 GST_REGISTRY=/tmp/gstreg.bin
/opt/mmlib/bin/gst-inspect-1.0 videoessink
```

## Network notes

- **Discovery** is a UDP "who is JellyfinServer?" to port 7359. From the PC
  it answers to a broadcast. From the TV a broadcast gets **no answer**: the
  vendor firewall's `FIREWALL_IN_EULAON` chain drops incoming UDP that
  conntrack cannot tie to something sent, and an answer to a broadcast is
  not tied. `jf_discover` therefore also sends the probe to every address
  of the TV's own /24 (254 packets, once, when asked). Those answers are
  ESTABLISHED and pass. Measured: both servers found.
- `debloat.sh`'s nat block only covers Hisense DNS names and uids, so the
  Jellyfin traffic is untouched.
- **Quick Connect** (`POST /QuickConnect/Initiate`, then poll `GET
  /QuickConnect/Connect?secret=`, then `POST /Users/AuthenticateWithQuickConnect`)
  returned a code from the TV. One pending request was made while testing
  and expired unused.

## Open questions (for the owner; the choice made is in bold)

1. **Native client or jellyfin-web in the TV's Opera engine?** The Opera
   engine is still on disk and plays HTML5 video through HMP, which would
   bring the whole official web client. But it needs the stock browser
   stack we stop at boot, it is heavy on memory, and it would compete with
   vibe-ui for keys. **Chosen: native**, as asked.
2. **Which server?** Two answer: "storage" (192.168.2.8) and "fin"
   (192.168.2.31). **Chosen: the app lists what discovery finds and you
   pick one at first sign-in.** Tonight's tests used "fin" (public endpoints only).
3. **Sign-in: Quick Connect or password?** **Chosen: Quick Connect first.**
   The TV shows a 6-digit code and you approve it in the Jellyfin app on
   your phone. A password entry with the on-screen keyboard can come later.
4. **Subtitles.** Text subtitles (SRT, ASS, VTT) will be fetched and drawn
   by vibe-ui over the video. Image subtitles (PGS, VobSub) make the server
   burn them in, which means transcoding. **Chosen as described.** Say so
   if you use many PGS subtitles: hardware subtitle decoding through
   `subtitleessink` is worth a look then.
5. **Transcode format.** **Chosen: HLS with MPEG-TS segments, H.264/HEVC
   video, AAC/AC-3/E-AC-3 audio up to 5.1.** Only used when direct play is
   impossible.
6. **Firewall.** Rather than the /24 sweep, `debloat.sh` could accept UDP
   from source port 7359. **Chosen: the sweep, no firewall change.**

## Playback path (static analysis; `research/jellyfin/playback-re.md`)

Read from the disassembly of the browser's media backend
(`libhisense-uva-backend.so`), `libhmplayer.so`, `libgsthsplayer.so`,
`libesplayerplugin.so` and `libgstessink.so`. Nothing below has been run.

- **`videoessink` on its own plays nothing.** It hands every buffer to a
  function pointer (`writeDataFunc`) that its owner sets. With none set,
  it drops the data. A plain `gst-launch` pipeline into it would show
  nothing.
- **HMP is the whole player.** It builds a playbin with those sinks, feeds
  the `HS_Esplayer*` hardware injector itself, and takes a URL. The order
  the browser uses for URL playback:
  1. `HMPInit`.
  2. `HMPCreate(&{id, appType 3, dataType 3, kind 32}, &h)`. Kind 32 is
     the URL player; 16 is MSE.
  3. `HMPRegisterCallback(h, fn(h, event, extra))`.
  4. `HMPSetURI(h, struct)`: 12 words, passed by value, with the URL
     probably at word 4.
  5. `HMPPrepare`, which is asynchronous.
  6. `HMPSetSubtitleShowState(h, 0)`.
  7. `HMPSetDisplayRect(h, rect, rect, 0)`: 5 ints each, order unknown.
  8. `HMPSetComponentMuteState(h, 2, 0)`: probably "unmute video".
  9. `HMPStart`. It blocks until the decoders are open, so it runs on its
     own thread.

  After that: `HMPPause`, `HMPResume`, `HMPSeek(h, ms)`, `HMPGetPts`,
  `HMPStop`, `HMPDestroy`. All return 0 on success.
- **No source switch through tvmain was found.** The player libraries call
  no `kilby` model action. They need `mi_daemon` and tvmain's HUI service,
  which stay up, but not app_man, the browser, websdk or LauncherService.
  At most two players can exist at once.
- **Unsure until tried** (`vibe-play` has options for each):
  - the URI word (`--uri-word`);
  - the rectangle's layout (`--rect`);
  - the event numbers: `vibe-play` prints each one raw;
  - the units of `HMPGetPts`. Until they are known, `vibe-play` reports
    its own clock.

## Unknowns and next tests (need the owner and the screen)

- Can our process start a pipeline into `videoessink` and get a picture?
  Does the TV need a source switch away from HDMI first, and does the
  vendor resource manager (`librcm`) need a session?
- Does the DirectFB OSD stay on top of the video plane? (It does for HDMI
  now.)
- Audio over ARC to the soundbar, and AV sync with `render-delay`.

## Morning test plan (owner at the TV, volume low)

1. Deploy (`TV_HOST=192.168.2.152 native/deploy.sh`), then
   `sh /var/local/vibe/restart-ui.sh`. Nothing of the Jellyfin code runs
   until the card is opened.
2. Home, then the **Jellyfin** card (after the inputs). Pick a server.
   Approve the code in the Jellyfin app on the phone (Quick Connect).
3. Browse: Continue watching, Next up, Libraries, a series, a season, an
   episode, then the detail screen.
4. First playback, from SSH, so the UI is out of the way:

   ```sh
   cd /var/local/vibe
   export LD_LIBRARY_PATH=/usr/local/lib/:/lib:/basic/lib
   # the panel's sign-in (step 2); these commands only read the file
   export JF_STATE=/var/local/vibe/state/jellyfin
   ./vibe-jf resume                  # or: views, ls ID, nextup; pick an id
   ./vibe-jf play ID                 # prints the method and the URL
   G=229,233,301,305,401,711,712,713,714,715,901,916,1022,3010,3013,3015,3016,3018
   (echo "open URL"; sleep 30; echo pause; sleep 5; echo resume; sleep 10; echo stop)      | ./vibe-play --user=311:311:$G; cat /tmp/vibe-play.log
   ```

   Watch for a picture, sound on the soundbar, `event N` lines and the
   `pos` lines. If nothing shows, try `--uri-word=0` and other
   `--rect=` orders.
5. Then from the panel: Play, OK (pause), LEFT/RIGHT (seek), BACK (stop),
   and check that the resume point shows on the phone.
6. Recovery: `kill -9` vibe-play mid-film, then switch to an HDMI input
   and check the picture comes back.

## First runs on the TV (2026-11-26, owner at the screen)

Measured with `vibe-play` run from SSH against a direct-play MKV (H.264 +
AC-3 5.1) on "fin". The UI was not involved.

Fixed, each seen in gdb (`/opt/bin/gdb` is on the TV) or in the vendor log:

1. **`dlopen(libhmplayer.so)` failed partway** with `undefined symbol:
   dfbrc_client_createsurface`; the old fallback `dlopen("libhmplayer.so")`
   then returned the half-relocated library, and its first PLT call jumped
   to `0x15ffc` (segfault in HMPInit/HMPCreate). The symbol is in
   `/usr/local/lib/libdfb_renderclient.so`, which needs `DirectFBCreate`
   (`libdirectfb-1.4.so.0`). vibe-play now loads both, RTLD_GLOBAL, first.
2. **libcsp-debug needs `cspDebugInit()`** before any vendor library logs
   (libhmplayer logs in its static constructors); without it,
   `cspDebugCheckLevelGetFrameAndChunk` crashed. vibe-play calls it.
3. **`libgsthsplayer.so` uses curl without linking it**: the separate
   plugin scanner could not load it (`undefined symbol: curl_share_init`).
   vibe-play loads `libcurl.so.4` RTLD_GLOBAL and sets
   `GST_REGISTRY_FORK=no` (registry `/tmp/vibe-play-gstreg2.bin`).
4. **`HMPUriPara_s`: URL in word 0, MIME type in word 4** (the vendor log
   printed our URL as `m_mimeType` when it was in word 4). With
   `video/x-matroska` the player reached "playing".
5. `HMPInit` is skipped (it only logs); `--init` restores it.

Vendor log: the libraries write to the fifo `/var/log/console` (config
`/var/local/kilby/csp-debug-levels.cfg`, level ERR, read only, not changed).
`VIBE_PLAY_VENDOR_LOG=/tmp/file` makes vibe-play (as root, before dropping
privileges) unshare its mount namespace and bind-mount that file over the
fifo, for its own process only. Do not `tail` `/var/log/console`: it blocks.

Event numbers seen: 513 (x2) and 260, 256 after Start; 15, 19 while
playing; 12 at stop; 259 = error (`HSPEvent ERROR`).

**Still failing: no picture, no sound (owner: black screen).** The player
went through prepare/start/pause/resume/seek/stop and every call returned
0, but the vendor log shows

    HUI_StreamInjectOpen for audio Failed! eInputType=0,ePlayer=0,eDataType=0
    HUI_StreamInjectOpen for video Failed! ...
    Appending data failed! ... result=1
    HUI_StreamInjectSetDisplayRect Failed! windowRect[0, 0, 1920, 1080, 0]

The same as root, so not permissions. Also seen at MI_MM_Init:
`LoadMmap: Class "mstar.MI.MmService" failed to MsOS_PA2KSEG(PA =
0x14d00000)`. Unknown: whether the stream injector needs the TV switched
off HDMI to its media source first (the browser's launch path probably does
this through tvmain/kilby; not found yet), or the MI mmap failure is the
cause. `HMPGetPts` returns 0; after a seek its first word is the seek
target.

### The fix: attach the process to MI (measured, 2026-11-26)

Cause (log, then disassembly of `libmi.so` and `libHui.so`):
`HUI_StreamInjectOpen` opens an MI audio-output handle for every stream, and
no MI module works in a process that has not attached to MI. With only
`MI_AOUT_Init` added, vibe-play's stderr showed

    _mi_hm_Open[355] Not inited !!
    MI_AOUT_Init[222]: mi_hm_open failed (4).

The handle manager (`mi_hm_Init`) has one caller, `MI_SYS_Init`.
`MI_SYS_Init(NULL)` is a client attach: it waits until the system is up
(`mi_sys_CheckInited`; `mi_daemon_sys` brings it up) and then sets up this
process. libHui's `Mif_Board_Start_Board` and `HUI_BtUploadInit` make that
same call and accept 0 or 2. It is not the system init that the earlier
static report warned about.

vibe-play now calls `MI_SYS_Init(NULL)` then `MI_AOUT_Init(&zero_byte)`
after loading libhmplayer (`--no-aout-init` skips both). Both return 0. Side
messages, harmless so far: `MI_IIC_Init ... Error 0x2`, `SecurityAuthentication:
pstInitParams is NULL`, `MI_APM_Init: not launched by APM`.

Result of the same 60 s test:

- `HUI_StreamInjectOpen ... success` for audio and video, and
  `SetDisplayRect` returns 0. "Appending data failed" no longer appears in
  the log.
- New events: 9, 10, 5, 1, 2, 17 at start; 21, 1, 2 after a seek; 12, 11 at
  stop.
- `pts` word 1 is now the position in ms (12471 at the pause; 308350 after a
  seek to 300000), and the last word went 1 → 0 after the seek (meaning
  unknown).

Also fixed: `say()` still wrote to `stdout` after `dup2(2,1)`, so protocol
lines went out on stderr, mixed with vendor printf. It now writes to the
saved protocol stream.

**But still nothing on screen, and the video plane stayed black afterwards**
(owner, same run). The decoders opened and took data, yet the owner saw no
picture and heard no sound. vibe-ui's menus still drew over it (OSD fine;
the panel was backlit dark grey). After vibe-play exited, switching to
HDMI3 or HDMI2 with the remote still gave a black picture. Earlier runs
(before `MI_SYS_Init`) also left the screen black, but changing input
brought the picture back. This time only a reboot did; after it, HDMI3
was selected as usual.

Unknown: what holds the video plane. Guesses, not checked: the MI attach
makes this process own handles (video path, display window, mute) that are
not released or reset when it exits; or the source switch to STREAM inside
esplayer is never undone, and with MI attached the HDMI switch no longer
overrides it. Before the next run vibe-play needs a stop path that undoes
the switch, and a way back without a reboot. Until then each playback test
may need a reboot.

### The browser's A/V request, and what the display rect really moved (2026-11-26)

Static analysis (`research/jellyfin/srcswitch-re.md`): the stock browser's
media backend sends `kilby.sl2.action.source.switch` with all-string
arguments `["ss_access_<n>", "2" (REQUEST_RESOURCE), "hbbtv", "3"]`
just before `HMPStart`. That call blocks up to 5 s. The backend also sets
`kilby.sl2.i32.videoplayer.enum.hbbtv.stream.media.status` to 1 for
playback and back to 0 afterwards. It never sends a release (reason 3).
vibe-play does both with `--av-request` / `--media-status` (off by default).

Live run with both flags on (owner at the screen): each model call
returned 1 (success), the decoders opened, and data went through with no
errors. Still no picture and no sound. **The HDMI3 picture shrank into the
top-left quarter of the screen**, and after stop the screen went black,
with the UI still drawn on top.

What that shows:
- The panel is 3840x2160 (`kilby.sl2.str.video.panel.resolution.info` =
  `3840*2160`), and `HMPSetDisplayRect` takes panel pixels. Our
  0,0,1920,1080 is the top-left quarter.
- That rect moved **the display window that shows HDMI**. The stream's
  decoders were never connected to it: tvmain kept the window on HDMI3
  (`source.input.current` stayed `HDMI3` throughout).
- At stop, HMP's teardown mutes or closes that window. That is the black
  screen after every run since the decoders first opened. After this run
  `kilby.sl2.str.servicemode.videoinfo.source` read `0,0,0,0`.
  Recovery was a reboot.

So the missing step is a switch of the main video path away from HDMI
to the stream/app source, which tvmain does when the stock UI opens an
app. The "hbbtv" resource request does not do it, since for HbbTV the
broadcast stays the source.

Next: find how the stock UI starts a full-screen app (the
`UI_START_APP` reason 4 call with its JSON), what `source.input.current`
reads while an app plays, and whether that path needs `app_man` (which
debloat stops). Also: the rect must be scaled to the panel (x2), and
stop must not leave the HDMI window muted.

### How the stock UI takes the video path: source "DMP" (static, 2026-11-26)

Read in the stock JS and in systemserver/tvmain (full notes:
`research/jellyfin/apm-path-re.md`; not yet tried on the TV):

- The stock media pages switch the source to `DMP` with
  `kilby.sl2.action.source.switch [ts, 9, "DMP"]`: DLNA (`DLNA.js`), PVR
  media (`86.js`) and the sound test (`302.js`). HiMedia (USB media) uses
  the reason-4 JSON form, which reaches the same handler.
- tvmain stores the vector in `kilby.sl2.vstr.source.switch`, and
  systemserver's APM watches it. For `DMP` the APM starts the virtual
  `DmpProxy`, which forks nothing and needs no app_man/apmbond. That proxy
  requests the resource; tvmain then gets
  `kilby.sl2.action.apm.source.switch ["sourceSwitchForApp", sid, "DMP",
  "external://plain/dmp", "", "-1"]` and builds the app path (source type 7).
- Going back: the same action with the physical input (`[ts, 9, "HDMI3"]`,
  as vibe-ui does). The stock pages do the same with `setSource("LAST")`.
- Not to use: the ids `browser`, `cobalt`, `Youtube` and `Netflix`. Their
  proxies fork the stock apps.
- Unknown: whether HMP/libHui output reaches the window on the DMP path
  (the browser uses `external://plain/browser`).

vibe-play `--dmp-path` (off by default) switches to `DMP` before
`HMPCreate` and back to the saved input at stop.

**Verified on the screen (owner, 2026-11-26): picture and sound.** With
`--dmp-path` and no other model call, the film played full screen with
sound. Seek worked, and at stop the switch back to HDMI3 brought the HDMI
picture back (no reboot). Log: `source.switch DMP -> 1`, rect
3840x2160 -> 0, both injectors opened, stop `source.switch HDMI3 -> 1`.
`kilby.sl2.str.source.input.current` read `HDMI3` throughout, so it
does not show `DMP`.

The working sequence, as vibe-play runs it now (DMP on by default):

1. Load the libraries, then `MI_SYS_Init(NULL)` and `MI_AOUT_Init`.
2. Save the current input, then `source.switch [ts, 9, "DMP"]`.
3. HMPCreate → RegisterCallback → SetURI (word 0 URL, word 4 MIME) →
   Prepare → SetSubtitleShowState(0).
4. HMPStart, then SetDisplayRect in panel pixels, then
   SetComponentMuteState(2, 0).
5. At stop: Stop, UnRegisterCallback, Destroy, then
   `source.switch [ts, 9, <saved input>]`.

The `hbbtv` request and the media status are not needed (both still
opt-in). Open: if vibe-play is killed, nothing switches back. vibe-ui must
re-select the input when the player dies.

**From the panel (owner, 2026-11-26, deployed build):** play, OK
pause/resume, LEFT/RIGHT seek and BACK stop all work, and HDMI comes back
at stop. The resume point is offered after a stop a few minutes in
(Jellyfin's own threshold). One glitch: the ◀ ▶ seek hint drew as boxes
(the font has no arrow glyphs), so it now reads `LEFT/RIGHT`. Not yet
tested: killing vibe-play mid-film (vibe-ui should switch back).

**Killing vibe-play mid-film (owner at the screen, 2026-11-26).** vibe-ui
noticed and went back to the item page, but the screen stayed black.
`source.switch [ts, 9, "HDMI3"]` alone was accepted (r=1) and changed
nothing: `kilby.sl2.str.servicemode.videoinfo.source` stayed `0,0,0,0`.
Switching to `DMP`, waiting 3 s, then `HDMI3` brought the picture back
(video info `4096,2160,60,0`; owner saw HDMI3). vibe-ui now does that
after any unclean exit of the player (`player_reap`).

Second kill test, with that recovery built in: the log showed `DMP ->
HDMI3`, both calls returned 1, and the screen stayed black (video info
`0,0,0,0`). The dead player had left the source on DMP, so the switch to
DMP did nothing and the HDMI3 switch only ended that DMP session. A second
DMP → HDMI3 brought the picture back. vibe-ui now switches to the input,
waits 2 s, switches to DMP, waits 3 s, then switches to the input again.

Third kill test (input → DMP → input, deployed build 48bf6e3): vibe-ui
logged `player died (status 0x9), HDMI3 -> DMP -> HDMI3`, and about 12 s
after the kill the video info read `4096,2160,60,0` (HDMI3 signal on the
main window) with no manual step.

### End of play and the position (static and logs, 2026-11-26)

- HMP event numbers, read in the browser backend's `hiOnHMPCallBack`
  (libhisense-uva-backend, dispatch at 0x78060; log strings resolved
  with `research/jellyfin/tools/xref.py`): **7 = HMP_VIDEO_EVENT_END_OF_PLAY**,
  **8 = HMP_AUDIO_EVENT_END_OF_PLAY**. The backend sends PlaybackFinished
  to the page on these. Not yet seen live: no test has played to the
  end. vibe-play now sends `ended` on 7, and vibe-ui stops, reports the
  item played and gives the input back. The clock-past-duration fallback
  stays.
- `HMPGetPts` first field: the media position in ms once the decoders run
  (12012 where our wall clock said 12485; 308350 ten seconds after a seek
  to 300000). Before the decoders opened it was 0, which the earlier
  "returns 0" note was about. vibe-play now re-bases its clock on it every
  second.

### Card screens (2026-11-26), and a lesson

Home is now laid out like the Jellyfin web client (owner's request, with
a screenshot): shelves "My Media", "Continue Watching" and "Next Up" of
16:9 cards that scroll sideways, then "Account" (Sign out). Libraries are a
grid of 2:3 posters (films, series, seasons, collections) or 16:9 cards.
Card images follow the web client's choice: Thumb, then the series' Thumb
for episodes (`ParentThumbItemId`/`ParentThumbImageTag`), then Primary,
then a backdrop. `jfimg.c` fetches them on its own thread, newest request
first, decodes with stb_image and caches up to 48 images / 24 MB.

Two traps found on the way:
- **Thread-locals break the binaries.** With zig, a `__thread` variable
  (stb_image has them by default) adds `NEEDED ld-linux-armhf.so.3`
  (`__tls_get_addr`). The TV only has `/lib/ld-linux.so.3`, so the binary
  does not start at all: vibe-ui would have looped in boot.sh. Fixed with
  `STBI_NO_THREAD_LOCALS`, and `build.sh` now fails if any binary needs
  that library. Found by `jfui_test` failing to start on the TV, before any
  deploy.
- **Signing out from a test revoked the UI's token.** `jfui_test` runs with
  a copy of the UI's session file, and Sign out calls the server's logout,
  which invalidates that token for every copy (`/UserViews` then answered
  HTTP 401). The owner had to sign in again. The panel now refuses to sign
  out when `JFUI_TEST` is set, and jfui_test sets it.

**Resume verified (owner, 2026-11-26):** with the cover held until the
position passes the seek target, no 0:00 frames show, and the loading
screen's text is right.

### Font

vibe-ui now uses Noto Sans (Regular, SemiBold, Bold; OFL, from
notofonts/notofonts.github.io, in `native/fonts/`, deployed to
`/var/local/vibe/fonts`). The TV has only the CJK, symbol and per-script
Noto files plus Roboto, which stays as the fallback if a Noto file is
missing or fails to load. The earlier "Â·" and "â€¦" on the loading screen
were double-encoded text from an edit, not missing glyphs.

### Detail page and subtitles (2026-11-26, not yet seen on screen)

- Detail page in the web client's layout: backdrop (the series' for
  episodes), poster or still with progress, title / S:E - name, meta line
  (year, runtime, rating, time left, "Ends at", 24-hour like vibe-ui's
  clock), overview wrapped by measured width, and the actions as pill
  buttons (LEFT/RIGHT). Checked off-screen with jfui_test.
- Subtitles: text tracks come from
  `/Videos/{item}/{mediaSource}/Subtitles/{index}/0/Stream.srt`. The
  server converts embedded and sidecar tracks alike: South Park S29E6's
  embedded "SDH - English - SUBRIP" gave 522 cues (`vibe-jf sub ID INDEX`
  on the TV). `jfsub.c` parses them (BOM, CRLF, tags, VTT-style times) and
  the panel draws the cue under the video timeline, interpolating the
  player's once-a-second position. In the player, DOWN (or the remote's
  SUBTITLE key, if it sends DIKS_SUBTITLE) opens the track menu. The
  server's default track for the user starts automatically and the choice
  goes into the progress reports. Picture-based tracks (PGS, DVD, DVB) are
  listed greyed out: switching to one needs a restart through a burning-in
  transcode, which is not done yet.
- Noto Sans has no music notes (U+2669..266F; checked with fontTools).
  Subtitle lines are drawn in runs, with the notes in the stock
  `/opt/font/Cobalt_fonts/NotoSansSymbols-Regular-Subsetted.ttf`, which has
  ♪ ♫ ♬.

**End of play, seen (owner, South Park S29E6, direct play):** at the end
HMP sent event **8** (audio end) and never 7. The position froze at the
last frame (1321111 ms, ~22:01) and the picture stayed there. vibe-play
now says `ended` on 7, or on 8 once the position stops moving. As a
backstop, the panel ends playback when the position is within 15 s of the
duration and has not moved for 6 s. On the end the panel reports the stop
at the full duration, marks the item played (`JB_PLAYED`), gives the input
back and returns to the detail page.

**The remote's CC button** sends key `0xf069` (code 617), a Hisense code
past DirectFB's list, not `DIKS_SUBTITLE`. It opens the subtitle menu now.

### Transcode (HLS) on the TV (2026-11-26)

Forced with `JF_FORCE_TRANSCODE=1`, or the switch file
`/var/local/vibe/jf.force-transcode` (no direct play or direct stream).
For the South Park episode the server offered
`/videos/{id}/master.m3u8?...VideoCodec=h264,hevc&AudioCodec=ac3...&SegmentContainer=ts`
(TranscodeReasons=DirectPlayError, i.e. forced). vibe-play with
`--mime=application/x-mpegURL`: both injectors opened, the position
advanced (20145 ms at 30 s), pause/resume worked, a seek to 300000 landed
at 313521 after 15 s, stop gave HDMI3 back (`4096,2160,60,0`), and the
owner heard the sound. Six "Appending data failed" lines in the whole
run. The picture was hidden by the panel, which was open during this
background test, so it is checked from the panel with the switch file.

**From the panel with the switch file (owner):** the HLS conversion plays
with picture and sound. Pause, seek, resume and stop work, and the text
subtitles stay in time after seeking. Switch file removed afterwards
(direct play again).

### Audio tracks (2026-11-26)

The player menu (DOWN / CC) now lists the audio tracks (when there is
more than one) above the subtitles. Picking another track restarts
playback at the current position: vibe-play gets `stop keep` (stops
without switching back to the input, so HDMI does not flash in between),
and `jf_playback` asks again with `AudioStreamIndex`. If the server still
offers direct play for a non-default track, it asks once more with direct
play off, so the server remuxes (video copied) with that track; the TV's
player would otherwise take the file's default. The subtitle choice
carries over. Not tried on screen yet (needs a file with two audio
tracks).

Faster route for later: the browser backend switches tracks in the player
with `HMPSelectTrack(handle, 2, streamId)`
(`HiBackendStreamingBackendImpl::notifyMediaToSelectAudioIndex`, 0x6899c).
The streamId comes from `HiStreamIdDataMap::getData(index)`, which is
filled from player events not yet decoded.

Pinned by the owner: picture-based subtitles (burn-in through a restart
with `SubtitleStreamIndex` on the conversion path). No such content in the
libraries yet; to pick up if one turns up.

**Audio switch, seen (owner):** switching works, but it restarted at 0:00.
The server's HLS conversion starts at the beginning even though
PlaybackInfo had `StartTimeTicks`, so every method now starts at 0 and
seeks, under the loading cover. Also fixed: audio track names were greyed
(the grey is meant for picture-based subtitles), and "On" was unreadable
on the focused row.

**Audio switch still plays the default language (owner, "The Social
Reckoning": #1 Portuguese (Brazil) E-AC-3 default, #2 English E-AC-3).**
What was measured:
- `PlaybackInfo` with `AudioStreamIndex=2` still offers direct play, and
  echoes the request back as `DefaultAudioStreamIndex`. So the "remux
  instead" check (requested != DefaultAudioStreamIndex) never fired from
  vibe-jf. The server's session list nevertheless showed the TV on a remux
  with `AudioStreamIndex: 2` (TS, video and audio copied).
- The remux URL carries `AudioStreamIndex=2`, and its first TS segment has
  one H.264 and one E-AC-3 stream (PMT, no language descriptor). But the
  audio payload of segment 0 is **byte-identical** for
  `AudioStreamIndex=1` and `=2` (md5 of the PID 257 payload). So with audio
  copied, this server sent the same track whatever the index; or both
  requests were served by one transcode job (not ruled out).
- Next: switch tracks in the TV's player instead (`HMPSelectTrack`,
  static analysis running); that also avoids the restart.

**Audio tracks in the player (static: research/jellyfin/hmp-tracks-re.md).**
Event 256's `extra` is `{int state; HMPMetaData_s *md}`, with the track
list there when state == 2. md has the count at +0x20 and 0x30-byte
records from +0x24: id +0 (the value `HMPSelectTrack` takes), type +4
(1 video, 2 audio, 4 subtitle), language +0xc, selected +0x18. The browser
switches with `HMPSetComponentMuteState(h, 1, 0)` then
`HMPSelectTrack(h, 2, id)` and never unselects. Component values: 1
audio, 2 video, 3 subtitle. A wrong id quietly selects the first audio
track. vibe-play logs the list (`track N id ...`) and takes
`track audio K`; the panel sends that for direct play (no restart) and
keeps the restart only for conversions.

**Audio switch verified (owner):** `HMPSelectTrack` changes the language
at once, with no restart. Now also:
- English by default (owner's wish): if the default audio track isn't
  English and the file has an English one, the panel switches to it when
  playback starts (direct play).
- Aspect ratio (owner: a 2.39:1 film, 1920x800, was stretched to 16:9):
  vibe-play reads event 19's `HMPVideoInfo_s` (frame +0x1c/+0x20, display
  aspect +0x24/+0x28, from static analysis) and fits the window to that
  shape inside the full-screen rect, leaving black bars. It is applied
  from the 1 s ticker, not inside the callback. Logged as `note video WxH
  aspect A:B`, to check against the static reading.

**English default, first try (owner):** the menu showed English "On"
while the TV played Portuguese, and no switch was sent. For this film the
server's `DefaultAudioStreamIndex` was English (a language preference),
but in direct play the TV's player takes the file's own default (the
track with IsDefault, Portuguese here). The panel now treats the file's
default as what plays, picks the server's choice (or English if that
isn't English), and switches when they differ.
Aspect ratio verified (owner). vibe-play logged `video 1920x800 aspect
24000:10000` and set `rect 0,280 3840x1600`; the first event 19 had
zeros, which are ignored.

**English audio default verified (owner).** Subtitles now (owner's rule):
off by default, or the English *forced* text track if the file has one
(`IsForced`, now parsed). The panel asks the server for no subtitles
(`SubtitleStreamIndex=-1`), so it never picks a picture-based default
that would force a burn-in conversion. The server's subtitle default is
ignored.

**Subtitle defaults verified (owner).** Then:
- The loading screen shows the item's backdrop (the detail page's, from
  the image cache), darkened, under the title and "Loading…".
- Resume: vibe-play mutes audio (`HMPSetComponentMuteState(h, 1, 1)`;
  component 1 = audio per the backend) right after `HMPStart` when it has
  to seek, and unmutes when the position has passed the start point, just
  before `state playing`. The mute only works once the decoders exist, so
  a few ms after HMPStart returns may still sound.
