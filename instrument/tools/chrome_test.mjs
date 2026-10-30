// SPDX-License-Identifier: MPL-2.0
// CYMA-1 in a real browser (M6's acceptance suite, run by tools/run_all.sh --chrome): headless Chrome over the DevTools
// protocol, the page served by python3 -m http.server. Switches on with a real click, plays the computer keyboard and
// the mouse, leans, turns knobs, presses buttons, latches (Shift-click and Alt-click, never with SEL held), captures
// the last minute as one .zip (a 24-bit WAV and its .mid, checked to start together), plays the phrase, the toolbars,
// the big screen, Escape, a suspended context, changes the theme, Factory reset in place, and screenshots the layout
// at 1280 px (1x and 2x; the LCD crisp where a whole multiple fits, smoothed where none does) and 390 px. Then the
// same page in a sandboxed iframe (opaque origin: no storage, no same-origin fetch), at 48 kHz, from file://, the
// unpackaged dev build, and with no app.wasm and no AudioWorklet (the errors must be words, not exceptions). Fails on
// any console error or exception. The page in its publishing target's conditions is tools/artifact_test.mjs.
//
//   node tools/chrome_test.mjs ROOT OUTDIR [HOST_HASH]
//
// ROOT is served at / and holds instrument/dist/web (package_web.py), instrument/build/emu (the --web build),
// instrument/dist-nowasm (package_web.py --no-inline-wasm, its app.wasm deleted) and a writable tests/ (sandbox.html
// goes there). run_all.sh builds that root out of links in APP/build/run_all/chrome_root. HOST_HASH is the host
// build's demo_hash 1 (build/golden/results.json): the page's determinism check must print it.
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { execFileSync } from "node:child_process";
import { pathToFileURL } from "node:url";

const [ROOT, OUT, HOST_HASH] = process.argv.slice(2);
const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
if (!ROOT || !OUT) { console.log("usage: node tools/chrome_test.mjs ROOT OUTDIR [HOST_HASH]"); process.exit(2); }
if (!fs.existsSync(CHROME)) { console.log("no Google Chrome"); process.exit(77); }
const PORT = 8000 + Math.floor(Math.random() * 900), DBG = 9300 + Math.floor(Math.random() * 600);
const DL = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-dl-"));
fs.mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: !!ok, detail: String(detail) });
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail !== "" ? "  (" + detail + ")" : ""}`);
}

// ------------------------------------------------------------------ server and browser
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", ROOT], { stdio: "ignore" });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-chrome-"));
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${DBG}`, `--user-data-dir=${prof}`, "--no-first-run",
  "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--window-size=1280,900", "--hide-scrollbars",
  "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "about:blank"], { stdio: "ignore" });
process.on("exit", () => { try { chrome.kill(); server.kill(); } catch { /* */ } });
let ver = null;
for (let i = 0; i < 100 && !ver; i++) { await sleep(100); try { ver = await (await fetch(`http://127.0.0.1:${DBG}/json/version`)).json(); } catch { /* */ } }
if (!ver) { console.log("FAIL chrome did not start"); process.exit(1); }
for (let i = 0; i < 50; i++) { try { await fetch(`http://127.0.0.1:${PORT}/`); break; } catch { await sleep(100); } }

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
const iframeSessions = [];

const problems = [];                    // console errors, exceptions, failed requests
const contexts = new Map();             // frameId -> execution context id
listeners.push((m) => {
  if (m.method === "Runtime.exceptionThrown") problems.push("exception: " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
  else if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "assert"))
    problems.push("console.error: " + m.params.args.map((a) => a.value ?? a.description).join(" "));
  else if (m.method === "Log.entryAdded" && m.params.entry.level === "error") problems.push("log: " + m.params.entry.text + " " + (m.params.entry.url || ""));
  else if (m.method === "Network.loadingFailed" && !m.params.canceled) problems.push("network: " + m.params.errorText + " " + (m.params.type || ""));
  else if (m.method === "Runtime.executionContextCreated" && m.params.context.auxData?.isDefault) contexts.set(m.params.context.auxData.frameId, m.params.context.id);
  else if (m.method === "Runtime.executionContextsCleared" && !m.sessionId) contexts.clear();
  else if (m.method === "Target.attachedToTarget" && m.params.targetInfo.type === "iframe") {
    const sid = m.params.sessionId;
    iframeSessions.push(sid);
    for (const d of ["Runtime.enable", "Log.enable", "Network.enable"]) cdp(d, {}, sid).catch(() => {});
    cdp("Runtime.runIfWaitingForDebugger", {}, sid).catch(() => {});
  }
});
await cdp("Runtime.enable");
await cdp("Log.enable");
await cdp("Page.enable");
await cdp("Network.enable");
await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: DL, eventsEnabled: true });

