// SPDX-License-Identifier: MPL-2.0
// CYMA-1 in the conditions of its publishing target (run_all.sh --artifact): the packaged page converted to the
// artifact shape (no doctype/html/head/body of its own, wrapped by the host's skeleton), served from an origin of its
// own under the host's Content-Security-Policy, inside a cross-site iframe sandboxed to
// "allow-scripts allow-same-origin allow-popups allow-popups-to-escape-sandbox allow-forms": no allow-downloads, no
// allow-modals, no midi permission. The host's runtime is stubbed the way its type definitions (runtime contract
// 0.2.90, downloads.d.ts) describe it: window.claude carries only use(); use("downloads") resolves, later, a frozen
// namespace whose save({filename, data}) accepts only the allowlisted extensions (gif png jpg jpeg webp mp4 webm mov
// txt json md docx pptx epub csv ttf html svg pdf xlsx zip) and rejects everything else with rejected_extension.
//
// What it asserts, each a line of the log (ok / FAIL), all of them in OUT/artifact_test.json:
//   - the converted page names itself "CYMA-1" first, its <title> within the first 8 KB;
//   - no horizontal page scroll at 1280 px and at 390 px;
//   - Switch on works with a real click: frames arrive, the AudioContext runs, a key plays, the LCD canvas holds a
//     picture, and the captured key is audible;
//   - with MIDI blocked by the frame's permissions policy, the MIDI controls are hidden and nothing alerts;
//   - every link to another origin opens a new tab (target=_blank, rel noopener);
//   - capture goes through the host: every save uses an allowlisted extension, one .zip holds a 24-bit WAV and a
//     .mid, and the .mid's first note lands on an onset in the WAV (they start at the same instant);
//   - Shift held for real (as a person holds it) plus a click latches a key, which stays down after Shift is let go;
//   - the AudioContext comes back after it is suspended under the page (an OS interruption), at the next gesture;
//   - claude.ai's anchor rule (measured 2026-10-24: a top-level #anchor reaches the page only when it is at most 128
//     characters), applied by the test host: a 128-character #cy2 link arrives (on hashchange, and opened from
//     scratch); a 138-character #cy1 link is held back and the page loads untouched, with no error;
//   - no uncaught exception and no console error from the page's own code.
//
//   node tools/artifact_test.mjs DIST_WEB OUTDIR      DIST_WEB: package_web.py's output (index.html, app.wasm, ...)
//
// Needs Google Chrome and python3 (the two static servers). It never touches hardware or real MIDI ports.
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const [DIST, OUTARG] = process.argv.slice(2);
if (!DIST || !OUTARG) { console.log("usage: node tools/artifact_test.mjs DIST_WEB OUTDIR"); process.exit(2); }
// the frozen Sounds vectors: beside the package (DIST/../../tests, for dist/artifact), else this tool's own tree (a
// package written elsewhere, e.g. run_all's share-base one under build/run_all), so the anchor-rule checks always run
const VECTORS = (() => {
  for (const p of [path.join(DIST, "..", "..", "tests", "sounds", "vectors.json"),
    path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "tests", "sounds", "vectors.json")]) {
    try { return JSON.parse(fs.readFileSync(p, "utf8")); } catch { /* the next */ }
  }
  return null;
})();
const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
if (!fs.existsSync(CHROME)) { console.log("no Google Chrome"); process.exit(77); }
const OUT = path.resolve(OUTARG);
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: !!ok, detail: String(detail) });
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail !== "" ? "  (" + detail + ")" : ""}`);
}

// ------------------------------------------------------------------ the artifact shape, and its host
const ART = path.join(OUT, "art"), HOST = path.join(OUT, "host");
fs.mkdirSync(ART); fs.mkdirSync(HOST);
const html = fs.readFileSync(path.join(DIST, "index.html"), "utf8");
const body = html.replace(/<!doctype[^>]*>|<\/?html\b[^>]*>|<\/?head\b[^>]*>|<\/?body\b[^>]*>/gis, "");
// the host's skeleton, as the platform wraps a published page (its own doctype, charset and viewport)
fs.writeFileSync(path.join(ART, "published.html"), '<!doctype html><html><head><meta charset=utf-8>' +
  '<meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover"></head><body>' + body + "</body></html>");
const native = !/<!doctype\b|<\/?html\b|<\/?head\b|<\/?body\b|<meta\b|<link\b/i.test(html);
check("artifact shape: no doctype, html, head, body, meta or link tag of its own", native || !/artifact\/?$/.test(DIST),
  native ? "published as is" : "a full page, converted here (dist/web)");
for (const f of fs.readdirSync(DIST)) if (f !== "index.html") fs.cpSync(path.join(DIST, f), path.join(ART, f), { recursive: true });
const t8 = body.slice(0, 8192).match(/<title>([^<]*)<\/title>/i);
check("artifact shape: <title> in the first 8 KB, and it starts with CYMA-1", t8 && t8[1].trim().startsWith("CYMA-1"), t8 ? t8[1] : "no <title> in the first 8 KB");

const CSP = "default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval' blob: https://cdnjs.cloudflare.com https://cdn.jsdelivr.net; " +
  "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; img-src 'self' data: blob:; " +
  "media-src 'self' blob:; connect-src 'self'; worker-src 'self' blob:";
const SANDBOX = "allow-scripts allow-same-origin allow-popups allow-popups-to-escape-sandbox allow-forms";
const PA = 8100 + Math.floor(Math.random() * 400), PB = PA + 500, DBG = 9300 + Math.floor(Math.random() * 600);
fs.writeFileSync(path.join(HOST, "host.html"), `<!doctype html><meta charset="utf-8"><title>host</title><link rel="icon" href="data:,">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>html,body{margin:0;background:#222}iframe{border:0;width:100vw;height:100vh;display:block}</style>
<iframe id="f" sandbox="${SANDBOX}" allow="autoplay"></iframe>
<script>
// the host's anchor rule, as measured in the real claude.ai frame on 2026-10-24: the top-level #anchor reaches the page
// (location.hash set, hashchange fired) only when it is at most 128 characters; 145 never arrived (129-144 untried)
const PAGE = "http://127.0.0.1:${PB}/published.html", ANCHOR_MAX = 128, f = document.getElementById("f");
window.__fwd = [];
const passes = () => { const a = location.hash.slice(1); window.__fwd.push([a.length, !!a && a.length <= ANCHOR_MAX]); return !!a && a.length <= ANCHOR_MAX; };
f.src = PAGE + (passes() ? location.hash : "");
addEventListener("hashchange", () => { if (passes()) f.src = PAGE + location.hash; });
</script>`);
const SERVE = `import sys, http.server, functools
port, root, csp = int(sys.argv[1]), sys.argv[2], sys.argv[3]
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if csp and self.path.split("?")[0].endswith((".html", "/")):
            self.send_header("Content-Security-Policy", csp)
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=root)).serve_forever()`;
const servers = [spawn("python3", ["-c", SERVE, String(PA), HOST, ""], { stdio: "ignore" }),
                 spawn("python3", ["-c", SERVE, String(PB), ART, CSP], { stdio: "ignore" })];
const prof = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-art-"));
const DL = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-artdl-"));
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${DBG}`, `--user-data-dir=${prof}`, "--no-first-run",
  "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--hide-scrollbars",
  "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--window-size=1280,800", "about:blank"], { stdio: "ignore" });
