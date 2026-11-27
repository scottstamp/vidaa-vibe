#!/usr/bin/env python3
"""
vidaa-vibe YouTube helper: the part of the TV's YouTube client that must keep
up with YouTube (stream resolution by yt-dlp), plus SponsorBlock, thumbnails
over plain http, and the client's small state (follows, progress, history).

The TV's vibe-ui talks to it over http on the LAN (docs/findings/14-youtube.md).
It plays YouTube's HLS itself: /hls/<id>.m3u8 is YouTube's master cut down to
one variant, with absolute https URIs the TV fetches directly.

usage: python ythelper.py [--host 0.0.0.0] [--port 8770] [--state FILE]
                          [--js-runtime node]
needs: pip install "yt-dlp[default]"; a JS runtime for YouTube's player
challenge (node or deno on PATH).

API (GET unless noted; JSON):
  /api/home                       shelves: Continue watching, one per followed channel
  /api/search?q=...&n=24          videos
  /api/channel?id=UC...&n=48      {title, id, followed, items}
  /api/video?id=...               details, SponsorBlock segments, saved position
  /api/play?id=...&maxh=1080      {url, mime, duration, live}
  /api/history?n=60               {items, source}: the account's YouTube history (signed in,
                                  history on) or the local one; most recent first
  /api/settings                   {dearrow, autoplay, history}; POST ?dearrow= / ?autoplay= / ?history=0|1
  /api/following                  channels
  POST /api/progress?id=&pos=&dur=[&stop=1]   seconds; signed in, also the account's watch history
                                  (at most once a minute; stop=1, the TV stopped: at once)
  POST /api/follow?id=UC...&on=0|1
  /hls/<id>.m3u8?maxh=1080        the trimmed HLS master (signed-in DASH files: one built here)
  /hlsf/<id>/<format>.m3u8        a DASH file as an HLS media playlist (byte ranges)
  /thumb/<id>.jpg?s=m|l           video thumbnail (320x180 | 1280x720)
  /avatar/<channel id>.jpg        channel avatar (if known)
 casting from the phone's YouTube app ("Link with TV code"; ytlounge.py):
  POST /api/remote/start          {code: "1234 5678 9012", name}; on from now, also after a restart
  /api/remote/status              {enabled, running, connected: [phone names]}
  POST /api/remote/stop           {ok}; off
  /api/remote/next?timeout=25     long poll: {cmd: none | play (id, pos) | pause | resume | seek (pos) | stop}
  POST /api/remote/state?id=&pos=&dur=&state=playing|paused|loading|stopped   what the TV plays
"""
import argparse, hashlib, json, os, re, signal, sys, threading, time, urllib.parse, urllib.request
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

import yt_dlp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # python -I leaves it out
import ytlounge
import ytextra
import ytauth

ARGS = None
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
SB_API = "https://sponsor.ajay.app/api/skipSegments/"
SB_CATS = ["sponsor", "selfpromo", "interaction", "intro", "outro", "preview",
           "music_offtopic", "filler", "poi_highlight"]

# ------------------------------------------------------------------ state

class State:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.d = {"follows": [], "progress": {}}
        try:
            with open(path, encoding="utf-8") as f:
                self.d.update(json.load(f))
        except FileNotFoundError:
            pass

    def save(self):
        tmp = self.path + ".new"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, self.path)

    def follows(self):
        with self.lock:
            return list(self.d["follows"])

    def is_followed(self, cid):
        return any(c["id"] == cid for c in self.follows())

    def follow(self, cid, on, title="", avatar=""):
        with self.lock:
            self.d["follows"] = [c for c in self.d["follows"] if c["id"] != cid]
            if on:
                self.d["follows"].append({"id": cid, "title": title, "avatar": avatar})
            self.save()

    def progress(self, vid):
        with self.lock:
            return dict(self.d["progress"].get(vid, {}))

    def set_progress(self, vid, pos, dur, meta):
        with self.lock:
            p = self.d["progress"].get(vid, {})
            p.update({"pos": pos, "dur": dur, "t": time.time()})
            for k in ("title", "channel", "channel_id"):
                if meta.get(k): p[k] = meta[k]
            self.d["progress"][vid] = p
            # keep the newest 500
            if len(self.d["progress"]) > 500:
                old = sorted(self.d["progress"].items(), key=lambda kv: kv[1].get("t", 0))
                for k, _ in old[:len(old) - 500]:
                    del self.d["progress"][k]
            self.save()

    def recent(self, n, unfinished_only):
        with self.lock:
            items = sorted(self.d["progress"].items(), key=lambda kv: -kv[1].get("t", 0))
        out = []
        for vid, p in items:
            pos, dur = p.get("pos", 0), p.get("dur", 0)
            if unfinished_only and not (pos > 30 and (dur <= 0 or pos < dur - 30)):
                continue
            out.append({"id": vid, "title": p.get("title", vid), "channel": p.get("channel", ""),
                        "channel_id": p.get("channel_id", ""), "duration": dur, "views": 0,
                        "published": "", "position": pos})
            if len(out) >= n:
                break
        return out

STATE = None

# ------------------------------------------------------------------ cache

