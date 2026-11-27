# Debug aid (run on the helper's host as its user): does yt-dlp's "tv" client
# accept our OAuth bearer token (signed-in playback, mark-watched)? Prints
# only outcomes, never the token.
#   sudo -u vibeyt /opt/vibe-ythelper/venv/bin/python -I probe_authplay.py VIDEO_ID [mark]
import sys
sys.path.insert(0, "/opt/vibe-ythelper")
import ythelper as Y
import yt_dlp

class A:
    js_runtime = "node"
Y.ARGS = A
Y.STATE = Y.State("/var/lib/vibe-ythelper/state.json")
vid = sys.argv[1]
tok = Y.access_token()
print("token:", bool(tok))

class Log:
    def debug(self, m):
        if "mark" in m.lower() or "watched" in m.lower() or "auth" in m.lower():
            print("dbg:", m[:160])
    def warning(self, m): print("warn:", m[:200])
    def error(self, m): print("err:", m[:200])

o = Y.ydl_opts(extra={"http_headers": {"Authorization": "Bearer " + tok},
                      "extractor_args": {"youtube": {"player_client": ["tv"]}},
                      "mark_watched": len(sys.argv) > 2, "logger": Log(), "verbose": True})
with yt_dlp.YoutubeDL(o) as y:
    try:
        info = y.extract_info("https://www.youtube.com/watch?v=" + vid, download=False)
        fm = [f for f in info.get("formats") or [] if f.get("vcodec") not in (None, "none")]
        print("formats:", len(fm), "hls:", any(f.get("protocol", "").startswith("m3u8") for f in fm),
              "age_limit:", info.get("age_limit"), "availability:", info.get("availability"))
    except Exception as e:
        print("failed:", str(e)[:300])