process.on("exit", () => { try { chrome.kill(); for (const s of servers) s.kill(); } catch { /* */ } });
let ver = null;
for (let i = 0; i < 100 && !ver; i++) { await sleep(100); try { ver = await (await fetch(`http://127.0.0.1:${DBG}/json/version`)).json(); } catch { /* */ } }
if (!ver) { console.log("FAIL chrome did not start"); process.exit(1); }
for (const p of [PA, PB]) for (let i = 0; i < 50; i++) { try { await fetch(`http://127.0.0.1:${p}/`); break; } catch { await sleep(100); } }

const target = await (await fetch(`http://127.0.0.1:${DBG}/json/new?about:blank`, { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let msgId = 0;
const pending = new Map(), listeners = [];
ws.addEventListener("message", (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { const { res, rej } = pending.get(m.id); pending.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); }
  else for (const l of listeners) l(m);
});
const cdp = (method, params = {}, sessionId) => new Promise((res, rej) => { const id = ++msgId; pending.set(id, { res, rej }); ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })); });

// the host's runtime, in the frame before any of its scripts: window.claude.use() only, as the contract promises
const STUB = `(() => {
  const ALLOW = /\\.(gif|png|jpe?g|webp|mp4|webm|mov|txt|json|md|docx|pptx|epub|csv|ttf|html|svg|pdf|xlsx|zip)$/i;
  const saves = window.__cyArtSaves = [];
  const b64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
  const bytesOf = async (d) => typeof d === "string" ? new TextEncoder().encode(d) : d instanceof Blob ? new Uint8Array(await d.arrayBuffer())
    : d instanceof ArrayBuffer ? new Uint8Array(d.slice(0)) : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength)) : null;
  const downloads = Object.freeze({ async save(req) {
    const name = req && req.filename;
    if (typeof name !== "string" || !name || name.length > 512) { saves.push({ name, error: "bad_request" }); throw { code: "bad_request", message: "bad filename" }; }
    if (!ALLOW.test(name)) { saves.push({ name, error: "rejected_extension" }); throw { code: "rejected_extension", message: "extension not allowed: " + name }; }
    const u8 = await bytesOf(req.data);
    if (!u8 || !u8.length) { saves.push({ name, error: "bad_request" }); throw { code: "bad_request", message: "empty data" }; }
    saves.push({ name, size: u8.length, b64: b64(u8) });
    return { status: "saved" };
  } });
  const use = (n) => new Promise((r) => setTimeout(() => r(n === "downloads" ? downloads : null), 30));
  Object.defineProperty(window, "claude", { value: Object.freeze({ use }), configurable: false, writable: false });
})();`;

