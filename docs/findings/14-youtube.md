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

So: **HMP cannot open YouTube's https URLs** (TLS; it plays Jellyfin's
http fine), and **HMP plays YouTube's own HLS with separate audio**. The
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
