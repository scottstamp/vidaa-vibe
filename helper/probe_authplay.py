# Debug aid (run on the helper's host as its user): yt-dlp's tv_downgraded
# client (SmartTube's signed-in playback client) with our OAuth bearer, sent
# on that client's player request only (ytauth.py). Prints only outcomes,
# never the token.
#   sudo -u vibeyt /opt/vibe-ythelper/venv/bin/python -I probe_authplay.py VIDEO_ID
import os, sys
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), "/opt/vibe-ythelper"]
import ythelper as Y
import ytauth
import yt_dlp

class A:
    js_runtime = "node"
Y.ARGS = A
Y.STATE = Y.State("/var/lib/vibe-ythelper/state.json")
vid = sys.argv[1]
tok = Y.access_token()
print("token:", bool(tok))

class Log:
    def debug(self, m): pass
    def warning(self, m): print("warn:", m[:200])
    def error(self, m): print("err:", m[:200])

o = Y.ydl_opts(extra={ytauth.PARAM: tok, "logger": Log(),
                      "extractor_args": {"youtube": {"player_client": [ytauth.CLIENT]}}})
with yt_dlp.YoutubeDL(o) as y:
    try:
        info = y.extract_info("https://www.youtube.com/watch?v=" + vid, download=False)
        fm = info.get("formats") or []
        vf = [f for f in fm if f.get("vcodec") not in (None, "none")]
        print("formats:", len(fm), "video:", len(vf), "max height:", max((f.get("height") or 0) for f in vf),
              "protocols:", sorted({f.get("protocol") for f in fm}), "has 18:", any(f.get("format_id") == "18" for f in fm),
              "age_limit:", info.get("age_limit"), "availability:", info.get("availability"))
        import urllib.request
        f = max(vf, key=lambda f: f.get("height") or 0)
        r = urllib.request.urlopen(urllib.request.Request(f["url"] + "&range=0-65535", headers=f.get("http_headers") or {}), timeout=20)
        print("best video fetch:", r.status, len(r.read()))
    except Exception as e:
        print("failed:", str(e)[:300])
