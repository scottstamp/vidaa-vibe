#!/usr/bin/env python3
"""
YouTube "Link with TV code" receiver: the screen side of YouTube's lounge
(MDX) protocol, so the YouTube app on a phone can cast to the TV through the
helper. Standard library only.

Ported from SmartTube's MediaServiceCore (MIT), youtubeapi/.../lounge/
(BindManager, BindParams, CommandManager, CommandParams, InfoManager,
LoungeService, models). Where this differs from it, the reason is in a
comment; what was observed on YouTube's servers is in
docs/findings/14-youtube.md.

The protocol, as a screen:
  GET  /api/lounge/pairing/generate_screen_id        -> a screen id (text), kept
  POST /api/lounge/pairing/get_lounge_token_batch    screen_ids=ID -> the lounge token
  POST /api/lounge/pairing/get_pairing_code?ctx=pair -> 12 digits (text) for
       "Link with TV code" in the phone app
  POST /api/lounge/bc/bind?...&RID=n   body count=0  -> a session: its first
       chunk carries ["c", SID] and ["S", gsessionid]
  GET  /api/lounge/bc/bind?...&RID=rpc&CI=0&SID=&gsessionid=&AID=last
       the long poll: a stream of chunks, each a decimal length, a newline and
       a JSON array of [index, [command, arguments...]]; a ["noop"] about
       every 25 s. AID is the index of the last array received.
  POST /api/lounge/bc/bind?...&RID=n&SID=&gsessionid=
       body count=N, ofs=first message number, reqI__sc=event, reqI_key=value:
       what the screen tells the phones (nowPlaying, onStateChange,
       onHasPreviousNextChanged).

Use: Lounge(get_state, set_state).start(); pairing_code(); next_command(t)
for the TV's long poll; report(id, pos, dur, state) with what the TV plays.
The volume commands are ignored on purpose: nothing here changes the TV's
volume.

Stand-alone check (no phone needed): python ytlounge.py [seconds]
"""
import codecs, collections, http.client, json, random, re, socket, sys, threading, time
import urllib.error, urllib.parse, urllib.request, uuid

API = "https://www.youtube.com/api/lounge"
UA = ("Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), "
      "Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)")
APP = "lb-v4"                   # BindParams.APP
ACCESS_TYPE = "permanent"       # BindParams.ACCESS_TYPE
BIND_QUERY = [("device", "LOUNGE_SCREEN"), ("theme", "cl"), ("capabilities", "dsp,mic,dpa"),
              ("mdxVersion", "2"), ("VER", "8"), ("v", "2"), ("t", "1"), ("app", APP)]

READ_TIMEOUT = 70               # a noop comes about every 25 s; silence this long is a dead connection
TOKEN_REFRESH = 6 * 3600        # the token lives 14 days; SmartTube asks again after 30 min
COMMAND_MAX_AGE = 45            # a command the TV did not fetch in this time is dropped
PENDING_SECS = 30               # after a play command, reports about another video are ignored this long
BACKOFF_MIN, BACKOFF_MAX = 3, 60

# what the screen reports in "state" (CommandParams.STATE_*; 3 is "buffering"
# to the phone, SmartTube's STATE_IDLE)
LOUNGE_STATE = {"playing": 1, "paused": 2, "loading": 3}
TV_STATES = ("playing", "paused", "loading", "stopped", "ended")
VIDEO_ID = re.compile(r"^[\w-]{11}$")


class HttpStatus(Exception):
    def __init__(self, code, what):
        Exception.__init__(self, "%s: HTTP %d" % (what, code))
        self.code = code