// ctx: an execution context id (a frame in this process), or { session } (a frame in its own process)
async function evaluate(expr, ctx) {
  const r = typeof ctx === "object" && ctx
    ? await cdp("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true }, ctx.session)
    : await cdp("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true, ...(ctx ? { contextId: ctx } : {}) });
  if (r.exceptionDetails) throw new Error("evaluate: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
  return r.result.value;
}
async function metrics(w, h, dpr, mobile = false) { await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: dpr, mobile }); }
async function navigate(url) {
  const loaded = new Promise((r) => { const l = (m) => { if (m.method === "Page.loadEventFired") { listeners.splice(listeners.indexOf(l), 1); r(); } }; listeners.push(l); });
  await cdp("Page.navigate", { url });
  await loaded;
  await evaluate("document.fonts ? document.fonts.ready.then(() => 1) : 1");
  await sleep(300);
}
async function shot(name, full = true) {
  if (!full) await evaluate("window.scrollTo(0, 0), 1");
  const r = await cdp("Page.captureScreenshot", { format: "png", captureBeyondViewport: full });
  fs.writeFileSync(path.join(OUT, name), Buffer.from(r.data, "base64"));
}
async function mouse(type, x, y, extra = {}) { await cdp("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1, ...extra }); }
async function click(x, y) { await mouse("mousePressed", x, y); await sleep(40); await mouse("mouseReleased", x, y); }
const KEYDEF = { KeyA: ["a", 65], KeyS: ["s", 83], KeyD: ["d", 68], KeyF: ["f", 70], KeyG: ["g", 71], KeyH: ["h", 72], Space: [" ", 32],
  ArrowRight: ["ArrowRight", 39], ArrowUp: ["ArrowUp", 38], ArrowLeft: ["ArrowLeft", 37], BracketRight: ["]", 221], Digit2: ["2", 50],
  Digit1: ["1", 49], Digit9: ["9", 57], KeyV: ["v", 86], KeyC: ["c", 67], KeyX: ["x", 88], KeyZ: ["z", 90], KeyB: ["b", 66],
  Escape: ["Escape", 27], ShiftLeft: ["Shift", 16], Tab: ["Tab", 9] };
async function key(type, code, modifiers = 0) {
  const [k, vk] = KEYDEF[code];
  await cdp("Input.dispatchKeyEvent", { type, code, key: k, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers, ...(type === "keyDown" && k.length === 1 ? { text: k } : {}) });
}
async function tap(code, ms = 80) { await key("keyDown", code); await sleep(ms); await key("keyUp", code); }
const rect = (sel, ctx) => evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); e.scrollIntoView({ block: "center", behavior: "instant" }); const r = e.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2, w: r.width, h: r.height, top: r.top }; })()`, ctx);
const st = (expr, ctx) => evaluate(`(() => { const c = window.__cyma; return ${expr}; })()`, ctx);
async function until(expr, ms = 5000, ctx) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await st(expr, ctx)) return true; await sleep(50); } return false; }
function report(label) {
  check(`${label}: no console errors, exceptions or failed requests`, problems.length === 0, problems.slice(0, 6).join(" | "));
  problems.length = 0;
}

const BASE = `http://127.0.0.1:${PORT}`;
// the worklet's capture ring, asked directly: how long it is, where its sound starts, its peak (and its t1)
const ringStats = (ctx) => evaluate(`new Promise((res) => { const S = window.__cyma.S, old = S.node.port.onmessage;
  S.node.port.onmessage = (e) => { if (e.data.type !== 'captured') return old(e); S.node.port.onmessage = old;
    const l = e.data.l, r = e.data.r; let f = -1, pk = 0, nan = 0;
    let z = -1;
    for (let i = 0; i < l.length; i++) { if (f < 0 && (l[i] !== 0 || r[i] !== 0)) f = i; if (l[i] !== 0 || r[i] !== 0) z = i; pk = Math.max(pk, Math.abs(l[i]), Math.abs(r[i])); if (l[i] !== l[i]) nan++; }
    let lp = 0; for (let i = Math.max(0, l.length - e.data.sampleRate); i < l.length; i++) lp = Math.max(lp, Math.abs(l[i]), Math.abs(r[i]));
    res({ n: l.length, first: f / e.data.sampleRate, last: (z + 1) / e.data.sampleRate, peakDb: 20 * Math.log10(pk + 1e-12), lastDb: 20 * Math.log10(lp + 1e-12), nan, t1: e.data.t1, ct: S.ac.currentTime }); };
  S.node.port.postMessage({ type: 'capture', seconds: 60 }); })`, ctx);
// ================================================================== 1. dist/web at 1280 px
await metrics(1280, 900, 1);
await navigate(`${BASE}/instrument/dist/web/index.html`);
check("page title", (await evaluate("document.title")).startsWith("CYMA-1"));
check("footer: produced by Team EG & Lizard_Man, the upstream credits kept", await evaluate("(() => { const f = document.querySelector('footer'), c = f && f.querySelector('.credit'); return !!c && c.textContent.trim() === 'Produced by Team EG & Lizard_Man.' && /fm1-base/.test(f.textContent) && /felucca-bsp/.test(f.textContent) && /OFL/.test(f.textContent); })()"));
check("41 LED elements drawn", await evaluate("document.querySelectorAll('#panel rect[fill=\"#FFF1D9\"]').length") === 41, await evaluate("document.querySelectorAll('#panel rect[fill=\"#FFF1D9\"]').length"));
check("hit targets: 8 knobs, 14 buttons, 27 keys", await evaluate("document.querySelectorAll('#controls .hit').length") === 49);
check("worklet inlined", await evaluate("!!document.getElementById('cy-worklet') && document.getElementById('cy-worklet').textContent.includes('registerProcessor')"));
check("no horizontal page scroll at 1280", await evaluate("document.documentElement.scrollWidth <= innerWidth"), await evaluate("document.documentElement.scrollWidth"));
await shot("page_1280_off.png");
report("load 1280");

const go = await rect("#go");
await click(go.x, go.y);
check("switched on", await until("c.S.on === true", 8000));
check("frames arrive", await until("c.frames > 30", 4000), await st("c.frames"));
const fps0 = await st("c.frames"); await sleep(1000); const fps = (await st("c.frames")) - fps0;
check("frame rate ~60 Hz", fps >= 50 && fps <= 66, `${fps} frames/s`);
check("worklet reports CYMA exports", await st("c.S.cy === true"));
check("context at 44.1 kHz", await st("c.S.ac.sampleRate"), await st("c.S.ac.sampleRate"));
check("AudioContext running", await st("c.S.ac.state === 'running'"), await st("c.S.ac.state"));
check("start overlay gone, controls live", await evaluate("document.getElementById('start').hidden && !document.getElementById('controls').inert"));
check("caption from info", await until("document.getElementById('caption').textContent.includes('SINGING BOWL')", 3000), await evaluate("document.getElementById('caption').textContent"));
check("screen painted (canvas not uniform)", await evaluate(`(() => { const c = document.getElementById('screen'), x = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let mn = 1e9, mx = -1; for (let i = 0; i < x.length; i += 4 * 97) { const v = x[i] + x[i + 1] + x[i + 2]; mn = Math.min(mn, v); mx = Math.max(mx, v); } return mx > mn; })()`));
check("HOME LED lit (level 3)", await until("c.S.leds && c.S.leds[27 + 6] === 3", 2000));
check("coach chip 1 shown", await evaluate("!document.getElementById('coach').hidden && document.getElementById('chipn').textContent.startsWith('1')"));