class Cache:
    def __init__(self, ttl, maxn=256):
        self.ttl, self.maxn, self.d, self.lock = ttl, maxn, {}, threading.Lock()

    def get(self, k):
        with self.lock:
            v = self.d.get(k)
            if v and time.time() - v[0] < self.ttl:
                return v[1]
        return None

    def put(self, k, v):
        with self.lock:
            if len(self.d) >= self.maxn:
                for kk in sorted(self.d, key=lambda x: self.d[x][0])[:self.maxn // 4]:
                    del self.d[kk]
            self.d[k] = (time.time(), v)
        return v

VIDEO_CACHE = Cache(3 * 3600, 128)   # stream URLs last ~6 h
LIST_CACHE = Cache(15 * 60, 256)
THUMB_CACHE = Cache(24 * 3600, 600)
SB_CACHE = Cache(30 * 60, 256)

# ------------------------------------------------------------------ yt-dlp

def ydl_opts(flat=False, extra=None):
    argv = ["--quiet", "--no-warnings", "--skip-download", "--js-runtimes", ARGS.js_runtime]
    if flat:
        argv.append("--flat-playlist")
    o = yt_dlp.parse_options(argv).ydl_opts
    if extra:
        o.update(extra)
    return o

def extract(url, flat=False, extra=None):
    with yt_dlp.YoutubeDL(ydl_opts(flat, extra)) as y:
        return y.extract_info(url, download=False)

def item_of(e):
    """a list entry (flat) or a full info dict -> a card item"""
    vid = e.get("id") or ""
    pub = e.get("upload_date") or ""
    if len(pub) == 8:
        pub = "%s-%s-%s" % (pub[:4], pub[4:6], pub[6:])
    it = {"id": vid, "title": e.get("title") or "",
          "channel": e.get("channel") or e.get("uploader") or "",
          "channel_id": e.get("channel_id") or "",
          "duration": int(e.get("duration") or 0), "views": int(e.get("view_count") or 0),
          "published": pub, "live": e.get("live_status") == "is_live" or bool(e.get("is_live"))}
    pos = best_pos(vid)
    if pos is not None:
        it["position"] = pos
    return it

# Where to resume. YouTube's position for the account comes from its lists
# (the tile's exact startTimeSeconds when it agrees with the watched-percent
# bar, else percent x length). YouTube's is preferred when it changed after
# this TV last saved one (watched elsewhere). Otherwise this TV's save wins:
# YouTube's marker barely follows the watch reports we send (measured
# 2026-11-26: stopped at 668 s, YouTube kept 26% = 623 s), so an unchanged
# YouTube value is just old. Last-seen values are kept in the state file, so
# a restart does not make old ones look new.
YT_POS_DIRTY = [False]

def yt_pos_seen(vid, pos):
    now = time.time()
    with STATE.lock:
        d = STATE.d.setdefault("ytpos", {})
        old = d.get(vid)
        if old and abs(old[0] - pos) < 1:
            return
        # first sight with a local save: we cannot tell when YouTube's changed
        changed = now if old or vid not in STATE.d["progress"] else 0
        d[vid] = [pos, changed]
        if len(d) > 3000:
            for k in sorted(d, key=lambda k: d[k][1])[:800]:
                del d[k]
        YT_POS_DIRTY[0] = True

def yt_pos_saver():
    while True:
        time.sleep(30)
        if YT_POS_DIRTY[0]:
            YT_POS_DIRTY[0] = False
            with STATE.lock:
                STATE.save()

def best_pos(vid):
    """seconds to resume at, or None (never watched, as far as we know)"""
    with STATE.lock:
        yp = (STATE.d.get("ytpos") or {}).get(vid)
    lp = STATE.progress(vid)
    if yp and (not lp or yp[1] >= lp.get("t", 0)):
        return yp[0]
    return lp.get("pos", 0) if lp else None

def with_positions(items):
    """cached lists carry the positions of when they were made: current ones;
    and, with the DeArrow setting on, the community's titles"""
    out = []
    for it in items:
        it = dict(it)
        pos = best_pos(it["id"])
        if pos is not None:
            it["position"] = pos
        out.append(it)
    if setting("dearrow"):
        brand_many([it["id"] for it in out])
        for it in out:
            b = DA_CACHE.get(it["id"])
            if b and b.get("title"):
                it["title"] = b["title"]
    return out

# ------------------------------------------------------------------ settings, DeArrow

def setting(name):
    with STATE.lock:
        return bool((STATE.d.get("settings") or {}).get(name))

def set_setting(name, on):
    with STATE.lock:
        STATE.d.setdefault("settings", {})[name] = bool(on)
        STATE.save()

DA_API = "https://sponsor.ajay.app/api/branding/"
DA_THUMB = "https://dearrow-thumb.ajay.app/api/v1/getThumbnail"
DA_CACHE = Cache(24 * 3600, 4000)

def brand_of(entry):
    """DeArrow's pick from a branding entry: the first title and thumbnail that
    is not the original and is locked or not voted down (as the extension)"""
    title, ttime = "", None
    for t in entry.get("titles") or []:
        if not t.get("original") and (t.get("locked") or t.get("votes", 0) >= 0) and t.get("title"):
            title = t["title"].replace(">", "").strip()
            break
    for t in entry.get("thumbnails") or []:
        if not t.get("original") and (t.get("locked") or t.get("votes", 0) >= 0) and t.get("timestamp") is not None:
            ttime = float(t["timestamp"])
            break
    return {"title": title, "time": ttime}

def brand(vid):
    b = DA_CACHE.get(vid)
    if b is not None:
        return b
    prefix = hashlib.sha256(vid.encode()).hexdigest()[:4]     # k-anonymous lookup
    try:
        req = urllib.request.Request(DA_API + prefix, headers={"User-Agent": "vidaa-vibe ythelper"})
        data = json.load(urllib.request.urlopen(req, timeout=8))
        for k, v in (data or {}).items():               # cache every video the prefix answered for
            if len(k) == 11:
                DA_CACHE.put(k, brand_of(v))
    except urllib.error.HTTPError as e:
        if e.code != 404:
            print("dearrow:", e, file=sys.stderr)
    except Exception as e:
        print("dearrow:", e, file=sys.stderr)
    b = DA_CACHE.get(vid)
    return b if b is not None else DA_CACHE.put(vid, {"title": "", "time": None})

def brand_many(ids):
    todo = [v for v in dict.fromkeys(ids) if DA_CACHE.get(v) is None]
    if not todo:
        return
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(brand, todo))

def is_video_entry(e):
    return len(e.get("id") or "") == 11 and e.get("ie_key", "Youtube") in ("Youtube", None)

def search(q, n):
    k = ("search", q, n)
    r = LIST_CACHE.get(k)
    if r is None:
        info = extract("ytsearch%d:%s" % (n, q), flat=True)
        r = LIST_CACHE.put(k, [item_of(e) for e in info.get("entries") or [] if is_video_entry(e)])
    return r

def avatar_of(info):
    best = None
    for t in info.get("thumbnails") or []:
        if t.get("id") in ("avatar_uncropped",) or "avatar" in (t.get("id") or ""):
            best = t.get("url")
    return best or ""

