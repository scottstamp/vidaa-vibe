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

## v1 built (2026-11-26, branch `youtube`)

- **Helper** `helper/ythelper.py` (Python, yt-dlp as a library, Node as
  its JS runtime): `/api/home|search|channel|video|play|history|following`,
  `POST /api/progress|follow`, `/hls/<id>.m3u8` (YouTube's master cut to
  one variant: the best AVC up to 1080p, VP9 above; SDR preferred; its
  audio group; absolute googlevideo https URIs the TV fetches itself),
  `/thumb/<id>.jpg` (i.ytimg.com over http). SponsorBlock by hashed prefix
  (`/api/skipSegments/<sha256[:4]>`), so the full video id is not sent.
  State (follows, positions) in `~/.vibe-ythelper.json` on the helper's
  machine. Runs on the PC for now (port 8770); to move to `fin`.
- **TV** `ytui.c` + `yt.c` (+ `jfimg_get_url` for plain http images): a
  YouTube card on Home (switch `yt.off`). Tabs Home (Continue watching,
  then one shelf per followed channel), Search (the Jellyfin keyboard),
  History; a channel grid. **OK on a card plays at once** (owner's UX
  choice; resumes past 30 s), a loading screen with the thumbnail; the
  details are in the overlay any navigation key shows: title, channel,
  views, age, the SponsorBlock summary, the bar with skip (green) and
  marked (yellow) segments, and actions on DOWN (Channel, Follow, From the
  start, Description). BACK hides the overlay, then stops; the end or a
  stop returns to the screen played from, with the card's progress.
- SponsorBlock auto-skip: sponsor, selfpromo, interaction, music_offtopic
  (yt.c `yt_auto_skip`), with a toast. Verified on the TV: "skipped
  selfpromo 61.1-75.1" on an LTT video; progress saved to the helper.
- Not yet: sign-in (device code), quality choice in the UI, live chat,
  DeArrow, casting, playback speed, more results paging.

## The helper on fin (2026-11-27)

Installed with `helper/install-fin.sh` on `fin` (192.168.2.31, Ubuntu 24.04
in Proxmox, also the Jellyfin server; root by the TV's key, host key pinned
in `server/keys/fin_known_hosts`): user `vibeyt` (no login),
`/opt/vibe-ythelper` (script + venv, yt-dlp 2026.11.23, Node 24 as the JS
runtime), state `/var/lib/vibe-ythelper/state.json` (moved over from the
PC), `vibe-ythelper.service` on :8770 (sandboxed: ProtectSystem=strict,
ProtectHome, NoNewPrivileges), `vibe-ythelper-update.timer` (daily
`pip install -U yt-dlp` + restart). fin's stream URLs are bound to the same
public IP as the TV's (checked), so the TV fetches what fin resolves. The
TV's `/var/local/vibe/state/youtube` says `helper=http://192.168.2.31:8770`
(owned by UhisUI), and that is also the built-in default now. Way back: in
the script's header.

## More of the UI (2026-11-27)

- **Speed:** `HMPSetSpeed(h, HMPSpeed_e)` exists in libhmplayer (no caller
  in the stock code). The streaming backend's `speed()` (0x26bc0) passes
  the value to the ES player and, above 99, calls
  `HSPlayerStreaming::SetSpeed(v / 100.0)`: percent. Measured: with
  `speed 150` HMP's own position (`HMPGetPts`, which vibe-play's clock
  follows) advanced 1.5 s per second, back to 1.0 after `speed 100`; it
  returns 1 if sent before playback has started. vibe-play: `speed P`
  (25..400); its clock runs at the rate. Audio pitch at speed: not judged.
- Overlay actions now: Channel, Follow/Unfollow, From the start,
  **Speed** (1x, 1.25, 1.5, 1.75, 2, 0.75; kept for the session, applied
  once each video plays), **Up to Np** (2160/1440/1080/720/480: saved in
  the state file, the stream restarts at the same position with the new
  cap; tested 1080 -> 720, speed re-applied), Description (a panel above).
- **Channel page:** a Follow/Unfollow button top right (UP from the first
  row). Home reloads when you come back to it after a follow or a watch.
- Tested on the TV through the dev hook: description panel, channel page,
  follow (its shelf on Home), quality switch, speed, a video to its end
  (back to the list it was played from, recorded as watched).
- Helper: positions are applied when a cached list is served.

## Sign-in (2026-11-27, owner signed in)