// the computer keyboard: A = key 7 (C4), held, then leaned on with Space
await until("c.S.ac.currentTime > 2.8", 4000);           // past app.c's boot logo (v1.1.1: Team EG's chime, 30 ms to 2.35 s)
const r0 = await ringStats();
check("the boot logo: exact silence until its E at 30 ms, then the chime, and exact silence again from its hand-over at 2.35 s",
      r0.first >= 0.025 && r0.first < 0.06 && r0.last > 1.9 && r0.last <= 2.4 && r0.peakDb > -40 && r0.nan === 0, JSON.stringify(r0));
const tKey = await st("c.S.ac.currentTime");
await key("keyDown", "KeyA");
check("QWERTY A holds key 7", await until("c.KS[7].down", 1000));
check("key 7 LED >= 2 while held", await until("c.S.leds[7] >= 2", 1500), await st("c.S.leds[7]"));
await key("keyDown", "Space");
await sleep(450);
check("Space lean ramps to 1", await st("c.S.lean > 0.99"), await st("c.S.lean.toFixed(2)"));
check("firmware reads the pressure back (press[7] > 64)", await until("c.S.press[7] > 64", 1500), await st("c.S.press[7]"));
check("magenta halo drawn", await evaluate("+document.querySelectorAll('#panel rect[stroke=\"#FF5DA5\"]')[0] !== undefined && [...document.querySelectorAll('#panel rect[stroke=\"#FF5DA5\"]')].some(e => +e.getAttribute('opacity') > 0.5)"));
check("key 7 LED 3 when leaned", await until("c.S.leds[7] === 3", 1500), await st("c.S.leds[7]"));
await shot("page_1280_leaning.png", false);
await key("keyUp", "Space");
await key("keyUp", "KeyA");
const r1 = await ringStats();
check("the note is heard after the logo (the last second's peak), no NaN", r1.lastDb > -50 && r1.lastDb > r0.lastDb + 20 && r1.nan === 0, `logo peak ${r0.peakDb.toFixed(1)} dBFS, the second before the key ${r0.lastDb.toFixed(1)} dBFS, the key's ${r1.lastDb.toFixed(1)} dBFS`);
check("key 7 released", await until("!c.KS[7].down", 1000));
await sleep(500);
check("lean released over 400 ms", await st("c.S.lean === 0"), await st("c.S.lean"));
for (const c of ["KeyS", "KeyD", "KeyF", "KeyG"]) await tap(c, 90);
check("coach advanced after four notes", await until("document.getElementById('chipn').textContent.startsWith('2')", 1500), await evaluate("document.getElementById('chipn').textContent"));