def channel(cid, n):
    k = ("channel", cid, n)
    r = LIST_CACHE.get(k)
    if r is None:
        info = extract("https://www.youtube.com/channel/%s/videos" % cid, flat=True,
                       extra={"playlistend": n})
        title = info.get("channel") or info.get("uploader") or info.get("title") or cid
        title = re.sub(r" - Videos$", "", title)
        r = LIST_CACHE.put(k, {"id": cid, "title": title, "avatar": avatar_of(info),
                               "items": [item_of(e) for e in info.get("entries") or [] if is_video_entry(e)]})
    out = dict(r)
    out["items"] = with_positions(r["items"])
    out["followed"] = STATE.is_followed(cid)
    return out

NEEDS_ACCOUNT = re.compile(r"sign in|confirm your age|members|join this channel|login|private video", re.I)

def video(vid):
    """yt-dlp's info, anonymous (YouTube's HLS); what needs an account
    (age-restricted, members-only) signed in, as SmartTube does (ytauth.py:
    DASH files only, which the helper serves as HLS, dash_master)"""
    v = VIDEO_CACHE.get(vid)
    if v is None:
        url = "https://www.youtube.com/watch?v=" + vid
        try:
            if setting("debug_signed_in"):         # test aid: POST /api/settings?debug_signed_in=1
                raise yt_dlp.utils.DownloadError("debug: sign in")
            v = extract(url)
        except yt_dlp.utils.DownloadError as e:
            tok = access_token() if NEEDS_ACCOUNT.search(str(e)) else None
            if not tok:
                raise
            v = extract(url, extra={ytauth.PARAM: tok,
                                    "extractor_args": {"youtube": {"player_client": [ytauth.CLIENT]}}})
            v["vibe_signed_in"] = True
        v = VIDEO_CACHE.put(vid, v)
    return v

def sponsor_segments(vid):
    r = SB_CACHE.get(vid)
    if r is not None:
        return r
    prefix = hashlib.sha256(vid.encode()).hexdigest()[:4]   # k-anonymous lookup
    url = SB_API + prefix + "?" + urllib.parse.urlencode({"categories": json.dumps(SB_CATS)})
    segs = []
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "vidaa-vibe ythelper"})
        data = json.load(urllib.request.urlopen(req, timeout=8))
        for v in data:
            if v.get("videoID") != vid:
                continue
            for s in v.get("segments", []):
                st, en = s.get("segment", [0, 0])
                segs.append({"start": round(st, 2), "end": round(en, 2),
                             "category": s.get("category", ""), "action": s.get("actionType", "skip")})
    except urllib.error.HTTPError as e:
        if e.code != 404:     # 404: no segments for any video with this prefix
            print("sponsorblock:", e, file=sys.stderr)
    except Exception as e:
        print("sponsorblock:", e, file=sys.stderr)
    segs.sort(key=lambda s: s["start"])
    return SB_CACHE.put(vid, segs)

def details(vid):
    v = video(vid)
    it = item_of(v)
    it["description"] = (v.get("description") or "")[:4000]
    it["chapters"] = [{"start": c.get("start_time", 0), "title": c.get("title", "")}
                      for c in v.get("chapters") or []]
    it["segments"] = sponsor_segments(vid)
    it["followed"] = STATE.is_followed(it["channel_id"])
    it.setdefault("position", 0)
    if setting("dearrow"):
        b = brand(vid)
        if b.get("title"):
            it["original_title"], it["title"] = it["title"], b["title"]
    return it

# ------------------------------------------------------------------ HLS

def hls_master_url(v):
    for f in v.get("formats") or []:
        if f.get("protocol", "").startswith("m3u8") and f.get("manifest_url"):
            return f["manifest_url"]
    return None

def trimmed_master(vid, maxh, base):
    """YouTube's master with one variant (and its audio group)"""
    v = video(vid)
    url = hls_master_url(v)
    if not url:
        return dash_master(v, maxh, base)
    text = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}),
                                  timeout=15).read().decode("utf-8", "replace")
    lines = text.splitlines()
    variants = []          # (score, inf line, uri)
    for i, l in enumerate(lines):
        if not l.startswith("#EXT-X-STREAM-INF") or i + 1 >= len(lines):
            continue
        m = re.search(r"RESOLUTION=(\d+)x(\d+)", l)
        h = int(m.group(2)) if m else 0
        bw = int((re.search(r"BANDWIDTH=(\d+)", l) or [0, 0])[1] or 0)
        hdr = "VIDEO-RANGE=PQ" in l or "VIDEO-RANGE=HLG" in l
        avc = "avc1" in l
        if h > maxh:
            continue
        # prefer: higher resolution; AVC up to 1080p (VP9 only above); SDR; then bitrate
        score = (h, 1 if (avc if h <= 1080 else not avc) else 0, 0 if hdr else 1, bw)
        variants.append((score, l, lines[i + 1]))
    if not variants:
        return None
    _, inf, uri = max(variants)
    group = (re.search(r'AUDIO="([^"]+)"', inf) or [None, None])[1]
    out = ["#EXTM3U"]
    media = []
    for l in lines:
        if l.startswith("#EXT-X-INDEPENDENT-SEGMENTS"):
            out.append(l)
        elif l.startswith("#EXT-X-MEDIA") and (group is None or 'GROUP-ID="%s"' % group in l):
            media.append(l)
    a = pick_audio(media)
    if a:
        out.append(a)
    out += [inf, uri]
    return "\n".join(out) + "\n"

def pick_audio(media):
    """One audio rendition, made the default. YouTube now auto-dubs videos
    into many languages and marks none DEFAULT (the TV then played the first:
    Arabic). Prefer: the English original, any English, the original in any
    language, YouTube's DEFAULT, the first."""
    if not media:
        return None
    def lang(l):
        return ((re.search(r'LANGUAGE="([^"]*)"', l) or [None, ""])[1]).lower()
    def original(l):
        name = (re.search(r'NAME="([^"]*)"', l) or [None, ""])[1].lower()
        return "original" in name or ("dubbed" not in name and "YT-EXT-AUDIO-CONTENT-ID" not in l)
    tests = [lambda l: lang(l).startswith("en") and original(l),
             lambda l: lang(l).startswith("en"),
             original,
             lambda l: "DEFAULT=YES" in l,
             lambda l: True]
    for t in tests:
        for l in media:
            if t(l):
                l = re.sub(r"DEFAULT=(YES|NO)", "DEFAULT=YES", l)
                return l if "DEFAULT=YES" in l else l + ",DEFAULT=YES"
    return media[0]