- As SmartTube does it (its MediaServiceCore `auth/V2`): the YouTube TV
  app's OAuth client id and secret are read from `base.js` of
  `https://www.youtube.com/tv` (fetched with a Cobalt TV user agent; regex
  `clientId:"...apps.googleusercontent.com",X:"secret"`); the TV client
  version from the same page (`7.20261123.19.00` today). Device flow:
  `POST youtube.com/o/oauth2/device/code` {client_id, device_id (a UUID
  kept in the helper's state), model_name `ytlr::`, scope
  `http://gdata.youtube.com https://www.googleapis.com/auth/youtube-paid-content`}
  -> user code; Google says to enter it at **www.google.com/device**.
  Poll `POST /o/oauth2/token` (grant `http://oauth.net/grant_type/device/1.0`)
  every 5 s; refresh with `grant_type=refresh_token`. The refresh token is
  in the helper's state file on fin (vibeyt only); the TV never holds it.
- Signed-in data: InnerTube `browse` as `TVHTML5` with `Authorization:
  Bearer`. Signed out, `FEwhat_to_watch` only returns a sign-in nudge.
  `FEsubscriptions` = "Most relevant" (15), then the chronological feed as
  **untitled shelves of 3 tiles**, then Shorts; `FEwhat_to_watch` = titled
  shelves of 5 (Recommended, Recently uploaded, From your Watch later
  playlist, topic shelves). Each shelf has a `nextContinuationData` token
  (more tiles: `browse {"continuation": tok}`), the page another for more
  shelves. Tiles: `tileRenderer` -> `onSelectCommand.watchEndpoint.videoId`,
  `metadata.tileMetadataRenderer.title` and `lines` (channel; "views · age"),
  `header.tileHeaderRenderer.thumbnailOverlays`
  (`thumbnailOverlayTimeStatusRenderer` duration/style LIVE/SHORTS,
  `thumbnailOverlayResumePlaybackRenderer.percentDurationWatched`).
  Account name: `account/accounts_list`. Debug aid: `helper/probe_feed.py`.
- Home when signed in: Continue watching, **Subscriptions** (the feed rows
  merged, one more page: 27 videos), YouTube's own shelves (each extended
  by one continuation), then locally followed channels. Shorts, movies and
  playlists are skipped. Account tab: Sign in (code, polled), Cancel, Sign
  out (OK twice; forgets the token on fin only).
- Not done: playback is still resolved signed out (yt-dlp), so members-only
  and age-restricted videos do not play; watching here is not reported to
  YouTube (no history, no "watched" marks there).

## Casting from the phone: the lounge receiver in the helper (2026-11-27)

`helper/ytlounge.py` (standard library only) is the screen side of
YouTube's lounge ("Link with TV code"), ported from SmartTube's
`youtubeapi/.../lounge/`. The helper keeps the session; the TV long-polls
the helper for commands and posts what it plays. **Not yet tried with a
real phone**; everything below was measured from the PC against YouTube's
servers, with a stand-in for the phone (`research/yt-lounge/fake_remote.py`:
`pairing/get_screen` with the code, then a bind as `REMOTE_CONTROL`).

