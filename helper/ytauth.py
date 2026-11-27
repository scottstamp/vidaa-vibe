"""Signed-in YouTube for the helper, as SmartTube does it: playback of what
needs an account (age-restricted, members-only) and reporting watches to the
account's history.

SmartTube's signed-in player client is TV_DOWNGRADED (MediaServiceCore:
videoinfo/V2/VideoInfoService.java, common/helpers/AppClient.kt): TVHTML5
5.x with an old Cobalt user agent, the non-TCL player's 5-digit
signatureTimestamp, the visitor data of the /tv page fetched with that user
agent, and the account's OAuth bearer token (the TV device-code sign-in).
It needs no PoToken. Its player response carries the playback-tracking URLs
whose ei/vm/of parameters SmartTube's TrackingService sends with the watch
reports (track/TrackingService.java, TrackingApi.java).

yt-dlp knows the client as "tv_downgraded" but signs requests in only with
browser cookies. A bearer in http_headers goes on every request (the WEB
client's too, which YouTube answers with HTTP 400), and with yt-dlp's own
user agent, version and visitor data the TV player answers "The page needs
to be reloaded" (measured 2026-11-26; findings 14). So, when the YoutubeDL
params carry a bearer under PARAM, the patch below sends the tv_downgraded
player request as SmartTube does. Nothing else changes; if the patch fails
it leaves the request as yt-dlp made it. Use with
    extractor_args youtube: player_client=[CLIENT]
and yt-dlp's default player JS variant ("tv"; its n solver fails on "main").
"""
import json
import random
import re
import sys
import threading
import time
import urllib.parse
import urllib.request

from yt_dlp.extractor.youtube._base import YoutubeBaseInfoExtractor

PARAM = "vibe_bearer"
CLIENT = "tv_downgraded"
# SmartTube's DefaultHeaders.USER_AGENT_COBALT_1 and CLIENTS.TV_DOWNGRADED
UA = "Mozilla/5.0 (DirectFB; Linux x86_64) Cobalt/4.13031-qa (unlike Gecko) Starboard/1"
VERSION = "5.20260901"
PAGE_TTL = 6 * 3600

_lock = threading.Lock()
_page = {"at": 0.0}


def _get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    return urllib.request.urlopen(req, timeout=20).read().decode("utf-8", "replace")


def tv_page():
    """{visitor, sts}: the /tv page's visitor data and its (non-TCL) player's
    signatureTimestamp, fetched with UA; cached"""
    with _lock:
        if _page.get("visitor") and time.time() - _page["at"] < PAGE_TTL:
            return dict(_page)
    page = _get("https://www.youtube.com/tv")
    vd = re.search(r'"visitorData":"(.*?)"', page)
    pu = re.search(r'"player_url":"(.*?)"', page)
    if not vd or not pu:
        raise LookupError("the /tv page has no visitorData or player_url")
    pu = pu.group(1).replace("\\/", "/")
    js = _get("https://www.youtube.com" + pu if pu.startswith("/") else pu)
    sts = re.search(r"(?:signatureTimestamp|sts)\s*:\s*(\d{5,})", js)
    if not sts:
        raise LookupError("no signatureTimestamp in the TV player")
    with _lock:
        _page.update(visitor=vd.group(1), sts=int(sts.group(1)), at=time.time())
        return dict(_page)


# ------------------------------------------------------------------ yt-dlp

_orig = YoutubeBaseInfoExtractor._call_api


def _call_api(self, ep, query, video_id, *args, **kw):
    try:
        tok = self._downloader.params.get(PARAM) if self._downloader else None
        if tok and ep == "player" and kw.get("default_client") == CLIENT and not args:
            vd = tv_page()["visitor"]
            ctx = dict(kw.get("context") or self._extract_context(default_client=CLIENT))
            ctx["client"] = dict(ctx.get("client") or {}, userAgent=UA, clientVersion=VERSION,
                                 clientScreen="WATCH", visitorData=vd)
            ctx["user"] = {"enableSafetyMode": False, "lockedSafetyMode": False}
            h = dict(kw.get("headers") or {})
            h.update({"Authorization": "Bearer " + tok, "User-Agent": UA,
                      "X-YouTube-Client-Version": VERSION, "X-Goog-Visitor-Id": vd,
                      "Referer": "https://www.youtube.com/tv"})
            kw = dict(kw, context=ctx, headers=h)
    except Exception as e:                       # yt-dlp changed: leave the request alone
        print("ytauth: patch skipped: %r" % e, file=sys.stderr)
    return _orig(self, ep, query, video_id, *args, **kw)


if not getattr(YoutubeBaseInfoExtractor._call_api, "_vibe", False):
    _call_api._vibe = True
    YoutubeBaseInfoExtractor._call_api = _call_api


# ------------------------------------------------------------------ history