# ------------------------------------------------------------------ DASH files as HLS
#
# Signed-in playback (ytauth.py) gets YouTube's DASH files only: one fMP4
# file per format, ftyp + moov, then a sidx indexing its fragments. HLS can
# play such a file as is (EXT-X-MAP for the init part, EXT-X-BYTERANGE per
# fragment), so the helper writes a playlist per format from the sidx, and
# a master with one AVC video (up to 1080p) and one AAC audio. The TV
# fetches the bytes from googlevideo itself, as with YouTube's own HLS.

DASH_CACHE = Cache(3 * 3600, 64)
HISTORY = ytauth.History()       # watches reported to the signed-in account

def dash_formats(v, maxh):
    fm = [f for f in v.get("formats") or [] if f.get("protocol") == "https" and f.get("url")]
    vids = [f for f in fm if (f.get("vcodec") or "").startswith("avc1") and f.get("acodec") in (None, "none")
            and f.get("ext") == "mp4" and (f.get("height") or 0) <= maxh]
    auds = [f for f in fm if (f.get("acodec") or "").startswith("mp4a") and f.get("vcodec") in (None, "none")]
    if not vids or not auds:
        return None, None
    vf = max(vids, key=lambda f: (f.get("height") or 0, f.get("fps") or 0, f.get("tbr") or 0))
    def lang(f):
        return (f.get("language") or "").lower()
    def original(f):
        note = (f.get("format_note") or "").lower()
        return "original" in note or "dubbed" not in note
    for t in (lambda f: lang(f).startswith("en") and original(f), lambda f: lang(f).startswith("en"),
              original, lambda f: True):
        c = [f for f in auds if t(f)]
        if c:
            # not the DRC (dynamic range compressed) copy
            return vf, max(c, key=lambda f: ("drc" not in (f.get("format_id") or "").lower(), f.get("tbr") or 0))
    return vf, auds[0]

def dash_master(v, maxh, base):
    vf, af = dash_formats(v, maxh)
    if not vf:
        return None
    vid = v["id"]
    bw = int(((vf.get("tbr") or 0) + (af.get("tbr") or 0)) * 1000) or 2000000
    out = ["#EXTM3U", "#EXT-X-INDEPENDENT-SEGMENTS",
           '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="audio",LANGUAGE="%s",DEFAULT=YES,AUTOSELECT=YES,URI="%s/hlsf/%s/%s.m3u8"'
           % ((af.get("language") or "en")[:8], base, vid, af["format_id"]),
           '#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d,CODECS="%s,%s",AUDIO="a"'
           % (bw, vf.get("width") or 0, vf.get("height") or 0, vf["vcodec"], af["acodec"]),
           "%s/hlsf/%s/%s.m3u8" % (base, vid, vf["format_id"])]
    return "\n".join(out) + "\n"

def boxes(b, start=0):
    """(type, offset, size) of the top-level MP4 boxes that fit in b"""
    off = start
    while off + 8 <= len(b):
        size = int.from_bytes(b[off:off + 4], "big")
        typ = b[off + 4:off + 8].decode("latin-1")
        hdr = 8
        if size == 1 and off + 16 <= len(b):
            size, hdr = int.from_bytes(b[off + 8:off + 16], "big"), 16
        if size < hdr:
            return
        yield typ, off, size
        off += size

def sidx_refs(b, off, size):
    """[(offset, size, seconds)] of the fragments a sidx box lists"""
    p = off + 8
    ver = b[p]
    p += 4 + 4                                   # version+flags, reference_ID
    timescale = int.from_bytes(b[p:p + 4], "big"); p += 4
    n = 8 if ver else 4
    p += n                                       # earliest_presentation_time
    first = int.from_bytes(b[p:p + n], "big"); p += n
    p += 2                                       # reserved
    count = int.from_bytes(b[p:p + 2], "big"); p += 2
    at = off + size + first
    refs = []
    for _ in range(count):
        r = int.from_bytes(b[p:p + 4], "big")
        dur = int.from_bytes(b[p + 4:p + 8], "big")
        p += 12
        if r >> 31:
            raise ValueError("hierarchical sidx")
        refs.append((at, r & 0x7fffffff, dur / timescale))
        at += r & 0x7fffffff
    return refs

def dash_playlist(vid, fid):
    k = (vid, fid)
    t = DASH_CACHE.get(k)
    if t:
        return t
    f = next((f for f in video(vid).get("formats") or [] if f.get("format_id") == fid), None)
    if not f:
        return None
    url = f["url"]
    head = b""
    for n in (65536, 1048576):
        req = urllib.request.Request(url, headers={"User-Agent": UA, "Range": "bytes=0-%d" % (n - 1)})
        head = urllib.request.urlopen(req, timeout=20).read()
        sx = next(((o, s) for typ, o, s in boxes(head) if typ == "sidx" and o + s <= len(head)), None)
        if sx:
            break
    else:
        raise LookupError("no sidx in format %s" % fid)
    init_len = sx[0]                              # ftyp + moov: everything before the sidx
    refs = sidx_refs(head, *sx)
    out = ["#EXTM3U", "#EXT-X-VERSION:7",
           "#EXT-X-TARGETDURATION:%d" % max(1, int(max(r[2] for r in refs) + 0.999)),
           "#EXT-X-MEDIA-SEQUENCE:0", "#EXT-X-PLAYLIST-TYPE:VOD", "#EXT-X-INDEPENDENT-SEGMENTS",
           '#EXT-X-MAP:URI="%s",BYTERANGE="%d@0"' % (url, init_len)]
    for at, size, dur in refs:
        out += ["#EXTINF:%.3f," % dur, "#EXT-X-BYTERANGE:%d@%d" % (size, at), url]
    out.append("#EXT-X-ENDLIST")
    return DASH_CACHE.put(k, "\n".join(out) + "\n")