// the mouse: a key struck low (hard), dragged down to lean, released
const keyHit = await evaluate(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label').startsWith('F4')); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, top: r.top, h: r.height }; })()`);
await mouse("mousePressed", keyHit.x, keyHit.top + keyHit.h * 0.85);
check("mouse holds F4 (key 12)", await until("c.KS[12].down", 1000));
const vel12 = await st("c.KS[12].vel");
check("low on the key = hard (vel > 100)", vel12 > 100, `vel ${vel12}`);
for (let i = 1; i <= 8; i++) { await mouse("mouseMoved", keyHit.x, keyHit.top + keyHit.h * 0.85 + i * 8); await sleep(30); }
check("drag down leans (press[12] > 64)", await until("c.S.press[12] > 64", 1500), await st("c.S.press[12]"));
check("coach advanced after the lean", await until("document.getElementById('chipn').textContent.startsWith('3')", 1000), await evaluate("document.getElementById('chipn').textContent"));
await mouse("mouseReleased", keyHit.x, keyHit.top + keyHit.h * 0.85 + 64);
check("mouse released F4", await until("!c.KS[12].down", 1000));
await mouse("mousePressed", keyHit.x, keyHit.top + 2);
await sleep(100);
const velTop = await st("c.KS[12].vel");
await mouse("mouseReleased", keyHit.x, keyHit.top + 2);
check("top edge = soft (vel <= 24)", velTop <= 24, `vel ${velTop}`);

// knobs and buttons: SELECT by arrows, PRESETS by arrow, ALGORITHM by ], K1 by the wheel, ENV by 2
for (let i = 0; i < 16; i++) await tap("ArrowRight", 20);
await tap("ArrowUp");
check("PRESETS: preset 02 HANDPAN", await until("c.S.info && c.S.info.num === 2", 2000), await st("c.S.info && c.S.info.preset"));
await tap("BracketRight");
check("ALGORITHM: exciter changes", await until("c.S.info && c.S.info.exciter !== 'MALLET'", 2000), await st("c.S.info && c.S.info.exciter"));
check("coach done after SELECT, ALGORITHM, PRESETS", await until("document.getElementById('coach').hidden", 1000));
await tap("Digit2");
check("ENV button: page EXCITE", await until("c.S.info && c.S.info.page === 'EXCITE'", 2000), await st("c.S.info && c.S.info.page"));
check("ENV LED 3", await until("c.S.leds[27 + 2] === 3", 1000));
const k1 = await rect("#controls .hit.knob:nth-child(5)");
const sayBefore = await st("c.S.info.say_n");
await cdp("Input.dispatchMouseEvent", { type: "mouseWheel", x: k1.x, y: k1.y, deltaX: 0, deltaY: -100 });
check("K1 wheel turns a parameter (say_n moves)", await until(`c.S.info.say_n !== ${sayBefore}`, 2000), await st("c.S.info.say"));
const ang = await evaluate("document.querySelectorAll('#panel g.ctl')[4].querySelector('g').getAttribute('transform')");
check("K1 indicator turned 60° (2 detents)", /rotate\(60 /.test(ang || ""), ang);
await tap("Digit9");
check("PLAY: frozen, green LED", await until("c.S.info.frozen === 1 && c.S.leds[41] === 3", 2000));
await shot("page_1280_frozen.png", false);
await tap("Digit9");
await until("c.S.info.frozen === 0", 2000);
await tap("Digit1");
const mst = await rect("#controls .hit.knob:nth-child(1)");
const m0 = await st("c.S.master");
await mouse("mousePressed", mst.x, mst.y); for (let i = 1; i <= 5; i++) { await mouse("mouseMoved", mst.x, mst.y + i * 10); await sleep(20); } await mouse("mouseReleased", mst.x, mst.y + 50);
check("MASTER drag down 50 px = -1024", (await st("c.S.master")) === Math.max(0, m0 - 1024), `${m0} -> ${await st("c.S.master")}`);
await tap("KeyV");
check("V: velocity +20", await st("c.S.vel") === 120, await st("c.S.vel"));
await tap("KeyC");
// latches on a key, key 14 (G4): Shift-click (as the page documents) and Alt-click (web-3)
const g4 = await evaluate(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label').startsWith('G4')); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
await mouse("mousePressed", g4.x, g4.y, { modifiers: 8 }); await mouse("mouseReleased", g4.x, g4.y, { modifiers: 8 });
check("Shift-click latches G4", await until("c.KS[14].latched && c.KS[14].down", 1000));
await click(g4.x, g4.y);
check("click again lets it go", await until("!c.KS[14].latched && !c.KS[14].down", 1000));
await mouse("mousePressed", g4.x, g4.y, { modifiers: 1 }); await mouse("mouseReleased", g4.x, g4.y, { modifiers: 1 });
check("Alt-click latches G4 too", await until("c.KS[14].latched && c.KS[14].down", 1000));
await click(g4.x, g4.y);
await until("!c.KS[14].latched && !c.KS[14].down", 1000);
// a real Shift held (SEL down) and a Shift-click on SAVE: SEL lets go in the same message as the latch, so the
// firmware never sees SEL+SAVE (REVERT)
const saveHit = await evaluate(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label') === 'SAVE'); h.scrollIntoView({ block: "center", behavior: "instant" }); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
await evaluate(`(() => { window.__btnlog = []; window.__says = []; const p = window.__cyma.S.node.port, o = p.postMessage.bind(p); p.postMessage = (m, t) => { if (m.type === 'buttons') window.__btnlog.push(m.mask); return o(m, t); };
  const q = p.onmessage; p.onmessage = (e) => { if (e.data.info) { try { window.__says.push(JSON.parse(e.data.info).say); } catch { /* */ } } q(e); }; return 1; })()`);
await key("keyDown", "ShiftLeft", 8); await sleep(150);
await mouse("mousePressed", saveHit.x, saveHit.y, { modifiers: 8 }); await mouse("mouseReleased", saveHit.x, saveHit.y, { modifiers: 8 });
await sleep(150); await key("keyUp", "ShiftLeft"); await sleep(400);
const masks = await evaluate("window.__btnlog"), says = await evaluate("window.__says");
check("a real Shift + Shift-click on SAVE: SAVE latched, never with SEL, no REVERT", masks.includes(128) && !masks.some((m) => (m & 2) && (m & 128)) && !says.some((t) => /REVERT/.test(t)),
      `masks ${JSON.stringify(masks)}; said ${JSON.stringify(says)}`);
await click(saveHit.x, saveHit.y);
await until("!(c.S.leds && c.S.leds[27 + 7] === 3)", 500);
await key("keyDown", "Escape"); await sleep(120); await key("keyUp", "Escape");      // (closes the SAVE dialog the latch opened)
report("playing");

// Web MIDI out (a stand-in port: headless Chrome has no MIDI devices): the FM-1's own notes and pressure go out
await evaluate(`(() => { window.__midiLog = []; window.__cyma.S.midiOutPort = { send(d, t) { window.__midiLog.push([...d, t === undefined ? null : t]); } }; return 1; })()`);
await key("keyDown", "KeyD");                   // key 11, E4 = 64
await key("keyDown", "Space"); await sleep(350); await key("keyUp", "Space");
await sleep(150);
await key("keyUp", "KeyD");
await sleep(300);
const mo = await evaluate("window.__midiLog");
const on64 = mo.find((m) => m[0] === 0x90 && m[1] === 64 && m[2] > 0), at64 = mo.find((m) => m[0] === 0xa0 && m[1] === 64), off64 = mo.find((m) => m[0] === 0x80 && m[1] === 64);
check("MIDI out: note-on, poly AT and note-off reach the port", on64 && at64 && off64, JSON.stringify(mo.slice(0, 6)));
check("MIDI out: sent with timestamps (aligned to what is heard)", mo.some((m) => m[3] !== null), `${mo.filter((m) => m[3] !== null).length} of ${mo.length} timestamped`);
await evaluate("(window.__cyma.S.midiOutPort = null, 1)");
// Web MIDI in, through the page's own handler: PANIC (OCT- and OCT+ together) first, so a voice is the note's
await key("keyDown", "KeyZ"); await key("keyDown", "KeyX"); await sleep(120); await key("keyUp", "KeyZ"); await key("keyUp", "KeyX");
const quiet = await until("c.S.info && c.S.info.voices === 0", 3000);
await evaluate("(window.__cyma.midiIn([0x90, 67, 110]), 1)");
check("MIDI in: a note-on sounds a voice", quiet && await until("c.S.info.voices >= 1", 2000), `voices ${await st("c.S.info.voices")}`);
await evaluate("(window.__cyma.midiIn([0x80, 67, 0]), 1)");
check("MIDI in: SysEx and clock are dropped quietly", await evaluate("(window.__cyma.midiIn([0xF0, 0x7D, 0xF7]), window.__cyma.midiIn([0xF8]), 1)") === 1);
// the determinism check, in an instance of its own
const dh = await rect("#dhbtn");
await click(dh.x, dh.y);
await until("/0x[0-9A-F]{8}/.test(document.getElementById('dhout').textContent)", 15000);
const dht = await evaluate("document.getElementById('dhout').textContent");
check("determinism check: the browser's demo_hash 1", /0x[0-9A-F]{8}/.test(dht) && (!HOST_HASH || dht.includes(HOST_HASH)), `${dht}${HOST_HASH ? " (host " + HOST_HASH + ")" : ""}`);
check("the playing engine is not disturbed by it", await st("c.S.on && c.frames > 0") && await st("c.S.ac.state === 'running'"));
report("midi and determinism");

// capture: one action, a .zip holding the 24-bit WAV and the .mid of the same window, starting together (web-1, web-6)
const r2 = await ringStats();
console.log("     ring before the capture:", JSON.stringify(r2));
const cap = await rect("#capbtn");
await click(cap.x, cap.y);
let zips = [];
for (let i = 0; i < 40 && !zips.length; i++) { await sleep(100); zips = fs.readdirSync(DL).filter((f) => f.endsWith(".zip")); }
const barmsg = await evaluate("document.getElementById('barmsg').textContent");
check("capture: one .zip downloaded", zips.length === 1, zips.join(", "));
check("capture: the bar does not claim \"Saved\" for a download link", /^Downloading .*\.zip/.test(barmsg) && !/^Saved/.test(barmsg), barmsg);
if (zips.length) {
  const zp = path.join(DL, zips[0]), ex = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-zip-"));
  let tested = "";
  try { tested = execFileSync("unzip", ["-t", zp], { encoding: "utf8" }); } catch (e) { tested = String(e.stdout || e); }
  check("capture: the .zip is valid (unzip -t, CRCs)", /No errors detected/.test(tested), tested.split("\n").slice(-2).join(" "));
  execFileSync("unzip", ["-q", "-o", zp, "-d", ex]);
  const inside = fs.readdirSync(ex);
  const wavF = inside.find((f) => f.endsWith(".wav")), midF = inside.find((f) => f.endsWith(".mid"));
  check("capture: the .zip holds the WAV, the .mid and a README", wavF && midF && inside.includes("README.txt"), inside.join(", "));
  if (wavF && midF) {
    const b = fs.readFileSync(path.join(ex, wavF));
    check("WAV: 24-bit stereo PCM", b.slice(0, 4).toString() === "RIFF" && b.readUInt16BE(20) === 0x0100 && b.readUInt16LE(22) === 2 && b.readUInt16LE(34) === 24, `${b.length} B, ${b.readUInt32LE(24)} Hz`);
    const m = fs.readFileSync(path.join(ex, midF));
    const ok = m.slice(0, 4).toString() === "MThd" && m.readUInt16BE(8) === 0 && m.readUInt16BE(12) === 480 && m.slice(14, 18).toString() === "MTrk";
    const evs = parseSmf(m);
    const ons = evs.filter((e) => (e.st & 0xf0) === 0x90 && e.d2 > 0), ats = evs.filter((e) => (e.st & 0xf0) === 0xa0);
    check(".mid is SMF 0, 480 PPQ, with notes and poly AT", ok && ons.length >= 5 && ats.length >= 1, `${m.length} B, ${ons.length} note-ons, ${ats.length} AT`);
    // the two files' zero: the WAV's first sound and the .mid's first note-on, 50 ms in, both (web-6)
    // the WAV's first onset: the first 2 ms hop whose RMS jumps past 1.8x (5 dB) the loudest of the 20 ms before it (a note
    // over the boot sound's tail is an onset; the tail itself is not)
    const sr = b.readUInt32LE(24), n = (b.length - 44) / 6, hop = Math.round(sr * 0.002), rms = [];
    for (let h = 0; (h + 1) * hop <= n; h++) {
      let e = 0;
      for (let i = h * hop; i < (h + 1) * hop; i++) for (let c = 0; c < 2; c++) { const o = 44 + i * 6 + c * 3; let v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16); if (v & 0x800000) v -= 0x1000000; e += (v / 8388608) ** 2; }
      rms.push(Math.sqrt(e / (2 * hop)));
    }
    let onset = -1;
    for (let h = 10; h < rms.length && onset < 0; h++) { let prev = 0; for (let j = Math.max(0, h - 10); j < h; j++) prev = Math.max(prev, rms[j]); if (rms[h] > 1.8 * prev + 1e-4) onset = h; }
    const wavMs = onset * 2, midMs = ons.length ? ons[0].tick / 0.96 : NaN;
    check("capture: WAV and .mid start together (first sound vs first note-on within 15 ms; both ~50 ms in)", Math.abs(wavMs - midMs) <= 15 && midMs > 40 && midMs < 60, `WAV ${wavMs.toFixed(1)} ms, MIDI ${midMs.toFixed(1)} ms`);
    fs.copyFileSync(path.join(ex, wavF), path.join(OUT, "capture.wav"));
    fs.copyFileSync(path.join(ex, midF), path.join(OUT, "capture.mid"));
    fs.copyFileSync(zp, path.join(OUT, "capture.zip"));
  }
}

// the phrase: PANIC first, so what sounds is the phrase (it comes in through MIDI in)
await key("keyDown", "KeyZ"); await key("keyDown", "KeyX"); await sleep(120); await key("keyUp", "KeyZ"); await key("keyUp", "KeyX");
await sleep(1500);
const p0 = await ringStats();
const ph = await rect("#phrase");
await click(ph.x, ph.y);
check("Play me something: playing", await until("!!c.S.phrase", 1000));
await sleep(1500);
const p1 = await ringStats();
check("Play me something: it sounds through MIDI in (BOW, MASTER -12 dB here)", p1.lastDb > -60 && p1.lastDb > p0.lastDb + 10, `last second ${p0.lastDb.toFixed(1)} -> ${p1.lastDb.toFixed(1)} dBFS`);
const ph2 = await rect("#phrase");
check("Play me something keeps its width as it becomes Stop", Math.abs(ph2.w - ph.w) < 0.5, `${ph.w.toFixed(1)} -> ${ph2.w.toFixed(1)} px`);
await click(ph2.x, ph2.y);
check("Play me something: stopped", await until("!c.S.phrase", 1000));

// factory reset arms on the first press, and the second reboots the firmware in the running worklet (web-22)
const rs = await rect("#reset");
await click(rs.x, rs.y);
const rs2 = await rect("#reset");
check("Factory reset arms, same width", (await evaluate("document.getElementById('reset').dataset.armed === '1'")) && Math.abs(rs2.w - rs.w) < 0.5);

// the theme, the tips
const th = await rect("#theme");
await click(th.x, th.y);
check("theme: light forced", await evaluate("document.documentElement.dataset.theme === 'light'"));
await shot("page_1280_light.png");
await click(th.x, th.y);
check("theme: dark forced", await evaluate("document.documentElement.dataset.theme === 'dark'"));
await shot("page_1280_dark.png");
await click(th.x, th.y);
const tipsB = await rect("#tips");
await click(tipsB.x, tipsB.y);
check("Show tips again", await evaluate("!document.getElementById('coach').hidden"));
report("bar");

// the new controls: four Tab stops, the encoders' values, the big screen, Escape, a suspended context (web-20, -12,
// -21, -5)
check("controls: four Tab stops (one per toolbar)", await evaluate("[...document.querySelectorAll('#controls .hit')].filter((e) => e.tabIndex === 0).length") === 4);
check("SELECT is a spinbutton with its shape as its value", await evaluate(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label') === 'SELECT'); return h.getAttribute('role') === 'spinbutton' && h.getAttribute('aria-valuetext') === window.__cyma.S.info.shape; })()`),
      await evaluate(`[...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label') === 'SELECT').getAttribute('aria-valuetext')`));
await evaluate("(window.scrollTo(0, 0), document.activeElement && document.activeElement.blur && document.activeElement.blur(), 1)");
await tap("KeyB");
const bs = await evaluate(`(() => { const b = document.getElementById('bigscreen'), c = document.getElementById('bigcanvas'), r = c.getBoundingClientRect(); return { open: !b.hidden, w: c.width, dev: r.width * devicePixelRatio, crisp: !c.classList.contains('smooth') }; })()`);
check("B: the big screen, live, at a whole multiple of 240 px", bs.open && bs.crisp && bs.w % 240 === 0 && bs.w >= 480 && Math.abs(bs.dev - bs.w) < 0.5, JSON.stringify(bs));
const bsPix = await evaluate(`(() => { const c = document.getElementById('bigcanvas'), x = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let mn = 1e9, mx = -1; for (let i = 0; i < x.length; i += 4 * 211) { const v = x[i] + x[i + 1] + x[i + 2]; mn = Math.min(mn, v); mx = Math.max(mx, v); } return mx > mn; })()`);
check("the big screen is painted", bsPix);
await key("keyDown", "KeyA"); await sleep(150);
check("the keyboard still plays under the big screen", await until("c.KS[7].down", 800));
await key("keyUp", "KeyA");
await tap("Escape");
check("Escape closes the big screen first", await evaluate("document.getElementById('bigscreen').hidden"));
const f4 = await evaluate(`(() => { const h = [...document.querySelectorAll('#controls .hit')].find(e => e.getAttribute('aria-label').startsWith('F4')); h.scrollIntoView({ block: "center", behavior: "instant" }); const r = h.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
await mouse("mousePressed", f4.x, f4.y, { modifiers: 1 }); await mouse("mouseReleased", f4.x, f4.y, { modifiers: 1 });
await until("c.KS[12].latched", 800);
const sayN = await st("c.S.info.say_n");
await tap("Escape");
check("Escape: latches let go and PANIC", await until(`!c.KS[12].latched && !c.KS[12].down && c.S.info.say_n !== ${sayN} && /PANIC/.test(c.S.info.say)`, 1500), await st("c.S.info.say"));
check("Escape: nothing sounding afterwards", await until("c.S.info.voices === 0", 2500),
      await st("JSON.stringify({ voices: c.S.info.voices, held: c.KS.map((s, i) => (s.down || s.latched ? i : null)).filter((x) => x !== null), info: c.S.info, phrase: !!c.S.phrase })"));
await evaluate("window.__cyma.S.ac.suspend().then(() => 1)");
check("a suspended context: the pill says Paused and the bar says how to resume", await until("document.getElementById('pill').dataset.state === 'paused' && /resume/.test(document.getElementById('barmsg').textContent)", 2000),
      await evaluate("document.getElementById('pilltext').textContent + ' | ' + document.getElementById('barmsg').textContent"));
const kb = await rect("#caption");
await click(kb.x, kb.y);
check("a click resumes it", await until("c.S.ac.state === 'running' && document.getElementById('pill').dataset.state === 'on' && !c.S.paused", 2000), await st("c.S.ac.state"));
report("new controls");

// the page's own per-frame cost: the screen conversion and the lights
const paintMs = await evaluate(`(() => { const s = performance.now(); let n = 0; return new Promise((r) => { function f() { if (++n < 60) requestAnimationFrame(f); else r((performance.now() - s) / 60); } requestAnimationFrame(f); }); })()`);
check("rAF keeps up (ms per frame)", paintMs < 25, paintMs.toFixed(1));
await shot("page_1280_on.png");

// the spec card and the demos (when package_web had them)
const specRows = await evaluate("[...document.querySelectorAll('#spectable tr')].map((r) => r.textContent)");
const hasSpec = await evaluate("CY_SPEC !== null");
check("spec card rows", hasSpec ? specRows.length >= 10 && specRows.some((r) => r.startsWith("Engine")) : specRows.length === 1, `${specRows.length} rows; ${specRows.slice(0, 3).join(" | ")}`);
const demoN = await evaluate("CY_DEMOS.length");
if (demoN) {
  // a card plays a synced <video> (render_demos.sh's MP4) or, without one, the <audio> beside a GIF: one player a card
  check("Listen: a card per demo", await evaluate("[...document.querySelectorAll('.demo')].filter((d) => d.querySelector('video, audio')).length") === demoN, `${demoN} demos`);
  const playable = await evaluate(`(async () => { const a = document.querySelector('.demo video, .demo audio'); a.muted = true; a.preload = 'auto'; await a.play(); await new Promise((r) => setTimeout(r, 700)); const t = a.currentTime; a.pause(); return { t, d: a.duration, kind: a.tagName.toLowerCase(), src: a.currentSrc.split('/').slice(-2).join('/') }; })()`);
  check("Listen: the first demo decodes and plays", playable.t > 0.2 && playable.d > 5, JSON.stringify(playable));
  check("Listen: titles from the demo scripts", await evaluate("document.querySelector('.demo h3').textContent.length > 6"), await evaluate("document.querySelector('.demo h3').textContent"));
}

// Factory reset, pressed twice: the firmware reboots in the running worklet (web-22). Last in this section: the
// fresh boot's logo would sound under the checks after it
{
  await until("!document.getElementById('reset').dataset.armed", 5000);   // (the arming above lasts 4 s)
  const rsA = await rect("#reset");
  await click(rsA.x, rsA.y);
  await tap("ArrowUp"); await until("c.S.info && c.S.info.num === 3", 1500);
  const numBefore = await st("c.S.info.num");
  await evaluate(`(() => { window.__rmsgs = []; const p = window.__cyma.S.node.port, o = p.onmessage; p.onmessage = (e) => { if (e.data.type !== 'frame') window.__rmsgs.push(e.data.type); o(e); }; return 1; })()`);
  const rs3 = await rect("#reset");
  await click(rs3.x, rs3.y);
  check("Factory reset: still on, back to preset 01, no reload", await until("c.S.on && c.S.info && c.S.info.num === 1 && c.S.info.preset === 'SINGING BOWL'", 4000) && await evaluate("!!window.__cyma"),
        `${numBefore} -> ${await st("c.S.info && c.S.info.num")}; msgs ${JSON.stringify(await evaluate("window.__rmsgs"))}; armed ${await evaluate("document.getElementById('reset').dataset.armed")}; bar ${await evaluate("document.getElementById('barmsg').textContent")}`);
  check("Factory reset: frames keep coming", await until("c.frames > 0", 500) && await (async () => { const f = await st("c.frames"); await sleep(400); return (await st("c.frames")) > f + 10; })());
  report("factory reset");
}

// ================================================================== 2. density: the screen at an exact multiple where one fits
// (the device is sized by the viewport's height too now: at 1280 x 1100 it is 1100 px wide, the LCD 220 css px)
await metrics(1280, 1100, 2);
await sleep(400);
const crisp = await evaluate(`(() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); return { w: c.width, css: r.width, dev: r.width * devicePixelRatio }; })()`);
check("2x: screen at exactly 2 device px per LCD px (480 backing = 480 device px)", crisp.w === 480 && Math.abs(crisp.dev - 480) < 0.5, JSON.stringify(crisp));
await shot("page_1280_2x_device.png", false);
await metrics(1280, 1100, 1);
await sleep(300);
const crisp1 = await evaluate(`(() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); return { w: c.width, css: r.width, dev: r.width * devicePixelRatio }; })()`);
check("1x: screen at exactly 1 device px per LCD px", crisp1.w === 240 && Math.abs(crisp1.dev - 240) < 0.5, JSON.stringify(crisp1));
// where no whole multiple fits (0.70x at 900 px, 1.40x at 900 px on 2x): smoothed, at the exact device size, 1:1
for (const [w, dpr] of [[900, 1], [900, 2]]) {
  await metrics(w, 1000, dpr);
  await sleep(400);
  const sm = await evaluate(`(() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); return { w: c.width, dev: r.width * devicePixelRatio, smooth: c.classList.contains('smooth'), css: getComputedStyle(c).imageRendering }; })()`);
  check(`${w}@${dpr}x: the LCD smoothed at its exact device size (no nearest-neighbour downscale)`, sm.smooth && sm.css === "auto" && Math.abs(sm.w - sm.dev) <= 1, JSON.stringify(sm));
}
await metrics(1280, 900, 1);
await sleep(300);
report("density");

// ================================================================== 3. 390 px phone
await metrics(390, 844, 3, true);
await navigate(`${BASE}/instrument/dist/web/index.html`);
check("390: no horizontal page scroll", await evaluate("document.documentElement.scrollWidth <= innerWidth"), `${await evaluate("document.documentElement.scrollWidth")} vs ${await evaluate("innerWidth")}`);
check("390: device keeps 560 px and scrolls in its box", await evaluate("(() => { const b = document.querySelector('.devicebox'); return b.scrollWidth > b.clientWidth && document.getElementById('device').getBoundingClientRect().width >= 559; })()"));
check("390: 16 px gutter", await evaluate("Math.round(document.querySelector('.stage').getBoundingClientRect().left)") === 16);
await shot("page_390.png");
report("phone");

// ================================================================== 4. a sandboxed iframe (opaque origin)
await metrics(1280, 900, 1);
fs.writeFileSync(path.join(ROOT, "tests", "sandbox.html"),
  `<!doctype html><meta charset="utf-8"><title>sandbox</title><link rel="icon" href="data:,"><style>body{margin:0}iframe{border:0;width:1280px;height:900px;display:block}</style>
   <iframe sandbox="allow-scripts allow-downloads" allow="autoplay; midi" src="../instrument/dist/web/index.html"></iframe>`);
await cdp("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
await navigate(`${BASE}/tests/sandbox.html`);
await sleep(1200);
const tree = await cdp("Page.getFrameTree");
const child = tree.frameTree.childFrames?.[0]?.frame;
const ctx = iframeSessions.length ? { session: iframeSessions[iframeSessions.length - 1] } : child && contexts.get(child.id);
check("sandboxed frame loaded", !!ctx, iframeSessions.length ? "in a process of its own" : child ? child.url : "no child frame");
if (ctx) {
  check("sandbox: origin is opaque", await evaluate("self.origin", ctx) === "null");
  const g2 = await rect("#go", ctx);
  await click(g2.x, g2.y);
  check("sandbox: switched on (embedded wasm, Blob worklet)", await until("c.S.on === true", 8000, ctx));
  check("sandbox: frames arrive", await until("c.frames > 30", 4000, ctx));
  await key("keyDown", "KeyA"); await sleep(200);
  check("sandbox: keyboard plays", await until("c.S.leds && c.S.leds[7] >= 2", 1500, ctx));
  await key("keyUp", "KeyA");
  await shot("page_sandboxed.png", false);
}
report("sandbox");

// ================================================================== 5. the resampler: a 48 kHz AudioContext
await cdp("Target.setAutoAttach", { autoAttach: false, waitForDebuggerOnStart: false, flatten: true });
await navigate(`${BASE}/instrument/dist/web/index.html?rate=48000`);
const g4b = await rect("#go");
await click(g4b.x, g4b.y);
check("48 kHz: on, resampling", await until("c.S.on && c.S.resampling && c.S.ac.sampleRate === 48000", 8000), `${await st("c.S.ac && c.S.ac.sampleRate")}`);
const f48 = await st("c.frames"); await sleep(1000); const fps48 = (await st("c.frames")) - f48;
check("48 kHz: frames at 60 Hz", fps48 >= 50 && fps48 <= 66, `${fps48}/s`);
await until("c.S.ac.currentTime > 2.6", 4000);
const q0 = await ringStats();
await key("keyDown", "KeyG"); await sleep(400); await key("keyUp", "KeyG");
const q1 = await ringStats();
// (frames over context time tells 44.1 kHz from 48 kHz, 3,900 Hz apart; a loaded machine's dropped render quanta read
// a few hundred Hz low, so the tolerance is 1,500 Hz: it still cannot mistake one rate for the other)
// (v1.1.1: the last second's peak, as above: the whole ring holds the boot logo, louder than a soft key)
check("48 kHz: the note is heard, the capture stays at 44.1 kHz", q1.lastDb > q0.lastDb + 3 && q1.nan === 0 && Math.abs(q1.n / q1.t1 - 44100) < 1500, `last second ${q0.lastDb.toFixed(1)} -> ${q1.lastDb.toFixed(1)} dBFS (the logo's peak ${q0.peakDb.toFixed(1)}), ${q1.n} frames in ${q1.t1.toFixed(2)} s`);
check("48 kHz: the page says so", /resampled/.test(await evaluate("document.getElementById('barmsg').textContent")));
report("48 kHz");

// ================================================================== 6. file:// (no server at all)
await navigate(pathToFileURL(path.join(path.resolve(ROOT), "instrument", "dist", "web", "index.html")).href);
const g5 = await rect("#go");
await click(g5.x, g5.y);
check("file://: switched on (embedded wasm)", await until("c.S.on === true", 8000));
check("file://: frames", await until("c.frames > 30", 4000));
report("file://");

// ================================================================== 7. the unpackaged dev build (worklet.js, app.wasm beside it)
await navigate(`${BASE}/instrument/build/emu/index.html`);
const g3 = await rect("#go");
await click(g3.x, g3.y);
check("dev build: switched on via worklet.js", await until("c.S.on === true", 8000));
check("dev build: frames", await until("c.frames > 30", 4000));
check("dev build: no inlined worklet", await evaluate("!document.getElementById('cy-worklet')"));
// (v1.1) the unpackaged dev build has no demos/ (package_web.py adds them to dist/web), so the intro's poster image
// (demos/01_singing_bowl_lean.png) is a 404 there by design; this Chrome loads that lazy image, v1.0's page included
for (let i = problems.length - 1; i >= 0; i--)
  if (/404/.test(problems[i]) && /\/instrument\/build\/emu\/demos\//.test(problems[i])) problems.splice(i, 1);
report("dev build");

// ================================================================== 8. graceful errors
// no app.wasm and no embedded copy: a clear message, the switch back, nothing uncaught (the 404 itself is expected)
await navigate(`${BASE}/instrument/dist-nowasm/index.html`);
const g6 = await rect("#go");
await click(g6.x, g6.y);
await until("document.getElementById('alert').textContent.length > 0", 6000);
const al = await evaluate("document.getElementById('alert').textContent");
check("no wasm: says so, in words", /Could not start/.test(al) && /app\.wasm/.test(al), al);
check("no wasm: the switch comes back", await evaluate("!document.getElementById('go').disabled && document.getElementById('pill').dataset.state === 'error'"));
check("no wasm: nothing uncaught", !problems.some((p) => p.startsWith("exception")), problems.filter((p) => p.startsWith("exception")).join(" | "));
problems.length = 0;
// no AudioWorklet in this browser
await navigate(`${BASE}/instrument/dist/web/index.html`);
await evaluate(`(Object.defineProperty(BaseAudioContext.prototype, "audioWorklet", { get() { return undefined; }, configurable: true }), 1)`);
const g7 = await rect("#go");
await click(g7.x, g7.y);
await until("document.getElementById('alert').textContent.length > 0", 6000);
const al2 = await evaluate("document.getElementById('alert').textContent");
check("no AudioWorklet: says so, in words", /AudioWorklet/.test(al2), al2);
report("errors");

fs.writeFileSync(path.join(OUT, "chrome_test.json"), JSON.stringify({ chrome: ver.Browser, failures, results }, null, 1));
console.log(failures ? `\n${failures} FAILED` : "\nall passed");
ws.close();
chrome.kill(); server.kill();
process.exit(failures ? 1 : 0);

// ------------------------------------------------------------------ an SMF type 0's events, with absolute ticks
function parseSmf(b) {
  const out = [];
  let i = 22, tick = 0, run = 0;
  const end = 22 + b.readUInt32BE(18);
  const vlq = () => { let v = 0, c; do { c = b[i++]; v = (v << 7) | (c & 0x7f); } while (c & 0x80); return v; };
  while (i < end) {
    tick += vlq();
    let st = b[i];
    if (st === 0xff) { i += 2; const n = vlq(); i += n; continue; }
    if (st & 0x80) { i++; run = st; } else st = run;
    const two = (st & 0xf0) === 0xc0 || (st & 0xf0) === 0xd0;
    const d1 = b[i++], d2 = two ? 0 : b[i++];
    out.push({ tick, st, d1, d2 });
  }
  return out;
}
