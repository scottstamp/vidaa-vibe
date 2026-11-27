# Debug aid (run on the helper's host as its user): the signed-in TVHTML5
# player request with and without the TV PoToken (ytpot.mjs). Prints only
# outcomes, never tokens.
#   sudo -u vibeyt /opt/vibe-ythelper/venv/bin/python -I probe_tvpot.py VIDEO_ID...
import json, re, subprocess, sys, urllib.request
sys.path.insert(0, "/opt/vibe-ythelper")
import ythelper as Y

class A:
    js_runtime = "node"
Y.ARGS = A
Y.STATE = Y.State("/var/lib/vibe-ythelper/state.json")

c = Y.tv_client()
pot = json.loads(subprocess.run(["node", "--max-http-header-size=131072", "/opt/vibe-ythelper/pot/ytpot.mjs",
                                 c["version"]], capture_output=True, text=True, timeout=60).stdout)
cfg = Y.tv_get("https://www.youtube.com/tv_config?action_get_config=true&client=lb4&theme=cl")
js = re.search(r'"WEB_PLAYER_CONTEXT_CONFIG_ID_LIVING_ROOM_WATCH":\{.*?"jsUrl":"(.*?)"', cfg).group(1).replace("\\/", "/")
pj = Y.tv_get("https://www.youtube.com" + js if js.startswith("/") else js)
sts = int(re.search(r"(?:signatureTimestamp|sts)\s*:\s*(\d{5,})", pj).group(1))
print("client", c["version"], "sts", sts, "pot", len(pot["poToken"]))
tok = Y.access_token()
print("signed in:", bool(tok))

def player(vid, use_pot, auth=True):
    cl = {"clientName": "TVHTML5", "clientVersion": c["version"], "userAgent": Y.TV_UA,
          "hl": "en", "gl": "CA", "visitorData": pot["visitorData"]}
    b = {"context": {"client": cl, "user": {"enableSafetyMode": False, "lockedSafetyMode": False}},
         "videoId": vid, "racyCheckOk": True, "contentCheckOk": True,
         "playbackContext": {"contentPlaybackContext": {"signatureTimestamp": sts}}}
    if use_pot:
        cl["tvAppInfo"] = {"livingRoomPoTokenId": pot["livingRoomId"]}
        b["serviceIntegrityDimensions"] = {"poToken": pot["poToken"]}
    h = {"Origin": "https://www.youtube.com", "Referer": "https://www.youtube.com/tv",
         "X-Goog-Visitor-Id": pot["visitorData"]}
    if auth and tok:
        h["Authorization"] = "Bearer " + tok
    return Y.post_json("https://www.youtube.com/youtubei/v1/player?prettyPrint=false", b, h)

for vid in sys.argv[1:]:
    for use_pot in (False, True):
        r = player(vid, use_pot)
        ps = r.get("playabilityStatus") or {}
        sd = r.get("streamingData") or {}
        af = sd.get("adaptiveFormats") or []
        print(vid, "pot" if use_pot else "nopot", ps.get("status"), (ps.get("reason") or "")[:80],
              "| hls:", bool(sd.get("hlsManifestUrl")), "dash:", bool(sd.get("dashManifestUrl")),
              "adaptive:", len(af), "ciphered:", sum(1 for f in af if "signatureCipher" in f),
              "| tracking:", sorted((r.get("playbackTracking") or {}).keys())[:4])

# can the streams be fetched? (first 64 KB of the best video format; n left as is)
def fetch(u):
    try:
        r = urllib.request.urlopen(urllib.request.Request(u + "&range=0-65535", headers={"User-Agent": Y.TV_UA}), timeout=20)
        return r.status, len(r.read())
    except urllib.error.HTTPError as e:
        return e.code, 0

for vid in sys.argv[1:]:
    for use_pot in (False, True):
        r = player(vid, use_pot)
        af = (r.get("streamingData") or {}).get("adaptiveFormats") or []
        v = max((f for f in af if f.get("mimeType", "").startswith("video")), key=lambda f: f.get("height", 0))
        sd = r.get("streamingData") or {}
        print("streamingData keys:", sorted(sd.keys()), "| format keys:", sorted(v.keys()))
        if "url" not in v:
            continue
        u = v["url"]
        print(vid, "pot" if use_pot else "nopot", v.get("height"), v["mimeType"][:28],
              "plain:", fetch(u), "with &pot:", fetch(u + "&pot=" + pot["poToken"]),
              "| tracking:", sorted((r.get("playbackTracking") or {}).keys()))