const problems = [], frames = [];
listeners.push((m) => {
  const tag = m.sessionId ? "frame" : "top";
  if (m.method === "Runtime.exceptionThrown") problems.push(`${tag} exception: ` + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text).split("\n")[0]);
  else if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "assert"))
    problems.push(`${tag} console.error: ` + m.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 200));
  else if (m.method === "Target.attachedToTarget" && m.params.targetInfo.type === "iframe") {
    const sid = m.params.sessionId;
    frames.push(sid);
    (async () => {
      for (const d of ["Runtime.enable", "Log.enable", "Page.enable"]) await cdp(d, {}, sid).catch(() => {});
      await cdp("Page.addScriptToEvaluateOnNewDocument", { source: STUB }, sid).catch(() => {});
      await cdp("Runtime.runIfWaitingForDebugger", {}, sid).catch(() => {});
    })();
  }
});
for (const d of ["Runtime.enable", "Log.enable", "Page.enable"]) await cdp(d);
await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: DL, eventsEnabled: true });
await cdp("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true });

async function evaluate(expr, sess) {
  const r = await cdp("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true }, sess);
  if (r.exceptionDetails) throw new Error("evaluate: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
  return r.result.value;
}
async function navigate(url) {
  const loaded = new Promise((r) => { const l = (m) => { if (m.method === "Page.loadEventFired" && !m.sessionId) { listeners.splice(listeners.indexOf(l), 1); r(); } }; listeners.push(l); });
  await cdp("Page.navigate", { url });
  await Promise.race([loaded, sleep(20000)]);       // (a page that never fires load fails its checks, never hangs the run)
}
const mouse = (type, x, y, extra = {}) => cdp("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1, ...extra });
async function click(x, y, extra = {}) { await mouse("mousePressed", x, y, extra); await sleep(50); await mouse("mouseReleased", x, y, extra); }
const KD = { KeyA: ["a", 65], KeyD: ["d", 68], KeyG: ["g", 71], ShiftLeft: ["Shift", 16] };
const key = (type, code, mods = 0) => { const [k, vk] = KD[code]; return cdp("Input.dispatchKeyEvent", { type, code, key: k, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods, ...(type === "keyDown" && k.length === 1 ? { text: k } : {}) }); };
async function shot(file) { const r = await cdp("Page.captureScreenshot", { format: "png" }); fs.writeFileSync(path.join(OUT, file), Buffer.from(r.data, "base64")); }
async function metrics(w, h) { await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: w < 600 }); }

// ================================================================== 1. load, at 1280 px
await metrics(1280, 800);
await navigate(`http://localhost:${PA}/host.html`);
for (let i = 0; i < 60 && !frames.length; i++) await sleep(100);
const F = frames[frames.length - 1];
if (!F) { check("the artifact frame loaded (a process of its own)", false, "no iframe target"); process.exit(1); }
const ev = (e) => evaluate(e, F);
for (let i = 0; i < 100; i++) { if (await ev("document.readyState === 'complete' && !!window.__cyma").catch(() => false)) break; await sleep(100); }
await ev("document.fonts ? document.fonts.ready.then(() => 1) : 1").catch(() => 1);
await sleep(500);
const st = (e) => ev(`(() => { const c = window.__cyma; return ${e}; })()`);
async function until(e, ms = 5000) { const t0 = Date.now(); while (Date.now() - t0 < ms) { try { if (await st(e)) return true; } catch { /* */ } await sleep(50); } return false; }
const rect = (sel) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; e.scrollIntoView({ block: "center", behavior: "instant" }); const r = e.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2, w: r.width, h: r.height }; })()`);

check("frame: cross-site, sandboxed, its own origin", (await ev("self.origin")) === `http://127.0.0.1:${PB}` && (await evaluate("location.origin")) === `http://localhost:${PA}`);
check("frame: the host's runtime is there (window.claude.use)", await ev("typeof window.claude === 'object' && typeof window.claude.use === 'function'"));
check("title starts with CYMA-1", (await ev("document.title")).startsWith("CYMA-1"), await ev("document.title"));
check("no horizontal page scroll at 1280", await ev("document.documentElement.scrollWidth <= innerWidth"), await ev("document.documentElement.scrollWidth"));
const midiAllowed = await ev("!!(document.featurePolicy && document.featurePolicy.allowsFeature('midi'))");
const ext = await ev(`[...document.querySelectorAll('a[href]')].filter((a) => { try { return new URL(a.href).origin !== location.origin; } catch { return false; } })
  .map((a) => ({ href: a.href, target: a.target, rel: a.rel }))`);
const badLinks = ext.filter((a) => a.target !== "_blank" || !/\bnoopener\b/.test(a.rel));
check("every link to another origin opens a new tab (target=_blank, rel noopener)", ext.length > 0 && badLinks.length === 0,
  badLinks.length ? badLinks.map((a) => `${a.href} target=${a.target || "(none)"} rel=${a.rel || "(none)"}`).join("; ") : `${ext.length} links`);
await shot("artifact_1280_off.png");

// ================================================================== 2. switch on, play
const go = await rect("#go");
await click(go.x, go.y);
check("switched on with a real click", await until("c.S.on === true", 10000));
check("frames arrive", await until("c.frames > 30", 5000), await st("c.frames"));
check("AudioContext running", await until("c.S.ac && c.S.ac.state === 'running'", 3000), await st("c.S.ac && c.S.ac.state"));
await until("c.S.ac.currentTime > 2.6", 5000);
await key("keyDown", "KeyA");
check("the computer keyboard plays (key 7 lit)", await until("c.S.leds && c.S.leds[7] >= 2", 1500), await st("c.S.leds && c.S.leds[7]"));
await sleep(300); await key("keyUp", "KeyA");
await sleep(1200);
// the screen draws: the LCD canvas holds a real picture (many lit pixels, many colours), not a blank or a single fill
const lcd = await ev(`(() => { const cv = document.getElementById("screen"); if (!cv) return null;
  const d = cv.getContext("2d").getImageData(0, 0, cv.width, cv.height).data; let lit = 0; const cols = new Set();
  for (let i = 0; i < d.length; i += 4) { if (d[i] + d[i + 1] + d[i + 2] > 60) lit++; if ((i & 63) === 0) cols.add((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]); }
  return { lit, n: d.length / 4, cols: cols.size }; })()`);
check("the screen draws: the LCD canvas holds a picture", lcd && lcd.lit > lcd.n * 0.2 && lcd.cols > 40,
  lcd ? `${lcd.lit} of ${lcd.n} pixels lit, ${lcd.cols} colours sampled` : "no #screen canvas");
await key("keyDown", "KeyD"); await sleep(300); await key("keyUp", "KeyD");
await sleep(1200);

