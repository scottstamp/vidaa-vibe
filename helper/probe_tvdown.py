# Debug aid (run on the helper's host as its user): SmartTube's signed-in
# playback client, TV_DOWNGRADED (TVHTML5 5.x, an old Cobalt user agent, the
# non-TCL player's 5-digit signatureTimestamp). Prints outcomes only.
#   sudo -u vibeyt /opt/vibe-ythelper/venv/bin/python -I probe_tvdown.py VIDEO_ID...
import re, sys
sys.path.insert(0, "/opt/vibe-ythelper")
import urllib.parse, urllib.request
import ythelper as Y

class A:
    js_runtime = "node"
Y.ARGS = A
Y.STATE = Y.State("/var/lib/vibe-ythelper/state.json")

UA = "Mozilla/5.0 (DirectFB; Linux x86_64) Cobalt/4.13031-qa (unlike Gecko) Starboard/1"
def get(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": UA}), timeout=20).read().decode("utf-8", "replace")
page = get("https://www.youtube.com/tv")
pu = re.search(r'"player_url":"(.*?)"', page).group(1).replace("\\/", "/")
vd = re.search(r'"visitorData":"(.*?)"', page).group(1)
pj = get("https://www.youtube.com" + pu if pu.startswith("/") else pu)
sts = int(re.search(r"(?:signatureTimestamp|sts)\s*:\s*(\d{5,})", pj).group(1))
print("player", pu.split("/")[3] if pu.count("/") > 3 else pu, "sts", sts)
tok = Y.access_token()

for vid in sys.argv[1:]:
    for auth in (True, False):
        b = {"context": {"client": {"clientName": "TVHTML5", "clientVersion": "5.20260901", "userAgent": UA,
                                    "clientScreen": "WATCH", "hl": "en", "gl": "CA", "visitorData": vd},
                         "user": {"enableSafetyMode": False, "lockedSafetyMode": False}},
             "videoId": vid, "racyCheckOk": True, "contentCheckOk": True,
             "playbackContext": {"contentPlaybackContext": {"signatureTimestamp": sts}}}
        h = {"User-Agent": UA, "Origin": "https://www.youtube.com", "Referer": "https://www.youtube.com/tv",
             "X-Goog-Visitor-Id": vd}
        if auth and tok:
            h["Authorization"] = "Bearer " + tok
        r = Y.post_json("https://www.youtube.com/youtubei/v1/player?prettyPrint=false", b, h)
        ps = r.get("playabilityStatus") or {}
        sd = r.get("streamingData") or {}
        af = sd.get("adaptiveFormats") or []
        print(vid, "auth" if auth else "anon", ps.get("status"), (ps.get("reason") or "")[:70],
              "| keys:", sorted(sd.keys()), "adaptive:", len(af),
              "url:", sum(1 for f in af if "url" in f), "cipher:", sum(1 for f in af if "signatureCipher" in f),
              "| tracking:", sorted(k for k in (r.get("playbackTracking") or {}) if "videostats" in k))

# do the URLs serve as given (n not transformed)? first 64 KB of the best video and audio
def fetch(u):
    try:
        r = urllib.request.urlopen(urllib.request.Request(u + "&range=0-65535", headers={"User-Agent": UA}), timeout=20)
        return r.status, len(r.read())
    except urllib.error.HTTPError as e:
        return e.code, 0
b["videoId"] = sys.argv[-1]
h["Authorization"] = "Bearer " + tok
r = Y.post_json("https://www.youtube.com/youtubei/v1/player?prettyPrint=false", b, h)
af = r["streamingData"]["adaptiveFormats"]
v = max((f for f in af if f["mimeType"].startswith("video")), key=lambda f: f.get("height", 0))
a = max((f for f in af if f["mimeType"].startswith("audio")), key=lambda f: f.get("bitrate", 0))
print("video", v.get("height"), v["mimeType"][:30], fetch(v["url"]), "n" in urllib.parse.parse_qs(urllib.parse.urlsplit(v["url"]).query))
print("audio", a["mimeType"][:30], fetch(a["url"]))