def play_info(vid, maxh, base):
    v = video(vid)
    live = bool(v.get("is_live"))
    if hls_master_url(v) or (not live and dash_formats(v, maxh)[0]):
        return {"url": "%s/hls/%s.m3u8?maxh=%d" % (base, vid, maxh), "mime": "application/x-mpegURL",
                "duration": int(v.get("duration") or 0), "live": live}
    # no HLS: the muxed 360p file (https; the TV verifies it)
    for f in v.get("formats") or []:
        if f.get("format_id") == "18":
            return {"url": f["url"], "mime": "video/mp4", "duration": int(v.get("duration") or 0),
                    "live": live}
    raise LookupError("no playable format")

# ------------------------------------------------------------------ account
#
# Sign-in as SmartTube does it (its MediaServiceCore, auth/V2): the YouTube TV
# app's OAuth client (id and secret read from the TV page's base.js), the
# device-code flow (the TV shows a code for youtube.com/activate), then
# InnerTube requests as the TVHTML5 client with the bearer token. Google may
# change or block any of this; nothing else in the helper depends on it.
# The refresh token lives in the state file (only the helper's user reads it).

TV_UA = ("Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), "
         "Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)")
OAUTH_SCOPE = "http://gdata.youtube.com https://www.googleapis.com/auth/youtube-paid-content"
DEVICE_GRANT = "http://oauth.net/grant_type/device/1.0"
TV_CACHE = Cache(12 * 3600, 4)
AUTH_LOCK = threading.Lock()
PENDING = {}            # the sign-in in progress: device_code, user_code, url, until, interval

def tv_get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": TV_UA}),
                                  timeout=20).read().decode("utf-8", "replace")

def tv_client():
    """{client_id, client_secret, version} of the YouTube TV app (cached)"""
    c = TV_CACHE.get("client")
    if c:
        return c
    page = tv_get("https://www.youtube.com/tv")
    m = (re.search(r'id="base-js" src="(.*?)"', page) or re.search(r"\.src = '(.*?m=base)'", page)
         or re.search(r"\.src = '(.*?)'; .\.id = 'base-js'", page))
    ver = re.search(r'"clientVersion":"([\d.]+)"', page) or re.search(r'"INNERTUBE_CLIENT_VERSION":"([\d.]+)"', page)
    if not m:
        raise LookupError("YouTube TV page changed: no base.js")
    url = m.group(1).replace("\\/", "/")
    if url.startswith("/"):
        url = "https://www.youtube.com" + url
    js = tv_get(url)
    cid = re.search(r'clientId:"([-\w]+\.apps\.googleusercontent\.com)",\n?[$\w]+:"(\w+)"', js)
    if not cid:
        raise LookupError("YouTube TV app changed: no OAuth client in base.js")
    return TV_CACHE.put("client", {"client_id": cid.group(1), "client_secret": cid.group(2),
                                   "version": ver.group(1) if ver else "7.20261123.19.00"})

def post_json(url, body, headers=None):
    h = {"User-Agent": TV_UA, "Content-Type": "application/json"}
    h.update(headers or {})
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST", headers=h)
    try:
        return json.load(urllib.request.urlopen(req, timeout=20))
    except urllib.error.HTTPError as e:
        try:
            return json.load(e)
        except Exception:
            raise e

def auth_state():
    with STATE.lock:
        return dict(STATE.d.get("auth") or {})

def auth_save(a):
    with STATE.lock:
        if a:
            STATE.d["auth"] = a
        else:
            STATE.d.pop("auth", None)
        STATE.save()

def device_id():
    with STATE.lock:
        if not STATE.d.get("device_id"):
            import uuid
            STATE.d["device_id"] = str(uuid.uuid4())
            STATE.save()
        return STATE.d["device_id"]

def signin_start():
    c = tv_client()
    r = post_json("https://www.youtube.com/o/oauth2/device/code",
                  {"client_id": c["client_id"], "device_id": device_id(), "model_name": "ytlr::",
                   "scope": OAUTH_SCOPE})
    if "user_code" not in r:
        raise LookupError("sign-in refused: %s" % (r.get("error_description") or r.get("error") or r))
    with AUTH_LOCK:
        PENDING.clear()
        PENDING.update({"device_code": r["device_code"], "user_code": r["user_code"],
                        "url": r.get("verification_url", "https://www.youtube.com/activate"),
                        "until": time.time() + int(r.get("expires_in", 1800)),
                        "interval": int(r.get("interval", 5))})
    return {"user_code": r["user_code"], "url": PENDING["url"], "expires_in": int(r.get("expires_in", 1800))}

def signin_poll():
    with AUTH_LOCK:
        p = dict(PENDING)
    if not p:
        return {"status": "none"}
    if time.time() > p["until"]:
        with AUTH_LOCK:
            PENDING.clear()
        return {"status": "expired"}
    c = tv_client()
    r = post_json("https://www.youtube.com/o/oauth2/token",
                  {"code": p["device_code"], "client_id": c["client_id"],
                   "client_secret": c["client_secret"], "grant_type": DEVICE_GRANT})
    if r.get("error") in ("authorization_pending", "slow_down"):
        return {"status": "pending"}
    if "refresh_token" not in r:
        with AUTH_LOCK:
            PENDING.clear()
        return {"status": "error", "error": r.get("error_description") or r.get("error") or "refused"}
    with AUTH_LOCK:
        PENDING.clear()
    a = {"refresh_token": r["refresh_token"], "access_token": r.get("access_token", ""),
         "expires": time.time() + int(r.get("expires_in", 3600)) - 60}
    auth_save(a)
    name = account_name()
    if name:
        a["name"] = name
        auth_save(a)
    LIST_CACHE.d.clear()
    return {"status": "done", "name": name}

def access_token():
    a = auth_state()
    if not a.get("refresh_token"):
        return None
    if a.get("access_token") and time.time() < a.get("expires", 0):
        return a["access_token"]
    c = tv_client()
    r = post_json("https://www.youtube.com/o/oauth2/token",
                  {"refresh_token": a["refresh_token"], "client_id": c["client_id"],
                   "client_secret": c["client_secret"], "grant_type": "refresh_token"})
    if "access_token" not in r:
        print("auth: refresh failed:", r.get("error"), file=sys.stderr)
        if r.get("error") == "invalid_grant":      # revoked: signed out
            auth_save(None)
        return None
    a["access_token"] = r["access_token"]
    a["expires"] = time.time() + int(r.get("expires_in", 3600)) - 60
    auth_save(a)
    return a["access_token"]