// ================================================================== 3. MIDI where the frame may not use it
if (midiAllowed) check("MIDI: this frame allows MIDI (nothing to check)", true, "permissions policy allows midi");
else {
  check("MIDI blocked by the frame: the MIDI controls are hidden", await ev("(() => { const w = document.getElementById('miwrap'); return !w || w.hidden || w.offsetParent === null; })()"));
  check("MIDI blocked by the frame: no alert", (await ev("(document.getElementById('alert') || {}).textContent || ''")).trim() === "",
    await ev("(document.getElementById('alert') || {}).textContent || ''"));
}

// ================================================================== 4. capture through the host
// One action (web-1): Capture saves the last minute as one .zip through window.claude's downloads; it waits, disabled,
// until use("downloads") answers. (A page with a Capture menu is walked item by item.)
const items = await ev("[...document.querySelectorAll('#capmenu button[data-cap]')].map((b) => b.dataset.cap)");
for (const it of items) {
  const cap = await rect("#capbtn");
  if (!cap) break;
  await click(cap.x, cap.y); await sleep(200);
  const b = await rect(`#capmenu button[data-cap="${it}"]`);
  if (b) { await click(b.x, b.y); await sleep(2500); }
  await ev("(document.getElementById('capmenu') && (document.getElementById('capmenu').hidden = true), 1)").catch(() => 1);
}
if (!items.length) {
  for (let i = 0; i < 50 && await ev("(() => { const b = document.getElementById('capbtn'); return !b || b.disabled; })()"); i++) await sleep(100);
  const cap = await rect("#capbtn");
  if (cap) { await click(cap.x, cap.y); for (let i = 0; i < 60 && !(await ev("(window.__cyArtSaves || []).length")); i++) await sleep(100); }
}
const saves = await ev("window.__cyArtSaves || []");
const done = saves.filter((s) => !s.error), refused = saves.filter((s) => s.error);
check("capture: it saves through the host (window.claude downloads), every save accepted", done.length > 0 && refused.length === 0,
  `${items.length ? `menu ${items.join(", ")}` : "one Capture action"}; saved ${done.map((s) => `${s.name} (${s.size} B)`).join(", ") || "nothing"}` +
  (refused.length ? `; refused ${refused.map((s) => `${s.name}: ${s.error}`).join(", ")}` : ""));
check("capture: the bar says Saved only after the host accepted", /^Saved /.test(await ev("document.getElementById('barmsg').textContent")),
  await ev("document.getElementById('barmsg').textContent"));