def _request(url, fields=None, what="request", timeout=20):
    """GET, or POST with form fields; the body as text. Errors never carry
    the URL (it holds the lounge token)."""
    data = urllib.parse.urlencode(fields).encode() if fields is not None else None
    req = urllib.request.Request(url, data=data, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raise HttpStatus(e.code, what) from None
    except urllib.error.URLError as e:
        raise OSError("%s: %s" % (what, e.reason)) from None


class Chunks:
    """The bind channel's framing: `<length>\\n<JSON array>` repeated. The
    arrays are found by parsing, the length is only checked against, so it
    does not matter whether it counts bytes or characters."""

    def __init__(self):
        self.buf = ""
        self.utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.json = json.JSONDecoder()

    def feed(self, data):
        """bytes in -> the list of complete arrays ([[index, [name, ...]], ...])"""
        self.buf += self.utf8.decode(data)
        out = []
        while True:
            s = self.buf.lstrip()
            declared = None
            m = re.match(r"(\d+)[ \t\r]*\n", s)
            if m:
                declared, body = int(m.group(1)), s[m.end():].lstrip()
            elif s[:1].isdigit():
                break                               # the length line is not complete yet
            else:
                body = s
            if not body:
                break
            if body[0] != "[":
                raise ValueError("bind stream: unexpected data")
            try:
                arr, end = self.json.raw_decode(body)
            except ValueError:
                limit = (declared * 4 + 256) if declared is not None else 1 << 20
                if len(body) > limit:
                    raise ValueError("bind stream: chunk does not parse")
                break                               # incomplete: wait for more
            out.append(arr)
            self.buf = body[end:]
        return out


class Session:
    def __init__(self, sid, gsid, token, rid, aid):
        self.sid, self.gsid, self.token = sid, gsid, token
        self.rid = rid          # the last request number used (bind, then each post)
        self.aid = aid          # the last array index received
        self.ofs = 0            # messages posted so far


class Lounge:
    """One screen ("Vibe TV") in YouTube's lounge.

    get_state(key) / set_state(key, value) keep "screen_id" and "device_id"
    (set_state must persist). log(text) gets one-line messages; no token,
    screen id or session id is ever passed to it."""

    def __init__(self, get_state, set_state, name="Vibe TV", log=None):
        self.name = name
        self._get, self._set = get_state, set_state
        self._log = log or (lambda s: print("lounge: " + s, file=sys.stderr, flush=True))
        self._ctl = threading.Lock()            # start/stop
        self._lock = threading.RLock()          # everything below
        self._cv = threading.Condition(self._lock)
        self._tok_lock = threading.Lock()
        self._stop = None                       # the running session's stop Event
        self._threads = []
        self._session = None
        self._conn = None                       # the long poll's connection
        self._out = None                        # events to post: lists of (name, params)
        self._token, self._token_time = None, 0
        self._remotes = {}                      # id -> name of the connected phones
        self._tvq = collections.deque()         # (time, command) for the TV
        self._playlist, self._index, self._list_id = [], -1, None
        self._ctt, self._ctt_vid = None, None
        self._cpn = ""
        self._pending = None                    # (video, until): asked of the TV, not reported by it yet
        self._now = {"id": "", "pos": 0.0, "dur": 0.0, "state": "stopped", "t": time.monotonic()}
        self._sent = None                       # what the phones were last told
        self.trace = None                       # for tests: called with (command, arguments)
        self.stats = {"binds": 0, "polls": 0, "chunks": 0, "noops": 0, "posts": 0, "errors": 0}

    # ------------------------------------------------------------ control

    def start(self):
        with self._ctl:
            if self.running():
                return
            stop = threading.Event()
            out = collections.deque()
            wake = threading.Event()
            with self._lock:
                self._stop, self._out, self._wake = stop, out, wake
            self._threads = [threading.Thread(target=self._run, args=(stop,), name="lounge", daemon=True),
                             threading.Thread(target=self._sender, args=(stop, out, wake), name="lounge-post",
                                              daemon=True)]
            for t in self._threads:
                t.start()

    def stop(self):
        with self._ctl:
            with self._lock:
                stop, s = self._stop, self._session
                if stop is None:
                    return
                stop.set()
                self._stop, self._session = None, None
                self._remotes = {}
                self._sent = None
                self._wake.set()
            self._kick()
            if s:
                self._terminate(s)
            for t in self._threads:
                t.join(5)
            self._threads = []

    def running(self):
        with self._lock:
            return self._stop is not None and any(t.is_alive() for t in self._threads)

    def bound(self):
        """a bind session is open (the phones can reach the screen)"""
        with self._lock:
            return self._session is not None

    def connected(self):
        """names of the phones in the lounge"""
        with self._lock:
            return list(dict.fromkeys(n for n in self._remotes.values() if n))

    # ------------------------------------------------------------ pairing

    def _device_id(self):
        d = self._get("device_id")
        if not d:
            d = str(uuid.uuid4())               # MediaServiceData.getDeviceId: a random UUID, kept
            self._set("device_id", d)
        return d

    def _ensure_token(self, force=False):
        """the lounge token (and the screen id it belongs to); LoungeService.getTokenInfo"""
        with self._tok_lock:
            if self._token and not force and time.time() - self._token_time < TOKEN_REFRESH:
                return self._token
            for attempt in (0, 1):
                sid = self._get("screen_id")
                if not sid:
                    sid = _request(API + "/pairing/generate_screen_id", what="generate_screen_id").strip()
                    if not re.match(r"^[\w-]{8,64}$", sid):
                        raise ValueError("generate_screen_id: unexpected answer")
                    self._set("screen_id", sid)
                    self._log("new screen id")
                body = _request(API + "/pairing/get_lounge_token_batch", {"screen_ids": sid},
                                what="get_lounge_token_batch")
                try:
                    screens = json.loads(body).get("screens") or []
                except ValueError:
                    screens = []
                tok = screens[0].get("loungeToken") if screens else None
                if tok:
                    if screens[0].get("screenId") and screens[0]["screenId"] != sid:
                        self._set("screen_id", screens[0]["screenId"])
                    self._token, self._token_time = tok, time.time()
                    return tok
                if attempt == 0:                # the screen id is not known any more: a new one
                    self._set("screen_id", None)
            raise LookupError("get_lounge_token_batch: no token")

    def pairing_code(self):
        """a fresh code for "Link with TV code", as "1234 5678 9012"
        (BindManager.getPairingCodeV2). SmartTube drops its token after this to
        make its listener start over; the session here is left alone."""
        for attempt in (0, 1):
            tok = self._ensure_token(force=attempt == 1)
            try:
                body = _request(API + "/pairing/get_pairing_code?ctx=pair",
                                {"lounge_token": tok, "screen_id": self._get("screen_id"),
                                 "screen_name": self.name, "access_type": ACCESS_TYPE, "app": APP,
                                 "device_id": self._device_id(), "qr": "1"}, what="get_pairing_code")
            except HttpStatus as e:
                if attempt == 0 and e.code in (400, 401, 403):
                    continue                    # an old token: once more with a new one
                raise
            m = re.search(r'"code"\s*:\s*"?(\d+)', body) or re.match(r"^\s*(\d+)\s*$", body)
            if not m:
                raise LookupError("get_pairing_code: unexpected answer")
            code = m.group(1)
            return " ".join(code[i:i + 4] for i in range(0, len(code), 4))

    def register_pairing_code(self, code):
        """pair with a code the sender made (a DIAL launch's pairingCode);
        InfoManager.registerPairingCode. Not used by the helper's API."""
        self._ensure_token()
        _request(API + "/pairing/register_pairing_code",
                 {"access_type": ACCESS_TYPE, "app": APP, "pairing_code": code,
                  "screen_id": self._get("screen_id"), "screen_name": self.name,
                  "device_id": self._device_id()}, what="register_pairing_code")
        return True

    # ------------------------------------------------------------ the channel

    def _bind_url(self, token, extra):
        q = BIND_QUERY + [("zx", "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))),
                          ("name", self.name), ("id", self._device_id()), ("loungeIdToken", token)] + extra
        return API + "/bc/bind?" + urllib.parse.urlencode(q)

    def _bind(self, stop):
        """open a session (CommandManager.getSessionData). SmartTube sends
        RID=1337 and AID=42 in every request; here RID counts up from a random
        start and AID is what was received, as the channel protocol means them
        (the server honours AID: it sends again what is above it)."""
        token = self._ensure_token()
        rid = random.randint(10000, 99999)
        body = _request(self._bind_url(token, [("RID", str(rid))]), {"count": "0"}, what="bind")
        arrays = [a for chunk in Chunks().feed(body.encode()) for a in chunk]
        sid = gsid = None
        for a in arrays:
            if a[1][0] == "c":
                sid = a[1][1]
            elif a[1][0] == "S":
                gsid = a[1][1]
        if not sid or not gsid:
            raise LookupError("bind: no session in the answer")
        s = Session(sid, gsid, token, rid, -1)
        with self._lock:
            if stop.is_set():
                return None
            self._session = s
            self._sent = None
            self.stats["binds"] += 1
        self._arrays(s, arrays)
        return s

    def _arrays(self, s, arrays):
        for a in arrays:
            try:
                idx, cmd = int(a[0]), a[1]
                name, args = cmd[0], cmd[1:]
            except (TypeError, ValueError, IndexError):
                continue
            if idx <= s.aid:
                continue                        # sent again after a reconnect
            s.aid = idx
            try:
                if self.trace:
                    self.trace(name, args)
                self._command(name, args)
            except Exception as e:
                self._log("command %s: %r" % (name, e))

    def _listen(self, s, stop):
        """one long poll; returns when the server ends it"""
        url = urllib.parse.urlsplit(self._bind_url(s.token, [("RID", "rpc"), ("CI", "0"), ("SID", s.sid),
                                                             ("gsessionid", s.gsid), ("AID", str(s.aid))]))
        conn = http.client.HTTPSConnection(url.netloc, timeout=READ_TIMEOUT)
        with self._lock:
            if stop.is_set():
                return
            self._conn = conn
        try:
            conn.request("GET", url.path + "?" + url.query, headers={"User-Agent": UA})
            resp = conn.getresponse()
            if resp.status != 200:
                raise HttpStatus(resp.status, "bind rpc")
            self.stats["polls"] += 1
            chunks = Chunks()
            while not stop.is_set():
                data = resp.read1(65536)
                if not data:
                    return
                for arr in chunks.feed(data):
                    self.stats["chunks"] += 1
                    self._arrays(s, arr)
        finally:
            with self._lock:
                if self._conn is conn:
                    self._conn = None
            conn.close()

    def _kick(self):
        """end the long poll from another thread (it then reconnects or stops).
        Linux wakes the blocked read at once; Windows only when the next data
        comes (the noop, within 25 s). Nothing depends on it being prompt."""
        with self._lock:
            conn = self._conn
        sock = getattr(conn, "sock", None)
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def _terminate(self, s):
        """tell the server the session is over, so the phones see the screen
        leave at once (the channel's TYPE=terminate; not in SmartTube, which
        just lets the session lapse)"""
        try:
            _request(self._bind_url(s.token, [("RID", str(s.rid + 1)), ("SID", s.sid), ("gsessionid", s.gsid),
                                              ("TYPE", "terminate")]), {"count": "0"}, what="terminate", timeout=5)
        except Exception:
            pass

    def _run(self, stop):
        backoff = BACKOFF_MIN
        while not stop.is_set():
            s = None
            try:
                s = self._bind(stop)
                if s is None:
                    break
                self._log("session open")
                quick = 0
                while not stop.is_set():
                    t0 = time.monotonic()
                    try:
                        self._listen(s, stop)
                    except (OSError, http.client.HTTPException) as e:   # timeouts too
                        if stop.is_set():
                            break
                        if time.monotonic() - t0 < 30:
                            quick += 1
                        self.stats["errors"] += 1
                        self._log("long poll: %s; again" % e.__class__.__name__)
                    else:
                        quick = quick + 1 if time.monotonic() - t0 < 5 else 0
                    if time.monotonic() - t0 >= 30:
                        backoff, quick = BACKOFF_MIN, 0
                    if quick >= 3:
                        raise OSError("long poll keeps ending")
                    with self._lock:
                        if self._session is not s:
                            break               # a post failed: a new session
                        idle = not self._remotes
                    if idle and time.time() - self._token_time > TOKEN_REFRESH:
                        if self._ensure_token() != s.token:
                            break               # a new token: a new session, while nobody is connected
                    stop.wait(0.5 + quick * 2)
            except HttpStatus as e:
                self.stats["errors"] += 1
                if e.code in (401, 403):
                    self._token = None          # expired: SmartTube drops it the same way
                self._log("%s; again in %d s" % (e, backoff))
            except Exception as e:
                self.stats["errors"] += 1
                self._log("%s: %s; again in %d s" % (e.__class__.__name__, e, backoff))
            finally:
                with self._lock:
                    if s is not None and self._session is s:
                        self._session = None
            if stop.wait(backoff):
                break
            backoff = min(backoff * 2, BACKOFF_MAX)

    # ------------------------------------------------------------ events to the phones

    def _post(self, *events):
        """queue events for the phones; call with the lock held"""
        if self._out is not None and self._session is not None:
            self._out.append([e for e in events if e])
            self._wake.set()

    def _sender(self, stop, out, wake):
        while not stop.is_set():
            wake.wait(30)
            wake.clear()
            while out and not stop.is_set():
                events = out.popleft()
                with self._lock:
                    s = self._session
                if s is None or not events:
                    continue
                # CommandParams.packageCommands: count, ofs, reqN__sc, reqN_<field>
                fields = {"count": str(len(events)), "ofs": str(s.ofs)}
                for i, (name, params) in enumerate(events):
                    fields["req%d__sc" % i] = name
                    for k, v in params.items():
                        fields["req%d_%s" % (i, k)] = str(v)
                s.rid += 1
                try:
                    _request(self._bind_url(s.token, [("RID", str(s.rid)), ("SID", s.sid),
                                                      ("gsessionid", s.gsid)]), fields, what="post")
                    s.ofs += len(events)
                    self.stats["posts"] += 1
                except Exception as e:
                    # the numbering is uncertain now: drop the session; the new
                    # one is asked getNowPlaying and gets the current state
                    self.stats["errors"] += 1
                    self._log("%s; new session" % e)
                    with self._lock:
                        if self._session is s:
                            self._session = None
                    out.clear()
                    self._kick()

    def _position(self):
        n = self._now
        pos = n["pos"]
        if n["state"] == "playing":
            pos += time.monotonic() - n["t"]
        if pos < 0 or (n["dur"] > 0 and pos > n["dur"]):
            pos = max(n["dur"], 0.0)            # LoungeService: live streams report odd positions
        return pos

    def _timing(self):
        """CommandParams.getBaseCommand(position, duration)"""
        p = {"currentTime": "%.3f" % self._position(), "loadedTime": "0", "seekableStartTime": "0"}
        if self._now["dur"] > 0:
            p["duration"] = p["seekableEndTime"] = "%.3f" % self._now["dur"]
        return p

    def _ev_now_playing(self):
        """CommandParams.getNowPlaying; with nothing playing, an empty one"""
        n = self._now
        if n["state"] == "stopped" or not n["id"]:
            return ("nowPlaying", {})
        p = self._timing()
        p.update({"videoId": n["id"], "state": LOUNGE_STATE[n["state"]], "cpn": self._cpn})
        if self._ctt and self._ctt_vid == n["id"]:
            p["ctt"] = self._ctt
        if self._list_id and self._index >= 0:
            p["listId"], p["currentIndex"] = self._list_id, self._index
        return ("nowPlaying", p)

    def _ev_state(self):
        """CommandParams.getOnStateChange"""
        p = self._timing()
        p.update({"state": LOUNGE_STATE[self._now["state"]], "cpn": self._cpn})
        return ("onStateChange", p)

    def _ev_prev_next(self):
        """CommandParams.getOnPrevNextChange (always false/false there; here from the playlist)"""
        playing = self._now["state"] != "stopped" and self._index >= 0
        return ("onHasPreviousNextChanged",
                {"hasPrevious": "true" if playing and self._index > 0 else "false",
                 "hasNext": "true" if playing and self._index + 1 < len(self._playlist) else "false"})

    def _send_full(self):
        self._post(self._ev_now_playing(), self._ev_prev_next())
        self._sent = dict(self._now, pos=self._position(), t=time.monotonic())

    # ------------------------------------------------------------ commands from the phones

    def _tv(self, cmd, flush=False):
        if flush:
            self._tvq.clear()
        self._tvq.append((time.monotonic(), cmd))
        self._cv.notify_all()

    def _play(self, vid, pos):
        self._now = {"id": vid, "pos": max(pos, 0.0), "dur": 0.0, "state": "loading", "t": time.monotonic()}
        self._cpn = "".join(random.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
                            for _ in range(16))
        self._pending = (vid, time.monotonic() + PENDING_SECS)
        self._tv({"cmd": "play", "id": vid, "pos": round(max(pos, 0.0), 3)}, flush=True)
        self._send_full()

    def _command(self, name, args):
        a = args[0] if args and isinstance(args[0], dict) else {}
        with self._lock:
            if name == "noop":
                self.stats["noops"] += 1
            elif name == "loungeStatus":
                try:
                    devs = json.loads(a.get("devices") or "[]")
                except ValueError:
                    devs = []
                self._remotes = {d.get("id") or d.get("name"): d.get("name") or "?"
                                 for d in devs if isinstance(d, dict) and d.get("type") == "REMOTE_CONTROL"}
            elif name == "remoteConnected":
                self._remotes[a.get("id") or a.get("name")] = a.get("name") or "?"
                self._log("a phone connected (%d now)" % len(self._remotes))
                self._send_full()
            elif name == "remoteDisconnected":
                self._remotes.pop(a.get("id") or a.get("name"), None)
                self._log("a phone left (%d now)" % len(self._remotes))
            elif name == "getNowPlaying":
                self._send_full()
            elif name in ("setPlaylist", "setVideo"):
                self._set_playlist(name, a)
            elif name == "updatePlaylist":
                ids = [v for v in (a.get("videoIds") or "").split(",") if VIDEO_ID.match(v)]
                self._playlist = ids
                self._list_id = a.get("listId") or self._list_id
                self._index = ids.index(self._now["id"]) if self._now["id"] in ids else -1
                self._post(self._ev_prev_next())
            elif name in ("next", "previous"):
                i = self._index + (1 if name == "next" else -1)
                if self._now["state"] == "stopped" or self._index < 0:
                    pass
                elif 0 <= i < len(self._playlist):
                    self._index = i
                    self._play(self._playlist[i], 0.0)
                elif name == "previous":
                    self._seek(0.0)             # the first one: from its start
            elif name == "seekTo":
                try:
                    self._seek(float(a.get("newTime") or 0))
                except ValueError:
                    pass
            elif name == "pause":
                if self._now["state"] != "stopped":
                    self._tv({"cmd": "pause"})
            elif name == "play":
                if self._now["state"] != "stopped":
                    self._tv({"cmd": "resume"})
                elif 0 <= self._index < len(self._playlist):
                    self._play(self._playlist[self._index], 0.0)    # after its end: once more
            elif name == "stopVideo":
                self._tv({"cmd": "stop"}, flush=True)
                self._pending = None
                self._now = {"id": "", "pos": 0.0, "dur": 0.0, "state": "stopped", "t": time.monotonic()}
                self._send_full()
            # ignored on purpose: setVolume, getVolume (the TV's volume is not
            # ours to change), dpadCommand, voiceCommand, setSubtitlesTrack,
            # getSubtitlesTrack, getDiscoveryDeviceId (DIAL), onUserActivity, ...

    def _seek(self, pos):
        if self._now["state"] == "stopped":
            return
        pos = max(pos, 0.0)
        self._tv({"cmd": "seek", "pos": round(pos, 3)})
        # tell the phones at once (SmartTube's postSeek), the TV's report follows
        self._now.update({"pos": pos, "t": time.monotonic()})
        self._post(self._ev_state())
        self._sent = dict(self._now)

    def _set_playlist(self, name, a):
        vid = a.get("videoId") or ""
        ids = [v for v in (a.get("videoIds") or "").split(",") if VIDEO_ID.match(v)]
        try:
            idx = int(a.get("currentIndex"))
        except (TypeError, ValueError):
            idx = -1
        if not vid and 0 <= idx < len(ids):
            vid = ids[idx]
        if not VIDEO_ID.match(vid):
            return
        try:
            pos = float(a.get("currentTime") or 0)
        except ValueError:
            pos = 0.0
        if name == "setVideo" and not ids:
            ids = self._playlist                # one video of the playlist we have
        else:
            self._list_id = a.get("listId") or None
        if not ids:
            ids = [vid]
        if not (0 <= idx < len(ids) and ids[idx] == vid):
            idx = ids.index(vid) if vid in ids else -1
        self._playlist, self._index = ids, idx
        if a.get("ctt"):
            self._ctt, self._ctt_vid = a["ctt"], vid
        if vid == self._now["id"] and self._now["state"] != "stopped" and self._pending is None:
            # the video the TV already plays (a phone that came back sends its
            # playlist again): do not start it over
            if abs(pos - self._position()) > 10:
                self._seek(pos)
            self._send_full()
            return
        self._play(vid, pos)

    # ------------------------------------------------------------ the TV's side

    def next_command(self, timeout):
        """the next command for the TV, or {"cmd": "none"} after `timeout` seconds"""
        end = time.monotonic() + max(0.0, min(float(timeout), 55.0))
        with self._cv:
            while True:
                while self._tvq:
                    t, cmd = self._tvq.popleft()
                    if time.monotonic() - t <= COMMAND_MAX_AGE:
                        return cmd
                left = end - time.monotonic()
                if left <= 0:
                    return {"cmd": "none"}
                self._cv.wait(left)

    def report(self, vid, pos, dur, state):
        """what the TV plays now: state playing | paused | loading | stopped
        (| ended: stopped at the video's end, the next of the phone's queue is
        started if there is one)"""
        if state not in TV_STATES:
            raise ValueError("state: playing, paused, loading or stopped")
        vid = vid if VIDEO_ID.match(vid or "") else ""
        with self._lock:
            if self._pending:
                # the TV was asked for a video and still tells about the one
                # before (or its stop): not news for the phones
                if vid != self._pending[0] and time.monotonic() < self._pending[1]:
                    return
                self._pending = None
            n = self._now
            if state in ("stopped", "ended") or not vid:
                if vid and n["id"] and vid != n["id"]:
                    return                      # about the video before the one asked for
                if state == "ended" and n["id"] and 0 <= self._index < len(self._playlist) - 1 \
                        and self._playlist[self._index] == n["id"]:
                    self._index += 1
                    self._play(self._playlist[self._index], 0.0)
                    return
                was = n["state"]
                self._now = {"id": "", "pos": 0.0, "dur": 0.0, "state": "stopped", "t": time.monotonic()}
                if was != "stopped" and self._remotes:
                    self._send_full()
                return
            new = vid != n["id"]
            if new:
                self._index = self._playlist.index(vid) if vid in self._playlist else -1
                self._cpn = "".join(random.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
                                    for _ in range(16))
            self._now = {"id": vid, "pos": float(pos), "dur": max(float(dur), 0.0), "state": state,
                         "t": time.monotonic()}
            if not self._remotes:
                return
            last = self._sent
            if new or not last or last.get("id") != vid:
                self._send_full()
                return
            expect = last["pos"] + (time.monotonic() - last["t"] if last["state"] == "playing" else 0)
            if last["state"] != state or abs(last["dur"] - self._now["dur"]) > 1 or abs(float(pos) - expect) > 2:
                self._post(self._ev_state())
                self._sent = dict(self._now)


# ---------------------------------------------------------------- stand-alone check

def _selftest(seconds):
    """screen id, token, pairing code, and the bind channel held open; prints
    no token, id or code digits"""
    state = {}
    lg = Lounge(state.get, state.__setitem__)
    tok = lg._ensure_token()
    print("screen id: %d chars; lounge token: %d chars" % (len(state["screen_id"]), len(tok)))
    code = lg.pairing_code()
    print("pairing code: %s" % re.sub(r"\d", "N", code))
    lg.start()
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(5)
        print("%3d s: running=%s bound=%s connected=%s %s" % (time.time() - t0, lg.running(), lg.bound(),
                                                             lg.connected(), lg.stats), flush=True)
    lg.stop()
    print("stopped: running=%s" % lg.running())
    return 0 if lg.stats["errors"] == 0 and lg.stats["binds"] == 1 else 1


if __name__ == "__main__":
    sys.exit(_selftest(int(sys.argv[1]) if len(sys.argv) > 1 else 60))