def player(vid, tok):
    """the signed-in TV_DOWNGRADED player response (only for its metadata and
    tracking URLs; the stream URLs in it are not n-solved)"""
    p = tv_page()
    body = {"context": {"client": {"clientName": "TVHTML5", "clientVersion": VERSION, "userAgent": UA,
                                   "clientScreen": "WATCH", "hl": "en", "visitorData": p["visitor"]},
                        "user": {"enableSafetyMode": False, "lockedSafetyMode": False}},
            "videoId": vid, "racyCheckOk": True, "contentCheckOk": True,
            "playbackContext": {"contentPlaybackContext": {"signatureTimestamp": p["sts"]}}}
    req = urllib.request.Request(
        "https://www.youtube.com/youtubei/v1/player?prettyPrint=false", data=json.dumps(body).encode(),
        method="POST", headers={"User-Agent": UA, "Content-Type": "application/json",
                                "Authorization": "Bearer " + tok, "X-Goog-Visitor-Id": p["visitor"],
                                "Origin": "https://www.youtube.com", "Referer": "https://www.youtube.com/tv"})
    return json.load(urllib.request.urlopen(req, timeout=20))


CPN_ALPHABET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
START_THRESHOLD_S = 180          # SmartTube's: a resume past 3 min starts the record there
RECORD_RENEW_S = 30 * 60
REPORT_EVERY_S = 60


class History:
    """SmartTube's TrackingService: a watch record (/api/stats/playback),
    then watch-time updates (/api/stats/watchtime); within 5% of the end the
    video is marked watched. One video at a time, like the TV."""

    def __init__(self):
        self.lock = threading.Lock()
        self.vid = None
        self.params = None        # ei, vm, of, len
        self.cpn = None
        self.pos = 0.0            # the last reported position
        self.record_at = 0.0
        self.sent_at = 0.0
        self.final = False
        self.busy = False

    def report(self, vid, pos, tok, force=False):
        """called with every position the TV posts (every 10 s); sends at
        most every REPORT_EVERY_S, in a thread"""
        with self.lock:
            if self.busy:
                return
            now = time.time()
            if vid == self.vid and not force and now - self.sent_at < REPORT_EVERY_S:
                return
            self.busy = True
        threading.Thread(target=self._send, args=(vid, pos, tok), daemon=True).start()

    def _send(self, vid, pos, tok):
        try:
            if vid != self.vid:
                pr = player(vid, tok)
                url = ((pr.get("playbackTracking") or {}).get("videostatsWatchtimeUrl") or {}).get("baseUrl")
                if not url:
                    raise LookupError("no tracking URL (%s)" % (pr.get("playabilityStatus") or {}).get("status"))
                q = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
                length = float((pr.get("videoDetails") or {}).get("lengthSeconds") or q.get("len", ["0"])[0])
                self.params = {"ei": q.get("ei", [""])[0], "vm": q.get("vm", [""])[0],
                               "of": q.get("of", [""])[0], "len": length}
                self.vid, self.cpn, self.final, self.record_at = vid, "".join(
                    random.choice(CPN_ALPHABET) for _ in range(16)), False, 0.0
                self.pos = 0.0 if pos < START_THRESHOLD_S else pos
            self._update(vid, pos, tok)
            self.sent_at = time.time()
        except Exception as e:
            print("history %s: %r" % (vid, e), file=sys.stderr)
            self.sent_at = time.time()
        finally:
            with self.lock:
                self.busy = False

    def _get(self, path, q, tok):
        q = dict(q, ns="yt", ver="2", docid=self.vid, cpn=self.cpn,
                 ei=self.params["ei"], vm=self.params["vm"], of=self.params["of"])
        req = urllib.request.Request("https://www.youtube.com/api/stats/%s?%s" % (path, urllib.parse.urlencode(q)),
                                     headers={"User-Agent": UA, "Authorization": "Bearer " + tok})
        st = urllib.request.urlopen(req, timeout=15).status
        if st not in (200, 204):
            raise IOError("%s: HTTP %d" % (path, st))

    def _update(self, vid, pos, tok):
        length = self.params["len"]
        old = self.pos
        if length and length - pos < length * 0.05:
            if not self.final:
                self._get("playback", {"final": "1"}, tok)
                self._get("watchtime", {"final": "1", "st": "%.3f" % length, "et": "%.3f" % length}, tok)
                self.final = True
        else:
            if not self.record_at or time.time() - self.record_at > RECORD_RENEW_S:
                self._get("playback", {"len": "%.3f" % length, "cmt": "%.3f" % old}, tok)
                self.record_at = time.time()
            self._get("watchtime", {"len": "%.3f" % length, "st": "%.3f" % old, "et": "%.3f" % pos,
                                    "cmt": "%.3f" % pos}, tok)
        self.pos = pos