check("capture: nothing went to a plain browser download (the sandbox drops it)", fs.readdirSync(DL).length === 0, fs.readdirSync(DL).join(", "));
function unzip(buf) {                                   // the central directory: name -> bytes (stored or deflated)
  const out = {};
  let e = buf.length - 22;
  while (e >= 0 && buf.readUInt32LE(e) !== 0x06054b50) e--;
  if (e < 0) return null;
  let p = buf.readUInt32LE(e + 16);
  const n = buf.readUInt16LE(e + 10);
  for (let i = 0; i < n; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) return null;
    const method = buf.readUInt16LE(p + 10), csize = buf.readUInt32LE(p + 20), nl = buf.readUInt16LE(p + 28), xl = buf.readUInt16LE(p + 30), cl = buf.readUInt16LE(p + 32);
    const lho = buf.readUInt32LE(p + 42), name = buf.toString("utf8", p + 46, p + 46 + nl);
    const d0 = lho + 30 + buf.readUInt16LE(lho + 26) + buf.readUInt16LE(lho + 28);
    const raw = buf.subarray(d0, d0 + csize);
    out[name] = method === 0 ? raw : method === 8 ? zlib.inflateRawSync(raw) : null;
    p += 46 + nl + xl + cl;
  }
  return out;
}
function wavInfo(b) {
  if (b.toString("ascii", 0, 4) !== "RIFF" || b.toString("ascii", 8, 12) !== "WAVE") return null;
  let p = 12, fmt = null, data = null;
  while (p + 8 <= b.length) { const id = b.toString("ascii", p, p + 4), n = b.readUInt32LE(p + 4); if (id === "fmt ") fmt = b.subarray(p + 8, p + 8 + n); if (id === "data") data = b.subarray(p + 8, p + 8 + n); p += 8 + n + (n & 1); }
  if (!fmt || !data) return null;
  return { ch: fmt.readUInt16LE(2), fs: fmt.readUInt32LE(4), bits: fmt.readUInt16LE(14), data };
}
function midiFirstNote(b) {                             // seconds to the first note-on (tempo events honoured)
  if (b.toString("ascii", 0, 4) !== "MThd") return null;
  const div = b.readUInt16BE(12);
  let p = 14, best = null;
  while (p + 8 <= b.length) {
    const id = b.toString("ascii", p, p + 4), n = b.readUInt32BE(p + 4);
    if (id === "MTrk") {
      let q = p + 8, end = q + n, tick = 0, sec = 0, tempo = 500000, run = 0;
      const vlq = () => { let v = 0, c; do { c = b[q++]; v = (v << 7) | (c & 0x7f); } while (c & 0x80); return v; };
      while (q < end) {
        const dt = vlq(); tick += dt; sec += dt * tempo / 1e6 / div;
        let s = b[q];
        if (s & 0x80) q++; else s = run;
        if (s === 0xff) { const t = b[q++], l = vlq(); if (t === 0x51) tempo = (b[q] << 16) | (b[q + 1] << 8) | b[q + 2]; q += l; continue; }
        if (s === 0xf0 || s === 0xf7) { q += vlq(); continue; }
        run = s;
        const hi = s & 0xf0, d1 = b[q], d2 = hi === 0xc0 || hi === 0xd0 ? 0 : b[q + 1];
        q += hi === 0xc0 || hi === 0xd0 ? 1 : 2;
        if (hi === 0x90 && d2 > 0) { if (best === null || sec < best) best = sec; break; }
      }
    }
    p += 8 + n;
  }
  return best;
}
const zips = done.filter((s) => /\.zip$/i.test(s.name));
let zipOk = false, alignNote = "no .zip saved", wavLevel = null;
for (const z of zips) {
  const files = unzip(Buffer.from(z.b64, "base64"));
  if (!files) { alignNote = `${z.name}: not a readable zip`; continue; }
  const wn = Object.keys(files).find((k) => /\.wav$/i.test(k)), mn = Object.keys(files).find((k) => /\.mid$/i.test(k));
  if (!wn || !mn) { alignNote = `${z.name} holds ${Object.keys(files).join(", ")}`; continue; }
  const w = wavInfo(files[wn]), t = midiFirstNote(files[mn]);
  if (!w || w.bits !== 24 || w.ch !== 2) { alignNote = `${wn}: not 24-bit stereo`; continue; }
  if (t === null) { alignNote = `${mn}: no note-on`; continue; }
  // the WAV's onsets: 10 ms frames whose level jumps 6 dB over the 50 ms before; the .mid's first note must sit on one
  const fr = Math.round(w.fs / 100), nfr = Math.floor(w.data.length / 6 / fr), lv = new Float64Array(nfr);
  for (let i = 0; i < nfr; i++) { let s = 0; for (let j = 0; j < fr; j++) { const o = (i * fr + j) * 6; const v = (w.data.readIntLE(o, 3) + w.data.readIntLE(o + 3, 3)) / 2; s += v * v; } lv[i] = 10 * Math.log10(s / fr + 1); }
  const at = Math.round(t * 100);
  let hit = null;
  for (let i = Math.max(5, at - 6); i <= Math.min(nfr - 1, at + 6); i++) { let b = 0; for (let j = i - 5; j < i; j++) b += lv[j] / 5; if (lv[i] > b + 6 && lv[i] > 40) { hit = i; break; } }
  let pk = 0, ss = 0;                                   // the sound itself: the capture's level
  for (let o = 0; o + 6 <= w.data.length; o += 6) { const v = Math.max(Math.abs(w.data.readIntLE(o, 3)), Math.abs(w.data.readIntLE(o + 3, 3))); if (v > pk) pk = v; ss += v * v; }
  wavLevel = { peak: 20 * Math.log10(pk / 8388607 + 1e-12), rms: 10 * Math.log10(ss / Math.max(1, w.data.length / 6) / 8388607 ** 2 + 1e-24) };
  zipOk = hit !== null;
  alignNote = `${z.name}: ${wn} (${(w.data.length / 6 / w.fs).toFixed(1)} s, 24-bit) and ${mn}; first note at ${t.toFixed(3)} s` +
    (hit !== null ? `, onset in the WAV at ${(hit / 100).toFixed(2)} s` : ", no onset in the WAV within 60 ms of it");
  if (zipOk) break;
}
check("capture: one .zip holds a 24-bit WAV and a .mid that start at the same instant", zipOk, alignNote);
check("sound renders: the captured key is audible (peak > -40 dBFS, below full scale)", wavLevel && wavLevel.peak > -40 && wavLevel.peak < -0.01,
  wavLevel ? `peak ${wavLevel.peak.toFixed(1)} dBFS, RMS ${wavLevel.rms.toFixed(1)} dBFS` : "no WAV");

// ================================================================== 5. Shift held for real, and a click: a latch
const g4 = await ev(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => (e.getAttribute('aria-label') || '').startsWith('G4')); if (!h) return null; h.scrollIntoView({ block: "center", behavior: "instant" }); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
if (g4) {
  await key("keyDown", "ShiftLeft", 8); await sleep(150);
  await click(g4.x, g4.y, { modifiers: 8 }); await sleep(200);
  await key("keyUp", "ShiftLeft"); await sleep(400);
  check("Shift held (a real key) + click latches a key, which stays down after Shift is let go", await until("c.KS[14].latched && c.KS[14].down", 1000),
    await st("JSON.stringify({ latched: c.KS[14].latched, down: c.KS[14].down })"));
  await click(g4.x, g4.y); await sleep(300);
  check("a plain click lets the latched key go", await until("!c.KS[14].latched && !c.KS[14].down", 1000));
} else check("Shift latch: a G4 key to click", false, "no G4 hit target");

