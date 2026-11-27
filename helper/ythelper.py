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
  /api/history?n=60               videos, most recent first
  /api/following                  channels
  POST /api/progress?id=&pos=&dur=   seconds
  POST /api/follow?id=UC...&on=0|1
  /hls/<id>.m3u8?maxh=1080        the trimmed HLS master
  /thumb/<id>.jpg?s=m|l           video thumbnail (320x180 | 1280x720)
  /avatar/<channel id>.jpg        channel avatar (if known)
"""
import argparse, hashlib, json, os, re, sys, threading, time, urllib.parse, urllib.request
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

import yt_dlp

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
    p = STATE.progress(vid)
    if p:
        it["position"] = p.get("pos", 0)
    return it

def with_positions(items):
    """cached lists carry the positions of when they were made: current ones"""
    out = []
    for it in items:
        it = dict(it)
        p = STATE.progress(it["id"])
        if p:
            it["position"] = p.get("pos", 0)
        out.append(it)
    return out

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

def video(vid):
    v = VIDEO_CACHE.get(vid)
    if v is None:
        v = VIDEO_CACHE.put(vid, extract("https://www.youtube.com/watch?v=" + vid))
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
    return it

# ------------------------------------------------------------------ HLS

def hls_master_url(v):
    for f in v.get("formats") or []:
        if f.get("protocol", "").startswith("m3u8") and f.get("manifest_url"):
            return f["manifest_url"]
    return None

def trimmed_master(vid, maxh):
    """YouTube's master with one variant (and its audio group)"""
    v = video(vid)
    url = hls_master_url(v)
    if not url:
        return None
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
    for l in lines:
        if l.startswith("#EXT-X-INDEPENDENT-SEGMENTS"):
            out.append(l)
        elif l.startswith("#EXT-X-MEDIA") and (group is None or 'GROUP-ID="%s"' % group in l):
            out.append(l)
    out += [inf, uri]
    return "\n".join(out) + "\n"

def play_info(vid, maxh, base):
    v = video(vid)
    live = bool(v.get("is_live"))
    if hls_master_url(v):
        return {"url": "%s/hls/%s.m3u8?maxh=%d" % (base, vid, maxh), "mime": "application/x-mpegURL",
                "duration": int(v.get("duration") or 0), "live": live}
    # no HLS: the muxed 360p file (https; the TV verifies it)
    for f in v.get("formats") or []:
        if f.get("format_id") == "18":
            return {"url": f["url"], "mime": "video/mp4", "duration": int(v.get("duration") or 0),
                    "live": live}
    raise LookupError("no playable format")

# ------------------------------------------------------------------ images

def fetch_bytes(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}),
                                  timeout=10).read()

def thumb(vid, size):
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
    cont = STATE.recent(20, unfinished_only=True)
    if cont:
        shelves.append({"title": "Continue watching", "items": cont})
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
                return self.send(200, {"items": with_positions(search(q.get("q", ""), min(int(q.get("n", 24)), 60)))})
            if p == "/api/channel":
                return self.send(200, channel(vid, min(int(q.get("n", 48)), 120)))
            if p == "/api/video":
                return self.send(200, details(vid))
            if p == "/api/play":
                return self.send(200, play_info(vid, int(q.get("maxh", 1080)), self.base()))
            if p == "/api/history":
                return self.send(200, {"items": STATE.recent(int(q.get("n", 60)), False)})
            if p == "/api/following":
                return self.send(200, {"items": STATE.follows()})
            if p == "/api/progress" and self.command == "POST":
                meta = {}
                v = VIDEO_CACHE.get(vid)
                if v:
                    meta = {"title": v.get("title"), "channel": v.get("channel"),
                            "channel_id": v.get("channel_id")}
                STATE.set_progress(vid, float(q.get("pos", 0)), float(q.get("dur", 0)), meta)
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
                t = trimmed_master(m.group(1), int(q.get("maxh", 1080)))
                return self.send(200 if t else 404, t or "no HLS", "application/x-mpegURL")
            m = re.match(r"^/thumb/([\w-]{11})\.jpg$", p)
            if m:
                b = thumb(m.group(1), q.get("s", "m"))
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
    global ARGS, STATE
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--state", default=os.path.join(os.path.expanduser("~"), ".vibe-ythelper.json"))
    ap.add_argument("--js-runtime", default="node")
    ARGS = ap.parse_args()
    STATE = State(ARGS.state)
    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), H)
    srv.daemon_threads = True
    print("ythelper on %s:%d, state %s, yt-dlp %s" % (ARGS.host, ARGS.port, ARGS.state,
                                                      yt_dlp.version.__version__), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
