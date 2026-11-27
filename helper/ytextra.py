"""
Related videos and comments for the TV's YouTube client, from InnerTube's
"next" endpoint as the WEB client, signed out (what youtube.com's watch page
asks for). Standard library only; imported by ythelper.py.

  related(video_id) -> [item]        what YouTube suggests next (videos only)
  comments(video_id, n) -> [comment] top comments: author, text, likes, age

Shapes measured 2026-11-27 (docs/findings/14-youtube.md): related videos are
`lockupViewModel`s under twoColumnWatchNextResults.secondaryResults
(contentId, title, metadataRows: [channel], [views, age]; the duration in
a thumbnailBadgeViewModel); comments come from the comment section's
continuation as `commentEntityPayload` mutations.
"""
import json, re, threading, time, urllib.request

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
_lock = threading.Lock()
_cache = {}          # key -> (time, value)
_TTL = 30 * 60


def _get(key):
    with _lock:
        v = _cache.get(key)
        if v and time.time() - v[0] < _TTL:
            return v[1]
    return None


def _put(key, value):
    with _lock:
        if len(_cache) > 300:
            for k in sorted(_cache, key=lambda k: _cache[k][0])[:100]:
                del _cache[k]
        _cache[key] = (time.time(), value)
    return value


def web_version():
    v = _get("web_version")
    if v:
        return v
    page = urllib.request.urlopen(urllib.request.Request("https://www.youtube.com/", headers={"User-Agent": UA}),
                                  timeout=20).read().decode("utf-8", "replace")
    m = re.search(r'"INNERTUBE_CLIENT_VERSION":"([\d.]+)"', page)
    return _put("web_version", m.group(1) if m else "2.20261125.00.00")


def web_next(body):
    b = {"context": {"client": {"clientName": "WEB", "clientVersion": web_version(), "hl": "en", "gl": "CA"}}}
    b.update(body)
    req = urllib.request.Request("https://www.youtube.com/youtubei/v1/next?prettyPrint=false",
                                 data=json.dumps(b).encode(), method="POST",
                                 headers={"User-Agent": UA, "Content-Type": "application/json",
                                          "Origin": "https://www.youtube.com"})
    return json.load(urllib.request.urlopen(req, timeout=20))


def _find_all(o, key, out):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == key:
                out.append(v)
            else:
                _find_all(v, key, out)
    elif isinstance(o, list):
        for v in o:
            _find_all(v, key, out)
    return out


def _clock(s):
    try:
        n = 0
        for p in s.split(":"):
            n = n * 60 + int(p)
        return n
    except ValueError:
        return 0


def _lockup_item(lv):
    if lv.get("contentType") != "LOCKUP_CONTENT_TYPE_VIDEO" or len(lv.get("contentId", "")) != 11:
        return None
    md = (lv.get("metadata") or {}).get("lockupMetadataViewModel") or {}
    title = ((md.get("title") or {}).get("content")) or ""
    rows = (((md.get("metadata") or {}).get("contentMetadataViewModel") or {}).get("metadataRows")) or []
    lines = []
    for r in rows:
        parts = [((p.get("text") or {}).get("content") or "").strip() for p in r.get("metadataParts") or []]
        parts = [p for p in parts if p]
        if parts:
            lines.append(parts)
    channel = " ".join(lines[0]) if lines else ""
    info = ""
    if len(lines) > 1:
        p = lines[1]
        if p and re.match(r"^[\d.,]+[KMB]?$", p[0]):
            p = [p[0] + " views"] + p[1:]
        info = " · ".join(p)
    badges = _find_all(lv.get("contentImage") or {}, "thumbnailBadgeViewModel", [])
    badge = (badges[0].get("text") if badges else "") or ""
    live = badge.upper() == "LIVE"
    return {"id": lv["contentId"], "title": title, "channel": channel, "channel_id": "",
            "duration": _clock(badge) if ":" in badge else 0, "views": 0, "published": "",
            "info": info[:90], "live": live}


def related(vid):
    r = _get(("related", vid))
    if r is not None:
        return r
    d = web_next({"videoId": vid})
    sec = (((d.get("contents") or {}).get("twoColumnWatchNextResults") or {}).get("secondaryResults") or {})
    out, seen = [], {vid}
    for lv in _find_all(sec, "lockupViewModel", []):
        it = _lockup_item(lv)
        if it and it["id"] not in seen:
            seen.add(it["id"])
            out.append(it)
    _put(("next", vid), d)                    # the comments' continuation comes from here
    return _put(("related", vid), out)


def _comments_token(d):
    """the comment section's continuation, in the watch-next response"""
    for sec in _find_all(d, "itemSectionRenderer", []):
        if sec.get("sectionIdentifier") == "comment-item-section" or sec.get("targetId") == "comments-section":
            toks = _find_all(sec, "continuationCommand", [])
            if toks and toks[0].get("token"):
                return toks[0]["token"]
    return None


def comments(vid, n=40):
    r = _get(("comments", vid))
    if r is not None:
        return r[:n]
    d = _get(("next", vid)) or web_next({"videoId": vid})
    tok = _comments_token(d)
    out = []
    pages = 0
    while tok and len(out) < n and pages < 3:
        pages += 1
        c = web_next({"continuation": tok})
        for m in _find_all(c, "commentEntityPayload", []):
            props = m.get("properties") or {}
            if props.get("replyLevel", 0):
                continue                               # top-level comments only
            out.append({
                "author": ((m.get("author") or {}).get("displayName")) or "",
                "text": ((props.get("content") or {}).get("content")) or "",
                "likes": ((m.get("toolbar") or {}).get("likeCountNotliked")) or "",
                "age": props.get("publishedTime") or "",
                "replies": ((m.get("toolbar") or {}).get("replyCount")) or "",
            })
        # the next page of comments: the last continuation in this page
        nxt = [t.get("token") for t in _find_all(c.get("onResponseReceivedEndpoints") or [], "continuationCommand", [])]
        tok = nxt[-1] if nxt else None
    return _put(("comments", vid), out)[:n]


if __name__ == "__main__":                        # a quick look: python -I ytextra.py VIDEO_ID
    import sys
    v = sys.argv[1] if len(sys.argv) > 1 else "Ip5EiqbuBpE"
    rel = related(v)
    print("related:", len(rel))
    for it in rel[:5]:
        print("  ", it["id"], it["duration"], "|", it["title"][:50], "|", it["channel"][:25], "|", it["info"])
    com = comments(v, 40)
    print("comments:", len(com))
    for c in com[:4]:
        print("  ", c["author"][:20], "|", c["likes"], "|", c["age"], "|", c["text"][:80].replace("\n", " "))