Helper API (TV side in `yt.c`): `POST /api/remote/start` -> `{code, name}`
(a fresh 12-digit code each call; on from now, also after a restart),
`GET /api/remote/status` -> `{enabled, running, connected}`,
`POST /api/remote/stop`, `GET /api/remote/next?timeout=25` -> `{cmd: none |
play (id, pos) | pause | resume | seek (pos) | stop}`,
`POST /api/remote/state?id=&pos=&dur=&state=playing|paused|loading|stopped`
(also `ended`: the next video of the phone's queue is started, if any).
State: `remote: {enabled, screen_id, device_id}` in the helper's state file.
Volume commands are dropped in the helper; nothing reaches the TV.

Measured:

- `pairing/generate_screen_id` -> 26 characters of text.
  `get_lounge_token_batch` -> JSON; the token is 119 characters,
  `loungeTokenLifespanMs` 14 days, `refreshIntervalMs` 13 days.
  `get_pairing_code?ctx=pair` -> 12 digits as text/plain. Older codes keep
  working after a newer one is made, and no restart of the session is
  needed for a code to work (SmartTube drops its token after each code).
- Bind (`POST bc/bind`, body `count=0`): one chunk, `<length>\n` then a
  JSON array of `[index, [command, args]]`: `c` (SID), `S` (gsessionid),
  `loungeStatus` (`devices` is a JSON string; ours is listed as
  `LOUNGE_SCREEN`, clientName `tvhtml5`), `getNowPlaying`,
  `getDiscoveryDeviceId` (indexes 0-4).
- The long poll (`GET bc/bind?RID=rpc&CI=0&SID&gsessionid&AID`): the server
  honours `AID`: with `AID=3` it sent array 4 again, with SmartTube's fixed
  `AID=42` it sent nothing old. A `noop` every 24-29 s. The server ends the
  poll after about 290 s (clean EOF); opening it again with the same SID and
  the last index works (10 min: one bind, three polls, no errors).
  `TYPE=xmlhttp` made no difference.
- Posting events with a counting `RID` and a per-session `ofs` is accepted
  (SmartTube sends `RID=1337` always). A post with an unknown SID gets
  HTTP 400; a new bind then lists the phone again in `loungeStatus`.
- A phone joining: `loungeStatus`, `remoteConnected` (name, id, type),
  then `getNowPlaying`, `getSubtitlesTrack`, `getVolume`,
  `getPartyGamesMode`. Leaving: `remoteDisconnected`, about 10 s after the
  phone's `TYPE=terminate`.
- `setPlaylist` arrives with `videoId`, `videoIds`, `currentIndex`,
  `currentTime`, `listId`, `eventDetails`, `sourceDeviceType`. The stand-in's
  `setVideo` reached the screen as a `setPlaylist`, its `listId` was
  replaced by the lounge's own queue id (also added to our `nowPlaying`),
  its `ctt` and its `updatePlaylist` did not arrive: the server owns the
  queue. `pause`, `play`, `seekTo {newTime}`, `next`, `previous`,
  `stopVideo`, `setVolume {volume, delta}` arrive as SmartTube models them.
- Our `nowPlaying` (state 3 loading / 1 / 2, `currentTime`, `duration`,
  `currentIndex`, `cpn`), `onStateChange` and `onHasPreviousNextChanged`
  reach the phone side; an empty `nowPlaying` is sent for "nothing playing".
- `TYPE=terminate` on stop (not in SmartTube): the phone side gets
  `loungeScreenDisconnected` within seconds.
- Ending the long poll from another thread by shutting the socket down
  wakes the reader at once on Linux (WSL), on Windows only at the next
  `noop`.

Unknown until a phone is used: what the YouTube app shows for our states
(the empty `nowPlaying`, `getVolume` left unanswered, autoplay), whether it
sends `updatePlaylist` with `videoIds` when the queue changes, whether it
finds the screen again by itself after the helper restarts, `ctt`.

## More SmartTube features (2026-11-27)

- **Dubbed audio:** YouTube auto-dubs many videos; the HLS master lists one
  `EXT-X-MEDIA` per language per group, all `DEFAULT=NO`, and HMP played the
  first (Arabic). The helper keeps one rendition: the English original, else
  English, else the original, else YouTube's default; made `DEFAULT=YES`.
- **DeArrow** (setting, off by default): `sponsor.ajay.app/api/branding/<sha256[:4]>`
  (k-anonymous); titles applied to every list and the details, thumbnails
  from `dearrow-thumb.ajay.app/api/v1/getThumbnail?videoID&time` (the TV
  asks with `&da=1` so its image cache keeps both kinds apart).
- **Watch later / Liked videos** (signed in): TV browse `VLWL` / `VLLL`,
  15 tiles a page plus continuations; two pages added.
- **Search** loads 30 more near the end of the results (up to 120).
- **Related and comments** (`helper/ytextra.py`): WEB InnerTube `next`,
  signed out. Related = `lockupViewModel` (contentId, title, metadataRows
  [channel] [views, age], `thumbnailBadgeViewModel` duration); comments =
  the comment section's continuation (`itemSectionRenderer`
  `comment-item-section`), `commentEntityPayload` mutations, top level only.
- **Player:** chapter gaps on the bar and the current chapter's name; an
  "Up next" row (DOWN from the actions); a Comments panel (UP/DOWN scroll);
  Autoplay (setting, on): at the end an 8 s countdown to the first
  suggestion, the player kept on DMP between videos ("stop keep"); with a
  phone connected the phone's queue decides instead (`state=ended`). The
  SponsorBlock summary line is gone (the bar shows the segments).
- **Not possible yet:** reporting watches to YouTube and signed-in
  playback (members-only, age-restricted): the TV client's `player`
  request answers "The page needs to be reloaded" even with
  `signatureTimestamp` (needs a BotGuard PoToken, which SmartTube makes in
  V8), and yt-dlp with our bearer header gets HTTP 400. Probes:
  `helper/probe_authplay.py`.

- **Phone casting tested by the owner** (2026-11-27): linked with a TV code from the YouTube app; casting, pause, seek and stop work as expected.