def innertube(endpoint, body):
    tok = access_token()
    c = tv_client()
    b = {"context": {"client": {"clientName": "TVHTML5", "clientVersion": c["version"], "hl": "en", "gl": "CA"}}}
    b.update(body)
    h = {"Origin": "https://www.youtube.com", "Referer": "https://www.youtube.com/tv"}
    if tok:
        h["Authorization"] = "Bearer " + tok
    return post_json("https://www.youtube.com/youtubei/v1/%s?prettyPrint=false" % endpoint, b, h)

def text_of(o):
    if not isinstance(o, dict):
        return ""
    if "simpleText" in o:
        return o["simpleText"]
    return "".join(r.get("text", "") for r in o.get("runs", []))

def find(o, key):
    if isinstance(o, dict):
        if key in o:
            return o[key]
        for v in o.values():
            r = find(v, key)
            if r is not None:
                return r
    elif isinstance(o, list):
        for v in o:
            r = find(v, key)
            if r is not None:
                return r
    return None

def clock_secs(s):
    try:
        n = 0
        for part in s.strip().split(":"):
            n = n * 60 + int(part)
        return n
    except ValueError:
        return 0

def tile_item(t):
    """a TV tileRenderer -> a card item, or None (shorts, movies, playlists)"""
    we = find(t.get("onSelectCommand", {}), "watchEndpoint")
    vid = (we or {}).get("videoId") or ""
    if len(vid) != 11 or find(t.get("onSelectCommand", {}), "reelWatchEndpoint"):
        return None
    meta = (t.get("metadata") or {}).get("tileMetadataRenderer") or {}
    lines = []
    for ln in meta.get("lines") or []:
        parts = [text_of((i.get("lineItemRenderer") or {}).get("text"))
                 for i in (ln.get("lineRenderer") or {}).get("items") or []]
        s = " ".join(p for p in parts if p and p.strip() not in ("•", "·")).strip()
        if s:
            lines.append(s)
    overlays = ((t.get("header") or {}).get("tileHeaderRenderer") or {}).get("thumbnailOverlays") or []
    badge, style, pct = "", "", None
    for ov in overlays:
        ts = ov.get("thumbnailOverlayTimeStatusRenderer")
        if ts:
            badge, style = text_of(ts.get("text")), ts.get("style", "")
        rp = ov.get("thumbnailOverlayResumePlaybackRenderer")
        if rp:
            pct = rp.get("percentDurationWatched", 0)
    if style == "SHORTS":
        return None
    dur = clock_secs(badge) if ":" in badge else 0
    it = {"id": vid, "title": text_of(meta.get("title")), "channel": lines[0] if lines else "",
          "channel_id": "", "duration": dur, "views": 0, "published": "",
          "info": " · ".join(lines[1:])[:90], "live": style == "LIVE"}
    start = (we or {}).get("startTimeSeconds")
    if pct is not None and dur:
        est = dur * pct / 100.0                       # 100: watched (the TV starts it over)
        # the exact second, when it agrees with the bar (seen: start 1 s at 10%)
        exact = isinstance(start, (int, float)) and pct < 100 and abs(start - est) <= dur * 0.01 + 5
        yt_pos_seen(vid, float(start) if exact else est)
    pos = best_pos(vid)
    if pos is not None:
        it["position"] = pos
    return it

def tiles_in(x, seen, out):
    """every playable tile under x, in order, once each"""
    if isinstance(x, dict):
        if "tileRenderer" in x:
            it = tile_item(x["tileRenderer"])
            if it and it["id"] not in seen:
                seen.add(it["id"])
                out.append(it)
            return
        for v in x.values():
            tiles_in(v, seen, out)
    elif isinstance(x, list):
        for v in x:
            tiles_in(v, seen, out)
    return out

def next_token(x):
    c = find(x, "nextContinuationData")
    return (c or {}).get("continuation")

def page_shelves(d, seen):
    """[[title, items, continuation]] of the shelves in a TV browse page (or a
    sectionListContinuation); title "" when the shelf has none"""
    out = []
    def walk(o):
        if isinstance(o, dict):
            if "shelfRenderer" in o:
                sh = o["shelfRenderer"]
                t = text_of(find(sh.get("headerRenderer") or {}, "title") or {})
                items = tiles_in(sh.get("content"), seen, [])
                if items:
                    out.append([t, items, next_token(sh.get("content"))])
                return
            for k, v in o.items():
                if k != "continuations":
                    walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(d.get("contents") or d.get("continuationContents") or d)
    return out

def tv_shelves(browse_id):
    """[(title, items)] from a signed-in TV browse page.
    FEsubscriptions: YouTube sends "Most relevant", then the chronological feed
    as untitled rows of 3, then Shorts; we keep the feed only (one shelf, a
    second page of rows added). Other pages: titled shelves of 5, each made
    longer with one continuation page (the first few shelves)."""
    d = innertube("browse", {"browseId": browse_id})
    seen = set()
    shelves = page_shelves(d, seen)
    if browse_id == "FEsubscriptions":
        feed = [it for t, items, _ in shelves if not t for it in items]
        tok = next_token(find(d, "sectionListRenderer") or {})
        if tok:
            try:
                more = innertube("browse", {"continuation": tok})
                feed += [it for t, items, _ in page_shelves(more, seen) if not t for it in items]
            except Exception as e:
                print("subscriptions: more:", e, file=sys.stderr)
        return [("Subscriptions", feed)] if feed else []
    out = []
    for i, (t, items, tok) in enumerate(shelves):
        if tok and i < 4:
            try:
                more = innertube("browse", {"continuation": tok})
                items = items + tiles_in(more.get("continuationContents") or more, seen, [])
            except Exception as e:
                print("%s: more: %s" % (browse_id, e), file=sys.stderr)
        out.append((t or "Recommended", items))
    return out

