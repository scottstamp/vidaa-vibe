// The YouTube TV client's session PoToken, as SmartTube makes it
// (MediaServiceCore: app/potokennp2/TvPoTokenProvider.kt, TvPoTokenWebView4.kt
// and assets/potokennp2/po_token2.html). SmartTube runs BotGuard in an
// Android WebView; here jsdom gives it a window.
//
//   1. tv_config            -> challengeRequestKey (the TV's BotGuard key)
//   2. the /tv page          -> visitorData, LIVING_ROOM_PO_TOKEN_ID
//   3. att/get (TVHTML5)     -> the BotGuard challenge (interpreter + program)
//   4. run it, GenerateIT    -> integrity token; mint over the living-room id
//
// The player request then carries context.client.visitorData,
// context.client.tvAppInfo.livingRoomPoTokenId and
// serviceIntegrityDimensions.poToken (the session token, not per video).
//
// Usage: node ytpot.mjs [CLIENT_VERSION]   -> one JSON line on stdout:
//   {"visitorData","livingRoomId","poToken","expiresAt"}   (expiresAt: unix s)
// Messages go to stderr. Needs jsdom (npm install in this directory).
import { JSDOM } from "jsdom";

const TV_UA = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), " +
              "Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)";
const BG_UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36(KHTML, like Gecko)";
// BotGuard's public API key (SmartTube's GOOGLE_API_KEY)
const BG_KEY = "AIzaSyDyT5W0Jh49F30Pqqtyfdf7pDLFKLJoAnw";
const clientVersion = process.argv[2] || "7.20260901.15.00";

const log = (...a) => console.error("ytpot:", ...a);

async function get(url, ua) {
    const r = await fetch(url, { headers: { "User-Agent": ua, "Accept-Language": "en-US,en;q=0.7" } });
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    return r.text();
}

async function bgPost(url, body, headers = {}) {
    const r = await fetch(url, {
        method: "POST",
        headers: { "User-Agent": BG_UA, "Accept": "application/json",
                   "Content-Type": "application/json+protobuf", "x-goog-api-key": BG_KEY,
                   "x-user-agent": "grpc-web-javascript/0.1", ...headers },
        body,
    });
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    return r.text();
}

function b64ToU8(s, U8 = Uint8Array) {
    return U8.from(Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/").replace(/\./g, "="), "base64"));
}

function u8ToB64(u8) {
    return Buffer.from(Array.from(u8)).toString("base64").replace(/\+/g, "-").replace(/\//g, "_");
}

// po_token2.html's BotGuard runner, evaluated in the jsdom window
const RUNNER = String.raw`
function loadBotGuard(c) {
  var self = { vm: window[c.globalName], vmFunctions: {} };
  if (!self.vm) throw new Error('[BotGuardClient]: VM not found in the global object');
  if (!self.vm.a) throw new Error('[BotGuardClient]: Could not load program');
  var cb = function (asyncSnapshot, shutdown, passEvent, checkCamera) {
    self.vmFunctions = { asyncSnapshotFunction: asyncSnapshot, shutdownFunction: shutdown,
                         passEventFunction: passEvent, checkCameraFunction: checkCamera };
  };
  self.syncSnapshotFunction = self.vm.a(c.program, cb, true, undefined, function () {}, [[], []])[0];
  return new Promise(function (resolve, reject) {
    var i = 0;
    var id = setInterval(function () {
      if (self.vmFunctions.asyncSnapshotFunction) { clearInterval(id); resolve(self); }
      else if (++i >= 10000) { clearInterval(id); reject(new Error('asyncSnapshotFunction is null after 10 s')); }
    }, 1);
  });
}
function runBotGuard(c) {
  new Function(c.interpreterJavascript)();
  var webPoSignalOutput = [];
  return loadBotGuard(c).then(function (bg) {
    return new Promise(function (resolve) {
      bg.vmFunctions.asyncSnapshotFunction(function (r) { resolve(r); },
        [undefined, undefined, webPoSignalOutput, undefined]);
    });
  }).then(function (botguardResponse) {
    return { webPoSignalOutput: webPoSignalOutput, botguardResponse: botguardResponse };
  });
}
`;

async function main() {
    // 1. the TV's BotGuard request key
    const cfg = await get("https://www.youtube.com/tv_config?action_get_config=true&client=lb4&theme=cl", TV_UA);
    const requestKey = (cfg.match(/"challengeRequestKey":"(.*?)"/) || [])[1];
    if (!requestKey) throw new Error("tv_config has no challengeRequestKey");

    // 2. visitor data and the living-room id, from the same /tv page
    const page = await get("https://www.youtube.com/tv", TV_UA);
    const visitorData = (page.match(/"visitorData":"(.*?)"/) || [])[1];
    let livingRoomId = (page.match(/"LIVING_ROOM_PO_TOKEN_ID":"(.*?)"/) || [])[1];
    if (!visitorData || !livingRoomId) throw new Error("the /tv page has no visitorData or LIVING_ROOM_PO_TOKEN_ID");
    livingRoomId = livingRoomId.replace(/\\u003d/g, "=");

    // 3. the challenge for the TV client
    const att = JSON.parse(await bgPost("https://www.youtube.com/youtubei/v1/att/get?prettyPrint=false",
        JSON.stringify({ context: { client: { clientName: "TVHTML5", clientVersion } },
                         engagementType: "ENGAGEMENT_TYPE_UNBOUND" }),
        { "Content-Type": "application/json" }));
    const ch = att.bgChallenge;
    if (!ch || !ch.program || !ch.interpreterUrl) throw new Error("att/get gave no bgChallenge");
    const interpUrl = ch.interpreterUrl.privateDoNotAccessOrElseTrustedResourceUrlWrappedValue;
    const interpreterJavascript = await get("https:" + interpUrl, BG_UA);

    // 4. BotGuard in a window
    const dom = new JSDOM("<!DOCTYPE html><html><head></head><body></body></html>", {
        url: "https://www.youtube.com/", userAgent: BG_UA, pretendToBeVisual: true, runScripts: "outside-only",
    });
    const w = dom.window;
    w.eval(RUNNER);
    const res = await w.runBotGuard({ interpreterJavascript, globalName: ch.globalName, program: ch.program });
    if (!res.webPoSignalOutput.length) throw new Error("webPoSignalOutput is empty");

    const it = JSON.parse(await bgPost("https://www.youtube.com/api/jnn/v1/GenerateIT",
        JSON.stringify([requestKey, res.botguardResponse])));
    const [integrityToken, ttl] = it;
    if (typeof integrityToken !== "string") throw new Error("GenerateIT gave no integrity token");

    const getMinter = res.webPoSignalOutput[0];
    const mint = await getMinter(b64ToU8(integrityToken, w.Uint8Array));
    if (typeof mint !== "function") throw new Error("no mint callback");
    const pot = await mint(w.Uint8Array.from(new TextEncoder().encode(livingRoomId)));
    if (!pot || !pot.length) throw new Error("mint gave nothing");

    // leave 10 minutes of margin, as SmartTube does
    const expiresAt = Math.floor(Date.now() / 1000) + Math.max(60, (ttl || 0) - 600);
    process.stdout.write(JSON.stringify({ visitorData, livingRoomId, poToken: u8ToB64(pot), expiresAt }) + "\n");
    w.close();
}

main().then(() => process.exit(0), (e) => { log(e && e.message || e, e && e.cause ? String(e.cause) : ""); process.exit(1); });
