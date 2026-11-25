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
- **Not yet known:** whether our own process can drive the video plane, and
  what it needs (resource manager, source switch, app type). Reverse
  engineering is below; the first on-screen test needs the owner.

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

## Unknowns and next tests (need the owner and the screen)

- Can our process start a pipeline into `videoessink` and get a picture?
  Does the TV need a source switch away from HDMI first, and does the
  vendor resource manager (`librcm`) need a session?
- Does the DirectFB OSD stay on top of the video plane? (It does for HDMI
  now.)
- Audio over ARC to the soundbar, and AV sync with `render-delay`.
