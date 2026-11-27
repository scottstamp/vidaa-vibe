# Debug aid (run on the helper's host as its user): the shape of a signed-in
# TV browse page: each shelf's header keys/title and tile count, and whether
# there are continuations. Prints no tokens.
#   sudo -u vibeyt /opt/vibe-ythelper/venv/bin/python -I probe_feed.py FEsubscriptions
import json, sys
sys.argv = [sys.argv[0]] + sys.argv[1:]
bid = sys.argv[1] if len(sys.argv) > 1 else "FEsubscriptions"
sys.path.insert(0, "/opt/vibe-ythelper")
import ythelper as Y
class A: js_runtime = "node"; host = "x"; port = 0
Y.ARGS = A
Y.STATE = Y.State("/var/lib/vibe-ythelper/state.json")
d = Y.innertube("browse", {"browseId": bid})
def walk(o, path):
    if isinstance(o, dict):
        if "shelfRenderer" in o:
            sh = o["shelfRenderer"]
            hdr = sh.get("headerRenderer") or {}
            n = json.dumps(sh.get("content")).count('"tileRenderer"')
            print(path[-60:], "| header keys:", list(hdr.keys()),
                  "| title:", Y.text_of(Y.find(hdr, "title") or {})[:40], "| tiles:", n)
            print("    header:", json.dumps(hdr)[:300])
            return
        for k, v in o.items():
            walk(v, path + "/" + k)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            walk(v, path + "[%d]" % i)
walk(d, "")
s = json.dumps(d)
print("continuations:", s.count("continuation"), "nextContinuationData:", s.count("nextContinuationData"),
      "reloadContinuationData:", s.count("reloadContinuationData"))
print("top keys:", list(d.keys()))