def tv_playlist(browse_id, pages=2):
    """a signed-in playlist page (VLWL Watch later, VLLL Liked): its videos,
    with up to `pages` continuations"""
    d = innertube("browse", {"browseId": browse_id})
    seen = set()
    items = tiles_in(d, seen, [])
    tok = next_token(d)
    for _ in range(pages):
        if not tok:
            break
        try:
            more = innertube("browse", {"continuation": tok})
        except Exception as e:
            print("%s: more: %s" % (browse_id, e), file=sys.stderr)
            break
        items += tiles_in(more.get("continuationContents") or more, seen, [])
        tok = next_token(more.get("continuationContents") or {})
    return items

def history(n):
    """the History tab: signed in with history reporting on, the account's
    YouTube history (FEhistory); otherwise what was played here.
    {items, source: "youtube" | "local"}"""
    if not setting("history_off") and auth_state().get("refresh_token"):
        k = "FEhistory"
        items = LIST_CACHE.get(k)
        try:
            if items is None:
                items = LIST_CACHE.put(k, tv_playlist("FEhistory", pages=3))
            return {"items": with_positions(items[:n]), "source": "youtube"}
        except Exception as e:
            print("history: YouTube's: %r; showing the local one" % e, file=sys.stderr)
    return {"items": with_positions(STATE.recent(n, False)), "source": "local"}

def account_name():
    try:
        d = innertube("account/accounts_list", {"accountReadMask": {"returnOwner": True,
                                                                    "returnBrandAccounts": True,
                                                                    "returnPersonaAccounts": False}})
        n = find(d, "accountName")
        return text_of(n) if isinstance(n, dict) else (n or "")
    except Exception as e:
        print("auth: account name:", e, file=sys.stderr)
        return ""

def account():
    a = auth_state()
    return {"signed_in": bool(a.get("refresh_token")), "name": a.get("name", ""),
            "pending": bool(PENDING)}

def signout():
    """forget the tokens here (the TV's sign-in can also be removed in the
    Google account's security settings)"""
    auth_save(None)
    LIST_CACHE.d.clear()
    return {"ok": True}

# ------------------------------------------------------------------ casting
#
# The phone's YouTube app casts to the TV through ytlounge.py (the screen side
# of YouTube's lounge). The TV long-polls /api/remote/next for what to do and
# posts what it plays to /api/remote/state. Volume commands are not passed on.

REMOTE = None           # ytlounge.Lounge
REMOTE_NAME = "Vibe TV"

def remote_get(key):
    with STATE.lock:
        return (STATE.d.get("remote") or {}).get(key)

def remote_set(key, value):
    with STATE.lock:
        STATE.d.setdefault("remote", {})[key] = value
        STATE.save()

def remote_start():
    REMOTE.start()
    remote_set("enabled", True)
    return {"code": REMOTE.pairing_code(), "name": REMOTE_NAME}

def remote_status():
    return {"enabled": bool(remote_get("enabled")), "running": REMOTE.running(),
            "connected": REMOTE.connected()}

def remote_stop():
    remote_set("enabled", False)
    REMOTE.stop()
    return {"ok": True}

# ------------------------------------------------------------------ images

def fetch_bytes(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}),
                                  timeout=10).read()

def thumb(vid, size, dearrow=False):
    if dearrow and setting("dearrow"):
        t = brand(vid).get("time")
        if t is not None:
            k = (vid, "da", t)
            b = THUMB_CACHE.get(k)
            if b is None:
                try:
                    b = fetch_bytes("%s?%s" % (DA_THUMB, urllib.parse.urlencode({"videoID": vid, "time": t})))
                    if b and (b[:2] == b"\xff\xd8" or b[:4] == b"\x89PNG"):
                        THUMB_CACHE.put(k, b)
                    else:
                        b = None
                except Exception:
                    b = None                    # not rendered yet: YouTube's own
            if b:
                return b
    k = (vid, size)
    b = THUMB_CACHE.get(k)
    if b is None:
        names = ["hq720.jpg", "sddefault.jpg", "hqdefault.jpg"] if size == "l" else ["mqdefault.jpg"]
        for n in names:
            try:
                b = fetch_bytes("https://i.ytimg.com/vi/%s/%s" % (vid, n))
                break
            except Exception:
                b = None
        if b is None:
            return None
        THUMB_CACHE.put(k, b)
    return b

def avatar(cid):
    b = THUMB_CACHE.get(("avatar", cid))
    if b is not None:
        return b
    url = next((c.get("avatar") for c in STATE.follows() if c["id"] == cid and c.get("avatar")), None)
    if not url:
        url = channel(cid, 1).get("avatar")
    if not url:
        return None
    return THUMB_CACHE.put(("avatar", cid), fetch_bytes(url))

# ------------------------------------------------------------------ home

def home():
    shelves = []
    cont = with_positions(STATE.recent(20, unfinished_only=True))    # DeArrow titles too
    if cont:
        shelves.append({"title": "Continue watching", "items": cont})
    if account()["signed_in"]:
        for bid, title in (("FEsubscriptions", "Subscriptions"), ("VLWL", "Watch later"),
                           ("FEwhat_to_watch", "Recommended"), ("VLLL", "Liked videos")):
            k = ("tv", bid)
            got = LIST_CACHE.get(k)
            if got is None:
                try:
                    got = LIST_CACHE.put(k, [(title, tv_playlist(bid))] if bid.startswith("VL")
                                         else tv_shelves(bid))
                except Exception as e:
                    print("home: %s: %s" % (bid, e), file=sys.stderr)
                    got = []
            got = [(t, items) for t, items in got if items and
                   not (bid == "FEwhat_to_watch" and "watch later" in t.lower())]   # its own shelf now
            for t, items in got[:8]:
                shelves.append({"title": t, "items": with_positions(items)[:40]})
    for c in STATE.follows():
        try:
            ch = channel(c["id"], 16)
            if ch["items"]:
                shelves.append({"title": ch["title"], "channel_id": c["id"], "items": ch["items"]})
        except Exception as e:
            print("home: channel %s: %s" % (c["id"], e), file=sys.stderr)
    return {"shelves": shelves}