// ================================================================== 6. the context suspended under the page
await ev("window.__cyma.S.ac.suspend().then(() => 1)");
await sleep(400);
const kk = await ev(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => (e.getAttribute('aria-label') || '').startsWith('F4')); h.scrollIntoView({ block: "center", behavior: "instant" }); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height * 0.7 }; })()`);
await click(kk.x, kk.y);
await key("keyDown", "KeyG"); await sleep(150); await key("keyUp", "KeyG");
check("the AudioContext comes back after a suspend, at the next gesture", await until("c.S.ac.state === 'running'", 2000), await st("c.S.ac.state"));
await shot("artifact_1280_on.png");

// ================================================================== 6b. Sounds in the publishing frame (CYMA-1 Sounds T6)
// docs/CYMA-1-sounds-spec.md §8.2 (8, 9, 11), §11.1 T6: MIDI blocked -> the FM-1 card explains, offers no Connect and
// never asks for MIDI; Save as file and Save all go through the host's downloads.save as .json; a blocked clipboard
// falls back to selecting the code and saying "press ⌘C"; no share base in a frame -> no Copy link; a link's fragment
// (a plain #anchor token) arrives. Skipped, said so, on a page without Sounds.
if (await ev("!!document.getElementById('sndsave') && !!(window.__cyma && window.__cyma.sounds)")) {
  const sndText = (sel) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.textContent.replace(/\\s+/g, " ").trim() : ""; })()`);
  const btn = (scope, text) => ev(`(() => { const b = [...document.querySelectorAll(${JSON.stringify(scope + " button, " + scope + " [role=menuitem]")})]
    .find((x) => x.offsetParent !== null && x.textContent.replace(/\\s+/g, " ").trim().startsWith(${JSON.stringify(text)})); if (!b) return null;
    b.scrollIntoView({ block: "center", behavior: "instant" }); const r = b.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2, dis: b.disabled }; })()`);
  const press = async (r) => { if (!r || r.dis) return false; await click(r.x, r.y); await sleep(250); return true; };
  await ev(`(() => { window.__cyMidiAsks = []; const real = navigator.requestMIDIAccess;
    try { Object.defineProperty(navigator, "requestMIDIAccess", { configurable: true, value: (o) => { window.__cyMidiAsks.push(o || {}); return real ? real.call(navigator, o) : Promise.reject(new Error("no midi")); } }); } catch { /* */ }
    return 1; })()`);
  // the FM-1 card, hosted: the explanation, no Connect, and nothing asks for MIDI
  const card = await sndText("#fm1card");
  const connectShown = await ev("(() => { const b = document.getElementById('fm1connect'); return !!b && b.offsetParent !== null && !b.closest('[hidden]'); })()");
  const disclose = await btn("#fm1card", "How to open CYMA-1 on your Mac") || await ev(`(() => { const s = [...document.querySelectorAll('#fm1card summary')].find((x) => /How to open CYMA-1 on your Mac/.test(x.textContent)); if (!s) return null; const r = s.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
  if (disclose) { await click(disclose.x, disclose.y); await sleep(250); }
  check("Sounds: MIDI blocked -> the FM-1 card explains why and how (the local page, the command-line tool), with no Connect",
    !midiAllowed && /Sending sounds to your FM-1 needs Web MIDI, which this page can't use here: claude\.ai runs it in a protected frame with no access to MIDI devices\./.test(card) && !connectShown &&
    /cyma_local\.py/.test(await sndText("#fm1card")) && /cyma_sounds\.py --help/.test(await sndText("#fm1card")), midiAllowed ? "this frame allows MIDI" : card.slice(0, 160));
  check("Sounds: nothing asks for MIDI with SysEx in the frame (no permission prompt)", (await ev("window.__cyMidiAsks.filter((o) => o.sysex).length")) === 0, JSON.stringify(await ev("window.__cyMidiAsks")));
  // a sound into My Sounds, then Save as file and Save all as a file: both through the host, as .json
  const before = (await ev("window.__cyArtSaves.length")) | 0;
  await press(await rect("#sndsave"));
  await sleep(300);
  await ev(`(() => { const i = document.querySelector('[role=dialog][aria-labelledby="save-h"] input'); i.focus(); i.select(); return 1; })()`).catch(() => 0);
  await cdp("Input.insertText", { text: "ART BOWL" });
  await sleep(150);
  await press(await btn('[role=dialog][aria-labelledby="save-h"]', "Save to"));
  await sleep(600);
  const inBank = await ev("(window.__cyma.sounds.bank() || []).some((s) => s && s.name === 'ART BOWL')");
  const more = await ev(`(() => { const b = [...document.querySelectorAll('#sndlist [aria-haspopup=menu]')].find((x) => /ART BOWL/.test(x.getAttribute('aria-label') || '')); if (!b) return null; b.scrollIntoView({ block: "center", behavior: "instant" }); const r = b.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
  if (more) { await click(more.x, more.y); await sleep(250); await press(await btn("[role=menu]:not([hidden])", "Save as file")); }
  for (let i = 0; i < 40 && (await ev("window.__cyArtSaves.length")) <= before; i++) await sleep(100);
  await press(await rect("#sndexport"));
  for (let i = 0; i < 40 && (await ev("window.__cyArtSaves.length")) <= before + 1; i++) await sleep(100);
  const ns = (await ev("window.__cyArtSaves")).slice(before);
  const docOf = (s) => { try { return JSON.parse(Buffer.from(s.b64, "base64").toString("utf8")); } catch { return null; } };
  const snd = ns.find((s) => !s.error && /\.cyma1\.json$/.test(s.name)), bnk = ns.find((s) => !s.error && /\.cyma1-bank\.json$/.test(s.name));
  check("Sounds: Save as file goes through the host's downloads.save as .json (a cyma1-sound)", inBank && snd && snd.name === "ART BOWL.cyma1.json" && docOf(snd) && docOf(snd).format === "cyma1-sound",
    `saved ${ns.map((s) => s.name + (s.error ? ` (${s.error})` : "")).join(", ") || "nothing"}${inBank ? "" : "; ART BOWL never reached My Sounds"}`);
  check("Sounds: Save all my sounds as a file goes through it too (a cyma1-bank .json); nothing refused",
    bnk && docOf(bnk) && docOf(bnk).format === "cyma1-bank" && docOf(bnk).sounds.some((s) => s.name === "ART BOWL") && !ns.some((s) => s.error), bnk ? bnk.name : "no bank saved");
  // (only the Sounds files are looked for: a transient "downloads.html" appears here a few seconds after the capture
  // section, on the page before Sounds too, and is not this feature's)
  const plainJson = fs.readdirSync(DL).filter((f) => /\.json/i.test(f));
  check("Sounds: no sound or bank file went to a plain browser download", plainJson.length === 0, plainJson.join(", "));
  // Share in a frame with no share base: no Copy link, the code instead; the clipboard blocked: select it, say ⌘C
  await ev(`(() => { try { Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText: () => Promise.reject(new DOMException("blocked", "NotAllowedError")) } }); } catch { /* */ }
    document.execCommand = () => false; return 1; })()`);
  await press(await rect("#sndshare"));
  await sleep(400);
  const pop = await ev("(() => { const d = [...document.querySelectorAll('[role=dialog]')].find((x) => !x.hidden && /^Share/.test((x.querySelector('h1,h2,h3') || {}).textContent || '')); return d ? d.textContent.replace(/\\s+/g, ' ') : ''; })()");
  const linkBtn = await btn("[role=dialog]:not([hidden])", "Copy link");
  // a package built with --share-base URL (the published artifact's own URL) offers links even in the frame; else none
  const shareBaseOf = () => { try { const m = /\/\*CY_SHARE_BASE:set\*\/("[^"\\]*")/.exec(fs.readFileSync(path.join(DIST, "index.html"), "utf8")); return m ? JSON.parse(m[1]) : ""; } catch { return ""; } };
  const SHARE_BASE = shareBaseOf();
  if (SHARE_BASE) {
    await press(linkBtn);
    await sleep(500);
    const lsel = await ev("(() => { const a = document.activeElement; const t = a && 'selectionStart' in a && a.value ? a.value.slice(a.selectionStart, a.selectionEnd) : String(getSelection()); return t.trim(); })()");
    const ltxt = await ev("document.body.innerText.replace(/\\s+/g, ' ')");
    // (revision 2026-10-24b: the link carries the short cy2 code, which fits claude.ai's 128-character anchor; a page
    // from before it carries cy1)
    const LP = (await ev("typeof window.__cyma.sounds.linkEncode === 'function'")) ? "#cy2." : "#cy1.";
    check(`Sounds: built with a share base, the frame offers Copy link: ${SHARE_BASE}${LP}… (clipboard blocked: the link selected, 'press ⌘C')`,
      !!linkBtn && lsel.startsWith(SHARE_BASE + LP) && /^[A-Za-z0-9._~-]+$/.test(lsel.slice(SHARE_BASE.length + 1)) &&
      (LP === "#cy1." || lsel.length - SHARE_BASE.length - 1 <= 128) &&
      /Your browser blocked copying\. The link is selected: press ⌘C \(Ctrl\+C\) to copy it\./.test(ltxt) && !/Links aren't available in this view/.test(pop),
      linkBtn ? `selected "${lsel.slice(0, 60)}…"` : "no Copy link");
  } else {
    check("Sounds: in a frame with no share base, no Copy link, and the popover says to share the code",
      !linkBtn && /Links aren't available in this view: share the code instead\./.test(pop), linkBtn ? "Copy link is offered" : pop.slice(0, 160));
  }
  await press(await btn("[role=dialog]:not([hidden])", "Copy code"));
  await sleep(500);
  const sel = await ev("(() => { const a = document.activeElement; const t = a && 'selectionStart' in a && a.value ? a.value.slice(a.selectionStart, a.selectionEnd) : String(getSelection()); return t.trim(); })()");
  const txt = await ev("document.body.innerText.replace(/\\s+/g, ' ')");
  check("Sounds: a blocked clipboard -> the code selected, and 'press ⌘C (Ctrl+C)'",
    /Your browser blocked copying\. The code is selected: press ⌘C \(Ctrl\+C\) to copy it\./.test(txt) && /^cy1\.[A-Za-z0-9_-]{12,240}$/.test(sel), `selected "${sel.slice(0, 30)}…"`);
  await cdp("Input.dispatchKeyEvent", { type: "rawKeyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await cdp("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  // a share link's fragment in the frame: a plain #anchor token arrives (by hashchange here), nothing is stored
  const code = await ev("window.__cyma.sounds.codeEncode('FRAME LINK', window.__cyma.sounds.bank().find(Boolean).q)");
  check("Sounds: a code is a plain #anchor token (letters, digits, . _ ~ -)", /^cy1\.[A-Za-z0-9._~-]+$/.test(code), code.slice(0, 24));
  await ev(`(location.hash = ${JSON.stringify("#")} + ${JSON.stringify(code)}, 1)`);
  const arrived = await until("/Playing FRAME LINK from a shared link\\. It isn't in My Sounds yet\\./.test(document.getElementById('sndbanner').textContent)", 4000);
  check("Sounds: a link's fragment arrives in the frame (Keep it / Undo), nothing stored", arrived && !(await ev("window.__cyma.sounds.bank().some((s) => s && s.name === 'FRAME LINK')")), await sndText("#sndbanner"));
  const undo = await rect("#sndundo");
  if (undo) await click(undo.x, undo.y);
  await sleep(400);
  // the host's own anchor rule (revision 2026-10-24b; host.html forwards a top-level #anchor of at most 128 characters,
  // as claude.ai was measured to): a #cy2 link of exactly 128 characters arrives and plays with Keep it / Undo; a cy1
  // link (138 characters) never reaches the page, and the page carries on untouched, with no error
  const VALL = VECTORS;
  const V2 = VALL && VALL.cy2;
  if (V2 && await ev("typeof window.__cyma.sounds.linkEncode === 'function'")) {
    const e128 = V2.edges.longest_that_fits, long1 = VALL.factory[0].code;   // (SINGING BOWL's cy1 code: 138 characters)
    await evaluate(`(location.hash = ${JSON.stringify("#" + e128.code)}, 1)`);
    const got128 = await until("/Playing CATHEDRAL GONG from a shared link\\. It isn't in My Sounds yet\\./.test(document.getElementById('sndbanner').textContent)", 5000);
    const fwd128 = await evaluate("JSON.stringify(window.__fwd.slice(-1))");
    check(`Sounds: claude.ai's anchor rule: a #cy2 link of ${e128.code.length} characters reaches the frame and plays (Keep it / Undo), nothing stored`,
      e128.code.length === 128 && got128 && !!(await rect("#sndkeep")) && !(await ev("window.__cyma.sounds.bank().some((s) => s && s.name === 'CATHEDRAL GONG')")),
      `host forwarded ${fwd128}; ${await sndText("#sndbanner")}`);
    const u2 = await rect("#sndundo");
    if (u2) await click(u2.x, u2.y);
    await sleep(500);
    const before = await sndText("#sndbanner"), stBefore = await ev("window.__cyma.sounds.call('cur').then((r) => r.state >>> 0)");
    const nProb = problems.length;
    await evaluate(`(location.hash = ${JSON.stringify("#" + long1)}, 1)`);
    await sleep(2000);
    const fwdLong = await evaluate("JSON.stringify(window.__fwd.slice(-1))");
    check(`Sounds: a cy1 link (${long1.length} characters after the #) is held back by the host, and the page carries on: no banner, nothing played, no error`,
      long1.length > 128 && fwdLong === JSON.stringify([[long1.length, false]]) && (await ev("location.hash")) === "" &&
      !/from a shared link|damaged/.test(await sndText("#sndbanner")) && (await ev("window.__cyma.sounds.call('cur').then((r) => r.state >>> 0)")) === stBefore &&
      problems.slice(nProb).filter((p) => p.startsWith("frame")).length === 0, `host ${fwdLong}; frame hash "${await ev("location.hash")}"; banner "${await sndText("#sndbanner")}" (before "${before}")`);
    await evaluate("(history.replaceState(null, '', location.pathname), 1)");
  }
  await shot("artifact_sounds_1280.png");
} else check("Sounds: (this page has no Sounds section yet: T6 not run)", true, "skipped");

// ================================================================== 7. 390 px
await metrics(390, 844);
await sleep(600);
check("no horizontal page scroll at 390", await ev("document.documentElement.scrollWidth <= innerWidth"), `${await ev("document.documentElement.scrollWidth")} vs ${await ev("innerWidth")}`);
await shot("artifact_390.png");

// ================================================================== 8. a share link opened from scratch (revision 2026-10-24b)
// The page loads inside the host with the anchor already there (the way a person opens a link someone sent): a #cy2 link
// of 128 characters arrives at load (the engine off: "… was shared with you" and Switch on to hear it); a cy1 link of
// 138 characters is held back by the host, and the page loads as if there were no link at all
{
  const VA = VECTORS;
  const fresh = async (anchor) => {
    await navigate("about:blank");                   // (else a change of the anchor alone is a same-page jump: no load)
    const n0 = frames.length;
    await navigate(`http://localhost:${PA}/host.html#${anchor}`);
    for (let i = 0; i < 80 && frames.length === n0; i++) await sleep(100);
    const sid = frames[frames.length - 1];
    if (frames.length === n0) return null;
    for (let i = 0; i < 100; i++) { if (await evaluate("document.readyState === 'complete' && !!(window.__cyma && window.__cyma.sounds)", sid).catch(() => false)) break; await sleep(100); }
    await sleep(800);
    return (e) => evaluate(e, sid);
  };
  if (VA && VA.cy2 && await ev("!!(window.__cyma && window.__cyma.sounds && window.__cyma.sounds.linkEncode)").catch(() => false)) {
    await metrics(1280, 800);
    const c128 = VA.cy2.edges.longest_that_fits.code, c138 = VA.factory[0].code;
    const e1 = await fresh(c128);
    const b1 = e1 ? await e1("(() => { const b = document.getElementById('sndbanner'); return b && !b.hidden ? b.textContent.replace(/\\s+/g, ' ').trim() : ''; })()") : "";
    const h1 = e1 ? await e1("location.hash") : "";
    check(`Sounds: a #cy2 link of ${c128.length} characters, opened from scratch in the host: it reaches the page at load ("${b1.slice(0, 60)}")`,
      !!e1 && h1 === "#" + c128 && /CATHEDRAL GONG was shared with you/.test(b1), `frame hash ${h1.length - 1} characters`);
    await shot("artifact_link_128.png");
    const e2 = await fresh(c138);
    const b2 = e2 ? await e2("(() => { const b = document.getElementById('sndbanner'); return b && !b.hidden ? b.textContent.replace(/\\s+/g, ' ').trim() : ''; })()") : "?";
    const h2 = e2 ? await e2("location.hash") : "?";
    check(`Sounds: a cy1 link of ${c138.length} characters, opened from scratch: the host holds it back and the page loads as without a link (no banner, no error)`,
      !!e2 && h2 === "" && b2 === "" && JSON.stringify(await evaluate("window.__fwd")) === JSON.stringify([[c138.length, false]]), `frame hash "${h2}", banner "${b2}"`);
  }
}

const own = problems.filter((p) => p.startsWith("frame") && !/favicon|Permissions policy violation/i.test(p));
check("no uncaught exception or console error in the frame", own.length === 0, own.slice(0, 5).join(" | "));
fs.writeFileSync(path.join(OUT, "artifact_test.json"), JSON.stringify({ chrome: ver.Browser, failures, results, saves: saves.map(({ b64, ...s }) => s) }, null, 1));
console.log(failures ? `\n${failures} FAILED (the page in its publishing conditions)` : "\nall passed");
ws.close();
chrome.kill(); for (const s of servers) s.kill();
process.exit(failures ? 1 : 0);
