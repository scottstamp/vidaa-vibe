# 14 — YouTube client ("SmartTube equivalent")

Branch `youtube`, started 2026-11-26. Goal (owner): a YouTube player in
vibe-ui like SmartTube for Android TV — no ad UI, SponsorBlock, the
convenience features — on this TV.

## Owner decisions (2026-11-26)

- **Stream resolution runs in a helper on a home machine**, not on the TV:
  yt-dlp, which the community fixes within days whenever YouTube changes
  its player challenge or bot checks (`pip install -U yt-dlp`). The TV
  asks the helper for something HMP can play.
- **Sign-in with a YouTube account** by device code (the TV shows a code
  for youtube.com/activate), for subscriptions, history, personalized home.
- Rules carried over: no DRM circumvention (paid/rented movies are
  Widevine and are skipped), personal use on this TV only.

## How SmartTube does it (read from `MediaServiceCore`, MIT, cloned to
`research/dl-smarttube/`, not built)

- InnerTube JSON API with several client identities (`WEB`, `MWEB`,
  `ANDROID`, `ANDROID_VR`, `TVHTML5`, `IOS`, …), chosen per request.
- An embedded V8 runs YouTube's player JavaScript to solve the
  n-challenge (`app/nsigsolver`) and to generate BotGuard PoTokens
  (`app/potokennp2`), with a cloud PoToken fallback (`app/potokencloud`).
  The TV client has its own session PoToken (`TvPoTokenProvider`).
- SponsorBlock (`block`), DeArrow (`dearrow`), lounge pairing for casting
  from a phone (`lounge`), live chat (`chat`), comments.

That JS/PoToken layer is the part that breaks; the helper with yt-dlp
replaces it. Browsing and account features are plain InnerTube JSON and
can live on the TV.

## Probe results (2026-11-26)

On the PC: yt-dlp 2026.11.23 with Node 26 as its JS runtime
(`--js-runtimes node`) resolves a public test video (Blender's Big Buck
Bunny): 144p–4K in AVC, VP9 and AV1; audio AAC, Opus, AC-3. Only format
18 (360p AVC+AAC) is muxed; the rest is separate video/audio, as DASH
files or as an **HLS master** (`manifest.googlevideo.com/.../hls_variant`)
with AVC and VP9 variants up to 1080p60 (VP9 to 4K) and the audio as
`EXT-X-MEDIA` groups.

On the TV, through `vibe-play` (scripts in `research/yt-probe/`, owner
watching):

| Test | Result |
| --- | --- |
| format 18, the googlevideo **https** URL directly | `HMPStart` fails (error 1), nothing in the log |
| the same file through a plain-**http** proxy on the PC (Range passed) | plays: audio + video tracks, 14.5 s reached |
| the HLS master through an http proxy that rewrites every playlist URI to itself | plays, separate audio group included |
| the same master cut down to the 1080p60 AVC variant only | plays |
| another video's master cut down to **2160p60 VP9 Profile 2 HDR10** (`vp09.02.51.10…`, ~31 Mbit/s) | plays: tracks found, 27.6 s reached in a 40 s run (≈12 s to start); smoothness and HDR to be confirmed by eye. Its SDR 4K variant is ~77 Mbit/s (not tried) |

**Correction, same day: https was never the problem — DNS was.** curl's
log (`VIBE_CURL_DEBUG=1` in `vibe-play`) said `Could not resolve:
…googlevideo.com`. The TV's manual-IP settings (`network/cfg/manual-ip/`
in `/var/local/kilby/loreg-kilby*.dat`, model keys
`kilby.sl2.str.network.dns-server-1/-2`, which read back empty through
the model) named the PC (192.168.2.154) as DNS 1, from the rooting work,
where `tools/dns-pin.py` still ran: it answers NXDOMAIN for everything
but `alvidaa.vidaahub.com`, and c-ares stops at NXDOMAIN instead of
trying 1.1.1.1. Every host name on the TV failed (Jellyfin uses an IP).
Fixed: `debloat.sh` keeps `/var/local/etc/resolv.conf` (nwksrv's file,
rewritten in place) at `1.1.1.1 1.0.0.1` on every pass (switch
`dns.off`; the original is in `/var/local/vibe/resolv.conf.orig`).
After that, the googlevideo https URL plays directly in HMP.

**TLS checking.** The TV's libcurl 7.77.0 (OpenSSL 1.0.2u) has no CA file
compiled in; Hisense's `curlhttpsrc` (`gst-plugins-hisense`, properties
`ssl-ca-file`, `ssl-strict`, env `SSL_VERIFY_DISABLED`) **turns peer
verification off by itself**: with a CA file holding one unrelated
certificate, https still played. `vibe-play` now exports
`curl_multi_add_handle`/`curl_easy_perform` shims (`-rdynamic`; the plugin
binds curl from the global scope) that set `CURLOPT_CAINFO`
(`/var/local/vibe/cacert.pem` if present, else Hisense's Mozilla bundle
`/opt/hisdk2.0/ca/cacert.pem`, 140 roots, 2023) plus `SSL_VERIFYPEER=1`,
`SSL_VERIFYHOST=2` on every handle. Tested: wrong CA → `HMPStart 1`;
the bundle → plays.

So the helper is still needed (resolution with yt-dlp, SponsorBlock), but
not necessarily as a video proxy: the TV can fetch googlevideo itself.
**HMP plays YouTube's own HLS with separate audio** (tested through the
proxy; direct HLS not yet). The
helper therefore needs no ffmpeg: resolve with yt-dlp, serve the HLS
master over http with every URI rewritten to go through the helper, and
choose the variant by cutting the master. YouTube's URLs are bound to the
requesting IP; the helper both resolves and fetches, so that holds.
Untested: VP9 variants (1440p/4K), AV1, the TV's own choice among several
variants, seeking within HLS.

## Plan

1. **Probe (PC + TV):** yt-dlp on the PC resolves a public test video;
   find which forms HMP plays: progressive MP4 (format 18, 360p), DASH
   MPD (separate audio/video), or HLS remuxed by the helper.
2. **Helper v0:** small HTTP service (Python, yt-dlp): `/play/<id>` →
   a URL or manifest HMP plays; `/sponsor/<id>` → SponsorBlock segments
   (public API). Runs on this PC first; moves to the always-on machine.
3. **TV client:** a YouTube card on Home and a panel like the Jellyfin
   one (shelves, search, channel, detail), playback through `vibe-play`,
   SponsorBlock skips, resume.
4. **Account:** device-code sign-in on the TV; subscriptions, history,
   home feed through InnerTube with the token. Token in vibe-ui's state.
5. Later: DeArrow titles, casting from a phone (lounge), quality and speed
   controls, live.