# ------------------------------------------------------------------ http

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def base(self):
        host = self.headers.get("Host") or "%s:%d" % (ARGS.host, ARGS.port)
        return "http://" + host

    def route(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        p = u.path
        vid = q.get("id", "")
        try:
            if p == "/api/home":
                return self.send(200, home())
            if p == "/api/search":
                return self.send(200, {"items": with_positions(search(q.get("q", ""), min(int(q.get("n", 24)), 120)))})
            if p == "/api/channel":
                return self.send(200, channel(vid, min(int(q.get("n", 48)), 120)))
            if p == "/api/video":
                return self.send(200, details(vid))
            if p == "/api/play":
                return self.send(200, play_info(vid, int(q.get("maxh", 1080)), self.base()))
            if p == "/api/history":
                return self.send(200, history(int(q.get("n", 60))))
            if p == "/api/settings":
                if self.command == "POST" and "dearrow" in q:
                    set_setting("dearrow", q["dearrow"] == "1")
                if self.command == "POST" and "debug_signed_in" in q:
                    set_setting("debug_signed_in", q["debug_signed_in"] == "1")
                    VIDEO_CACHE.d.clear()
                if self.command == "POST" and "history" in q:
                    set_setting("history_off", q["history"] != "1")      # on unless turned off
                if self.command == "POST" and "autoplay" in q:
                    set_setting("autoplay_off", q["autoplay"] != "1")      # on unless turned off
                    LIST_CACHE.d.clear()
                return self.send(200, {"dearrow": setting("dearrow"), "autoplay": not setting("autoplay_off"),
                                       "history": not setting("history_off")})
            if p == "/api/related":
                return self.send(200, {"items": with_positions(ytextra.related(vid))[:30]})
            if p == "/api/comments":
                return self.send(200, {"items": ytextra.comments(vid, min(int(q.get("n", 40)), 60))})
            if p == "/api/account":
                return self.send(200, account())
            if p == "/api/signin/start" and self.command == "POST":
                return self.send(200, signin_start())
            if p == "/api/signin/poll":
                return self.send(200, signin_poll())
            if p == "/api/signout" and self.command == "POST":
                return self.send(200, signout())
            if p == "/api/remote/start" and self.command == "POST":
                return self.send(200, remote_start())
            if p == "/api/remote/status":
                return self.send(200, remote_status())
            if p == "/api/remote/stop" and self.command == "POST":
                return self.send(200, remote_stop())
            if p == "/api/remote/next":
                return self.send(200, REMOTE.next_command(float(q.get("timeout", 25))))
            if p == "/api/remote/state" and self.command == "POST":
                if q.get("state") not in ytlounge.TV_STATES:
                    return self.send(400, {"error": "state: playing, paused, loading or stopped"})
                REMOTE.report(vid, float(q.get("pos") or 0), float(q.get("dur") or 0), q["state"])
                return self.send(200, {"ok": True})
            if p == "/api/following":
                return self.send(200, {"items": STATE.follows()})
            if p == "/api/progress" and self.command == "POST":
                meta = {}
                v = VIDEO_CACHE.get(vid)
                if v:
                    meta = {"title": v.get("title"), "channel": v.get("channel"),
                            "channel_id": v.get("channel_id")}
                pos = float(q.get("pos", 0))
                STATE.set_progress(vid, pos, float(q.get("dur", 0)), meta)
                if pos > 0 and not setting("history_off") and auth_state().get("refresh_token"):
                    tok = access_token()
                    if tok:
                        HISTORY.report(vid, pos, tok, force=q.get("stop") == "1")
                        LIST_CACHE.d.pop("FEhistory", None)
                return self.send(200, {"ok": True})
            if p == "/api/follow" and self.command == "POST":
                on = q.get("on", "1") == "1"
                title = avatar_url = ""
                if on:
                    ch = channel(vid, 1)
                    title, avatar_url = ch["title"], ch.get("avatar", "")
                STATE.follow(vid, on, title, avatar_url)
                return self.send(200, {"ok": True, "followed": on})
            m = re.match(r"^/hls/([\w-]{11})\.m3u8$", p)
            if m:
                t = trimmed_master(m.group(1), int(q.get("maxh", 1080)), self.base())
                return self.send(200 if t else 404, t or "no HLS", "application/x-mpegURL")
            m = re.match(r"^/hlsf/([\w-]{11})/([\w-]{1,24})\.m3u8$", p)
            if m:
                t = dash_playlist(m.group(1), m.group(2))
                return self.send(200 if t else 404, t or "no such format", "application/x-mpegURL")
            m = re.match(r"^/thumb/([\w-]{11})\.jpg$", p)
            if m:
                b = thumb(m.group(1), q.get("s", "m"), q.get("da") == "1")
                return self.send(200, b, "image/jpeg") if b else self.send(404, "none", "text/plain")
            m = re.match(r"^/avatar/([\w-]{24})\.jpg$", p)
            if m:
                b = avatar(m.group(1))
                return self.send(200, b, "image/jpeg") if b else self.send(404, "none", "text/plain")
            return self.send(404, {"error": "not found"})
        except Exception as e:
            print("%s %s: %r" % (self.command, self.path, e), file=sys.stderr)
            msg = str(e).splitlines()[0] if str(e) else e.__class__.__name__
            msg = re.sub(r"\x1b\[[0-9;]*m", "", msg)
            return self.send(500, {"error": msg[:200]})

    def do_GET(self):
        self.route()

    def do_HEAD(self):
        self.route()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        self.route()

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % a))


def main():
    global ARGS, STATE, REMOTE
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--state", default=os.path.join(os.path.expanduser("~"), ".vibe-ythelper.json"))
    ap.add_argument("--js-runtime", default="node")
    ARGS = ap.parse_args()
    STATE = State(ARGS.state)
    REMOTE = ytlounge.Lounge(remote_get, remote_set, name=REMOTE_NAME)
    threading.Thread(target=yt_pos_saver, daemon=True).start()
    if remote_get("enabled"):
        REMOTE.start()
    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), H)
    srv.daemon_threads = True
    print("ythelper on %s:%d, state %s, yt-dlp %s" % (ARGS.host, ARGS.port, ARGS.state,
                                                      yt_dlp.version.__version__), flush=True)
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))      # systemd's stop: leave the lounge first
    try:
        srv.serve_forever()
    finally:
        REMOTE.stop()


if __name__ == "__main__":
    main()
