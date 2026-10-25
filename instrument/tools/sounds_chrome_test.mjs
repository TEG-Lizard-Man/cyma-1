// SPDX-License-Identifier: MPL-2.0
// CYMA-1 Sounds T5 (docs/CYMA-1-sounds-spec.md §8, §11.1): the Sounds flows of the packaged page in headless Chrome
// (always --mute-audio), driven as a person would, through the frozen DOM ids and test hooks (§8.6, I7) and the
// spec's exact strings (§8.4). Run by tools/run_all.sh --chrome (step sounds_chrome).
//
//   node tools/sounds_chrome_test.mjs APP OUTDIR
//
// APP is the app directory: APP/dist/web/index.html (package_web.py) is the page, APP/build/emu/app.wasm the second
// firmware that plays the fake FM-1, APP/tests/sounds/vectors.json the frozen vectors. It serves APP on 127.0.0.1.
// Sections (each check a line, ok / FAIL; all of them in OUTDIR/sounds_chrome.json; screenshots there too):
//   codec      window.__cyma.sounds reproduces every vector: names, the 24 codes both ways, the files' values, the bad
//              codes' kinds, N = 38 / 42, the JSON import edge case, and the exhaustive writer digests and round trip
//   save       Save sound -> a name -> listed -> reload -> still there (localStorage "cyma-1-store"["2"]); the name
//              rule as typed; the empty name; Rename; Delete + Undo; Delete all + Undo; ⌘S / Ctrl+S
//   link       #cy1.<code>: the start note and banner before switch-on; after it the caption names the sound, nothing is
//              stored until Keep it; Keep it -> a slot, the hash cleared, a reload keeps it; Undo; the duplicate; a
//              damaged code; a 500-character fragment ignored; hashchange; a pasted code
//   files      Save as file and Save all as a file (CDP downloads), re-imported through DOM.setFileInputFiles: the bank
//              sheet sees the duplicates; bad files (5 MB, prototype pollution, format v2, unknown params, not JSON)
//              give their exact messages, pollute nothing and log no console error
//   reset      Factory reset keeps My Sounds (OBJ_USER) and forgets the settings
//   tabs       two tabs: A saves a sound; B is told, its Save is disabled, and B's own autosave never overwrites A's slot
//   phone      390 px: no horizontal scroll with the Sounds section, a popover and a long code showing
//   hardening  (spec revision 2026-10-24b) 390 px: every Save / Share popover control and every ⋯ menu item is a 44 x 44
//              target, and both menus open inside the screen; the code box shows the longest code whole (390 and 1280
//              px); an Undo offers no second Undo and leaves the focus on a control; "Delete all" with one sound speaks
//              of one; a credit's Unicode format characters never reach the banner or a saved file. With the cy2
//              vectors: the codec section checks linkEncode / codeDecode against vectors.json "cy2", and the link
//              section a #cy2 arrival, Copy link (<page>#cy2.…) and no link for a sound over 128 characters
//   fm1        a fake FM-1 (a second app.wasm behind an injected navigator.requestMIDIAccess): nothing sent before
//              Connect; connect, get, send all (SAVE pressed on the fake through web_buttons), declined, unplugged
//              mid-send -> "stopped answering" and nothing changed; play the current sound; a stock FM-1 (FM-1_015)
//              sees only HS_QUERY; every frame the page sent passes the allowlist (§5.6 P7)
// Exit 0 when every check passes, 1 when one fails, 77 when the page has no Sounds yet (no #sndsave) or Chrome is missing.
// Nothing here touches MIDI hardware: headless Chrome has no MIDI devices, and the only MIDI port is the fake.
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";

const [APPARG, OUTARG] = process.argv.slice(2);
if (!APPARG || !OUTARG) { console.log("usage: node tools/sounds_chrome_test.mjs APP OUTDIR"); process.exit(2); }
const APP = path.resolve(APPARG), OUT = path.resolve(OUTARG);
const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
if (!fs.existsSync(CHROME)) { console.log("no Google Chrome"); process.exit(77); }
const PAGE_FILE = path.join(APP, "dist", "web", "index.html");
if (!fs.existsSync(PAGE_FILE)) { console.log("no dist/web/index.html (package_web.py)"); process.exit(77); }
if (!/id="sndsave"/.test(fs.readFileSync(PAGE_FILE, "utf8"))) { console.log("this page has no Sounds yet (no #sndsave): SKIP"); process.exit(77); }
const VEC = JSON.parse(fs.readFileSync(path.join(APP, "tests", "sounds", "vectors.json"), "utf8"));
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT, { recursive: true });
const DL = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-sounds-dl-"));
const FIX = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-sounds-fix-"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: !!ok, detail: String(detail) });
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail !== "" ? "  (" + String(detail).slice(0, 400) + ")" : ""}`);
  return !!ok;
}

// ------------------------------------------------------------------------------------- codes for the fixtures
const CRC_T = new Uint32Array(256).map((_, i) => { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = (u8) => { let c = 0xFFFFFFFF; for (const b of u8) c = CRC_T[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function codeOf(name, q, layout = 1) {
  const nb = Buffer.from(name, "ascii"), r = Buffer.alloc(4 + nb.length + 2 * q.length);
  r[0] = layout; r[1] = 1; r[2] = q.length; r[3] = nb.length; nb.copy(r, 4);
  q.forEach((v, i) => r.writeUInt16LE(v, 4 + nb.length + 2 * i));
  const blob = Buffer.concat([r, Buffer.alloc(4)]);
  blob.writeUInt32LE(crc32(r), r.length);
  return "cy1." + blob.toString("base64url");
}
const F = VEC.factory;
const soundDoc = (name, params, extra = {}) => ({ format: "cyma1-sound", version: 1, name, created: "2026-10-24T18:20:00Z",
  made_with: { app: "CYMA-1", version: "DEV", where: "web" }, param_set: 1, params, ...extra });

// ------------------------------------------------------------------------------------- server and browser
const PORT = 8000 + Math.floor(Math.random() * 900), DBG = 9300 + Math.floor(Math.random() * 600);
const server = spawn("python3", ["-m", "http.server", String(PORT), "--bind", "127.0.0.1", "--directory", APP], { stdio: "ignore" });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), "cyma-sounds-chrome-"));
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${DBG}`, `--user-data-dir=${prof}`, "--no-first-run",
  "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--window-size=1280,900", "--hide-scrollbars",
  "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows", "about:blank"], { stdio: "ignore" });
const cleanup = () => { try { chrome.kill(); server.kill(); } catch { /* */ } };
process.on("exit", cleanup);
setTimeout(() => { console.log("FAIL sounds_chrome: the whole run took over 12 minutes (a hang)"); process.exit(1); }, 12 * 60000).unref();
process.on("unhandledRejection", (e) => { console.log("FAIL sounds_chrome: " + (e && e.message || e)); process.exit(1); });
let ver = null;
for (let i = 0; i < 100 && !ver; i++) { await sleep(100); try { ver = await (await fetch(`http://127.0.0.1:${DBG}/json/version`)).json(); } catch { /* */ } }
if (!ver) { console.log("FAIL chrome did not start"); process.exit(1); }
for (let i = 0; i < 50; i++) { try { await fetch(`http://127.0.0.1:${PORT}/`); break; } catch { await sleep(100); } }
const BASE = `http://127.0.0.1:${PORT}/dist/web/index.html`;

class Tab {
  static async open() {
    const t = await (await fetch(`http://127.0.0.1:${DBG}/json/new?about:blank`, { method: "PUT" })).json();
    const tab = new Tab(t);
    await tab.ready;
    return tab;
  }
  constructor(t) {
    this.id = t.id;
    this.ws = new WebSocket(t.webSocketDebuggerUrl);
    this.msgId = 0; this.pending = new Map(); this.listeners = []; this.problems = [];
    this.ws.addEventListener("message", (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && this.pending.has(m.id)) { const { res, rej } = this.pending.get(m.id); this.pending.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); return; }
      if (m.method === "Runtime.exceptionThrown") this.problems.push("exception: " + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text).split("\n")[0]);
      else if (m.method === "Runtime.consoleAPICalled" && (m.params.type === "error" || m.params.type === "assert"))
        this.problems.push("console.error: " + m.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 200));
      else if (m.method === "Log.entryAdded" && m.params.entry.level === "error" && !/favicon/.test(m.params.entry.url || "")) this.problems.push("log: " + m.params.entry.text);
      for (const l of this.listeners) l(m);
    });
    this.ready = new Promise((r) => this.ws.addEventListener("open", r, { once: true })).then(async () => {
      for (const d of ["Runtime.enable", "Log.enable", "Page.enable", "DOM.enable"]) await this.cdp(d);
    });
  }
  cdp(method, params = {}) {                  // (60 s at most: a page that stops answering fails the run, never hangs it)
    return new Promise((res, rej) => {
      const id = ++this.msgId;
      const t = setTimeout(() => { if (this.pending.has(id)) { this.pending.delete(id); rej(new Error(`${method}: no answer in 60 s`)); } }, 60000);
      this.pending.set(id, { res: (v) => { clearTimeout(t); res(v); }, rej: (e) => { clearTimeout(t); rej(e); } });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  async ev(expr) {
    const r = await this.cdp("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error("evaluate: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    return r.result.value;
  }
  async nav(url, wait = true) {
    if (wait && url.includes("#")) await this.nav("about:blank", false);   // (a fragment-only change fires no load)
    const loaded = new Promise((r) => { const l = (m) => { if (m.method === "Page.loadEventFired") { this.listeners.splice(this.listeners.indexOf(l), 1); r(); } }; this.listeners.push(l); });
    await this.cdp("Page.navigate", { url });
    await loaded;
    if (!wait) return;
    for (let i = 0; i < 100 && !(await this.ev("!!(window.__cyma && window.__cyma.sounds)").catch(() => false)); i++) await sleep(50);
    await this.ev("document.fonts ? document.fonts.ready.then(() => 1) : 1");
    await sleep(250);
  }
  async reload() {
    const loaded = new Promise((r) => { const l = (m) => { if (m.method === "Page.loadEventFired") { this.listeners.splice(this.listeners.indexOf(l), 1); r(); } }; this.listeners.push(l); });
    await this.cdp("Page.reload", { ignoreCache: true });
    await loaded;
    for (let i = 0; i < 100 && !(await this.ev("!!(window.__cyma && window.__cyma.sounds)").catch(() => false)); i++) await sleep(50);
    await sleep(300);
  }
  async until(expr, ms = 5000) { const t0 = Date.now(); while (Date.now() - t0 < ms) { try { if (await this.ev(expr)) return true; } catch { /* */ } await sleep(50); } return false; }
  metrics(w, h) { return this.cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: w < 600 }); }
  async rect(sel) {
    return this.ev(`(async () => { const e = ${sel.startsWith("@") ? sel.slice(1) : `document.querySelector(${JSON.stringify(sel)})`}; if (!e) return null;
      e.scrollIntoView({ block: "center", behavior: "instant" });
      await new Promise((r) => { requestAnimationFrame(() => requestAnimationFrame(r)); setTimeout(r, 60); });   // (layout settled; a background tab has no rAF)
      const r = e.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2, w: r.width, h: r.height, vis: r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== "hidden" && !e.closest("[hidden]"), dis: !!e.disabled }; })()`);
  }
  async mouse(type, x, y, extra = {}) { await this.cdp("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1, ...extra }); }
  async click(sel) {                            // a real click, at the element's centre; false when it is not there to click
    const r = await this.rect(sel);
    if (!r || !r.vis || r.dis) return false;
    await this.mouse("mousePressed", r.x, r.y); await sleep(30); await this.mouse("mouseReleased", r.x, r.y);
    await sleep(80);
    return true;
  }
  async key(code, key, vk, mods = 0, text) {
    await this.cdp("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", code, key, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods, ...(text ? { text } : {}) });
    await this.cdp("Input.dispatchKeyEvent", { type: "keyUp", code, key, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods });
  }
  async type(text) { await this.cdp("Input.insertText", { text }); await sleep(60); }
  async typeKeys(text) { for (const ch of text) { await this.cdp("Input.dispatchKeyEvent", { type: "keyDown", key: ch, text: ch }); await this.cdp("Input.dispatchKeyEvent", { type: "keyUp", key: ch }); } await sleep(60); }
  async shot(name) { const r = await this.cdp("Page.captureScreenshot", { format: "png", captureBeyondViewport: false }); fs.writeFileSync(path.join(OUT, name), Buffer.from(r.data, "base64")); }
  close() { try { this.ws.close(); fetch(`http://127.0.0.1:${DBG}/json/close/${this.id}`); } catch { /* */ } }
}
await (async () => { const t = await (await fetch(`http://127.0.0.1:${DBG}/json/list`)).json(); return t; })();
const P = await Tab.open();
await P.cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: DL, eventsEnabled: true }).catch(() => {});
const report = (label, tab = P) => { check(`${label}: no console errors or exceptions`, tab.problems.length === 0, tab.problems.slice(0, 4).join(" | ")); tab.problems.length = 0; };

// helpers on the page: a button by its text (inside a scope), the visible text of an element, the bank, the store
const BTN = (scope, text) => {                // a visible button by its text: exact, or a prefix when text ends with "*"
  const sel = scope.split(",").map((x) => `${x.trim()} button, ${x.trim()} [role=menuitem]`).join(", ");
  const pre = text.endsWith("*"), t = pre ? text.slice(0, -1) : text;
  return `@[...document.querySelectorAll(${JSON.stringify(sel)})].find((b) => { if (b.offsetParent === null) return false; const x = b.textContent.replace(/\\s+/g, " ").trim(); return ${pre ? `x.startsWith(${JSON.stringify(t)})` : `x === ${JSON.stringify(t)}`}; })`;
};
const text = (sel, tab = P) => tab.ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e && !e.closest("[hidden]") ? e.textContent.replace(/\\s+/g, " ").trim() : ""; })()`);
const allText = (tab = P) => tab.ev("document.body.innerText.replace(/\\s+/g, ' ')");
const bank = (tab = P) => tab.ev("(window.__cyma.sounds.bank() || []).map((s) => s ? { name: s.name, q: [...s.q] } : null)");
const bankNames = async (tab = P) => (await bank(tab)).map((s, i) => (s ? `U${String(i + 1).padStart(2, "0")} ${s.name}` : null)).filter(Boolean);
const lsUser = (tab = P) => tab.ev(`(() => { try { const v = JSON.parse(localStorage.getItem("cyma-1-store") || "{}"); return v["2"] || null; } catch (e) { return "ERR"; } })()`);
function decodeBank(b64) {
  if (!b64) return null;
  const b = Buffer.from(b64, "base64");
  if (b.length !== 3020) return { bad: b.length };
  const used = b.readUInt32LE(8), slots = [];
  for (let s = 0; s < 32; s++) {
    if (!(used >>> s & 1)) { slots.push(null); continue; }
    const o = 12 + 94 * s;
    let n = ""; for (let i = 0; i < 14 && b[o + i]; i++) n += String.fromCharCode(b[o + i]);
    const q = []; for (let i = 0; i < 40; i++) q.push(b.readUInt16LE(o + 14 + 2 * i));
    slots.push({ name: n, q });
  }
  return { used, slots };
}
const lsNames = async (tab = P) => { const d = decodeBank(await lsUser(tab)); return d && d.slots ? d.slots.map((s, i) => (s ? `U${String(i + 1).padStart(2, "0")} ${s.name}` : null)).filter(Boolean) : []; };
const msg = () => text("#sndmsg");
const barmsg = () => text("#barmsg");
const state = (tab = P) => tab.ev("window.__cyma.sounds.call('cur').then((r) => r && r.state >>> 0).catch(() => -1)");
async function switchOn(tab = P) {
  if (await tab.ev("!!(window.__cyma.S && window.__cyma.S.on)")) return true;
  await tab.click("#go");
  return tab.until("!!(window.__cyma.S && window.__cyma.S.on) && window.__cyma.frames > 10", 12000);
}
const savePop = '[role=dialog][aria-labelledby="save-h"]';
const nameInput = `@document.querySelector('${savePop} input[type=text], ${savePop} input:not([type])')`;
async function saveViaUi(name, tab = P) {   // the bar's Save sound: type a name, press the dialog's Save/Replace button
  if (!(await tab.click("#sndsave"))) return "no Save sound button";
  if (!(await tab.until(`!document.querySelector('${savePop}').hidden`, 2000))) return "the Save sound dialog did not open";
  await tab.ev(`(() => { const i = ${nameInput.slice(1)}; i.focus(); i.select(); return 1; })()`);
  await tab.type(name);
  const ok = await tab.click(BTN(savePop, "Save to*")) || await tab.click(BTN(savePop, "Replace*"));
  if (!ok) return "no Save to / Replace button enabled";
  await sleep(400);
  return "";
}
async function rowMenu(slotLabel, item, tab = P) {   // a row's ⋯ menu in My Sounds, then one of its items
  const more = `@[...document.querySelectorAll('#sndlist [aria-haspopup=menu]')].find((b) => (b.getAttribute('aria-label') || '').includes(${JSON.stringify(slotLabel)}))`;
  if (!(await tab.click(more))) return false;
  await sleep(150);
  return tab.click(`@[...document.querySelectorAll('[role=menu]:not([hidden]) [role=menuitem], [role=menu]:not([hidden]) button')].find((b) => b.offsetParent !== null && b.textContent.trim().startsWith(${JSON.stringify(item)}))`);
}

// ============================================================================ 1. the page's codec (G1)
await P.metrics(1280, 900);
await P.nav(BASE);
check("page: window.__cyma.sounds has every I7 hook", await P.ev("['cleanName','codeEncode','codeDecode','soundToJson','soundFromJson','bankToJson','bankFromJson','bank','call','sx'].every((k) => typeof window.__cyma.sounds[k] === 'function')"));
check("page: every frozen DOM id is there", await P.ev("['sndsave','sndshare','sndbanner','sndkeep','sndundo','sndlist','sndimport','sndpaste','sndopen','sndexport','sndmsg','fm1card','fm1connect'].filter((i) => !document.getElementById(i))").then((m) => m.length === 0 || m), "");
const codec = await P.ev(`(async () => {
  const V = ${JSON.stringify(VEC)}, s = window.__cyma.sounds, fails = [], f = (m) => fails.length < 30 && fails.push(m);
  for (const [a, b] of V.names) if (s.cleanName(a) !== b) f("name " + JSON.stringify(a) + " -> " + s.cleanName(a));
  for (const fa of V.factory) {
    if (s.codeEncode(fa.name, fa.q16) !== fa.code) f("code " + fa.name);
    const d = s.codeDecode("Listen: https://claude.ai/artifact/x#" + fa.code.slice(0, 60) + "\\n" + fa.code.slice(60) + ").");
    if (d.name !== fa.name || [...d.q].join() !== fa.q16.join() || d.extra || d.missing) f("decode " + fa.name);
    const j = s.soundToJson(fa.name, fa.q16);
    if (j.format !== "cyma1-sound" || j.version !== 1 || j.name !== fa.name || j.param_set !== 1 || "author" in j) f("file head " + fa.name);
    if (JSON.stringify(Object.keys(j.params)) !== JSON.stringify(Object.keys(fa.params))) f("param order " + fa.name);
    for (const k in fa.params) if (j.params[k] !== fa.params[k]) f("json " + fa.name + " " + k + " " + j.params[k] + " != " + fa.params[k]);
    const back = s.soundFromJson(JSON.parse(JSON.stringify(j)));
    if ([...back.q].join() !== fa.q16.join() || back.skipped.length || back.missing.length || back.name !== fa.name) f("json back " + fa.name);
  }
  for (const [kind, list] of Object.entries(V.bad_codes)) for (const c of list) {
    try { s.codeDecode(c); f("bad code accepted " + c.slice(0, 20)); } catch (e) { if (!e || e.code !== kind) f("bad code " + c.slice(0, 20) + ": " + (e && e.code) + " != " + kind); }
  }
  const e = s.soundFromJson({ format: "cyma1-sound", version: 1, name: "edge", params: V.json_import_edge.params });
  if ([...e.q].join() !== V.json_import_edge.q16.join()) f("json import edge values");
  if (JSON.stringify(e.skipped) !== JSON.stringify(V.json_import_edge.skipped)) f("json import edge skipped " + JSON.stringify(e.skipped));
  if (e.missing.length !== V.json_import_edge.missing_count) f("json import edge missing " + e.missing.length);
  // a bank both ways
  const slots = Array(32).fill(null); slots[0] = { name: V.factory[0].name, q: V.factory[0].q16 }; slots[4] = { name: V.factory[3].name, q: V.factory[3].q16 };
  const bj = s.bankToJson(slots), bb = s.bankFromJson(JSON.parse(JSON.stringify(bj)));
  if (bj.format !== "cyma1-bank" || bb.length !== 2 || bb[0].slot !== 1 || bb[1].slot !== 5 || [...bb[1].q].join() !== V.factory[3].q16.join()) f("bank json both ways");
  return fails;
})()`);
check("codec: names, 24 codes both ways, file values and order, bad codes, the import edge case, a bank (vectors.json)", codec.length === 0, codec.join(" | "));
// the short link code cy2 (spec revision 2026-10-24b, §4a): the page's linkEncode and codeDecode against vectors.json
// "cy2", which tests/sounds/gen_vectors.py derives from the spec's text (the CLI is held to the same vectors in T7)
if (VEC.cy2) {
  const c2 = await P.ev(`(() => { const V = ${JSON.stringify(VEC.cy2)}, F = ${JSON.stringify(F)}, s = window.__cyma.sounds, fails = [], f = (m) => fails.length < 30 && fails.push(m);
    if (typeof s.linkEncode !== "function") return ["the page has no linkEncode hook (no cy2 links)"];
    if (s.SHARE_LINK_MAX !== V.anchor_max) f("SHARE_LINK_MAX " + s.SHARE_LINK_MAX + ", not " + V.anchor_max);
    if (JSON.stringify([...s.CY2_DEF]) !== JSON.stringify(V.def_q16)) f("CY2_DEF is not the frozen table");
    V.factory.forEach((e, i) => {
      const c = s.linkEncode(F[i].name, F[i].q16);
      if (c !== e.code) f("encode " + e.name + ": " + c.slice(0, 30) + "… (" + c.length + ")");
      if (s.codeEncode(F[i].name, F[i].q16) !== F[i].code) f("the code (cy1) of " + e.name + " changed");
      for (const t of [e.code, "#" + e.code, "Listen: https://claude.ai/artifact/x#" + e.code.slice(0, 30) + "\\n  " + e.code.slice(30) + ")."]) {
        const d = s.codeDecode(t);
        if (d.name !== e.name || [...d.q].join() !== F[i].q16.join() || d.extra || d.missing) f("decode " + e.name + " from " + t.slice(0, 16));
      }
    });
    for (const [k, e] of Object.entries(V.edges)) {
      if (s.linkEncode(e.name, e.q16) !== e.code) f("edge encode " + k);
      const d = s.codeDecode(e.code);
      if (d.name !== e.decoded_name || [...d.q].join() !== e.q16.join()) f("edge decode " + k);
      if ((e.code.length <= s.SHARE_LINK_MAX) !== e.anchor_fits) f("edge fits " + k);
    }
    for (const [k, t] of Object.entries(V.tolerated)) {
      const d = s.codeDecode(t.code);
      if ([...d.q].join() !== t.q16.join() || d.extra !== t.extra || d.missing !== t.missing) f("tolerated " + k + ": extra " + d.extra + ", missing " + d.missing);
    }
    for (const [kind, list] of Object.entries(V.bad)) for (const c of list) {
      try { s.codeDecode(c); f("bad cy2 accepted: " + c.slice(0, 30)); } catch (e) { if (!e || e.code !== kind) f("bad cy2 " + c.slice(0, 30) + ": " + (e && e.code) + " != " + kind); }
    }
    return fails; })()`);
  check(`codec (cy2): the 24 factory link codes (${Math.min(...VEC.cy2.factory.map((e) => e.length))}–${Math.max(...VEC.cy2.factory.map((e) => e.length))} characters) both ways, the edges (128 fits, 131 does not), N = 38 / 42, the refusals`,
    c2.length === 0, c2.join(" | "));
  const zw = await P.ev(`(() => { try { const d = window.__cyma.sounds.codeDecode(${JSON.stringify(VEC.cy2.format_chars.text)}); return d.name; } catch (e) { return "refused: " + (e && e.code); } })()`);
  check("codec (cy2): a code with zero-width characters, a soft hyphen and a BOM inside it still decodes (Unicode format characters are removed first, as the CLI does)",
    zw === VEC.cy2.format_chars.name, zw);
}
const tol = await P.ev(`(() => { const s = window.__cyma.sounds, r = [];
  for (const c of ${JSON.stringify([codeOf("TEST", F[0].q16.slice(0, 38)), codeOf("TEST", [...F[0].q16, 1, 2])])}) { const d = s.codeDecode(c); r.push([d.missing, d.extra, [...d.q].join()]); }
  return r; })()`);
const DEF = await P.ev("window.__cyma.sounds.soundFromJson({ format: 'cyma1-sound', version: 1, name: 'D', params: {} }).q.map((x) => x)");
check("codec: N = 38 takes the defaults for the two it lacks; N = 42 ignores the two extra", tol[0][0] === 2 && tol[0][2] === [...F[0].q16.slice(0, 38), ...DEF.slice(38)].join() && tol[1][1] === 2 && tol[1][2] === F[0].q16.join(),
  JSON.stringify(tol.map((x) => x.slice(0, 2))));
// the exhaustive writer: every stored value of every continuous parameter (every grid point of the others), the text
// SHA-256'd exactly as cyma_sounds_ref.py's writer_digest does, and every value read back exactly
const dig = await P.ev(`(async () => {
  const s = window.__cyma.sounds, keys = Object.keys(${JSON.stringify(F[0].params)}), base = ${JSON.stringify(F[0].q16)};
  const enums = new Map(), texts = keys.map(() => []), bad = keys.map(() => 0);
  const fixed6 = (x) => { if (typeof x === "string") return x; let t = x.toFixed(6); if (t.includes(".")) t = t.replace(/0+$/, "").replace(/\\.$/, ""); return t === "-0" ? "0" : t; };
  // which parameters are on a grid (ENUM, or an integer): their file values are labels, or whole numbers for every q
  const probe = s.soundToJson("P", base.map(() => 12345)).params, probe2 = s.soundToJson("P", base.map(() => 12346)).params;
  const grid = keys.map((k) => typeof probe[k] === "string" || k === "modes");
  const seen = keys.map(() => new Map());       // the grid parameters: their grid points, in order (the writer's domain)
  for (let q = 0; q < 65536; q++) {
    const qs = base.map(() => q), j = s.soundToJson("X", qs).params, back = s.soundFromJson({ format: "cyma1-sound", version: 1, name: "X", params: j }).q;
    keys.forEach((k, i) => {
      if (!grid[i]) { texts[i].push(fixed6(j[k])); if (back[i] !== q) bad[i]++; }
      else if (!seen[i].has(String(j[k]))) seen[i].set(String(j[k]), fixed6(j[k]));
    });
  }
  keys.forEach((k, i) => { if (grid[i]) texts[i] = [...seen[i].values()]; });
  const out = {};
  for (let i = 0; i < keys.length; i++) {
    const h = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(texts[i].join("\\n")));
    out[keys[i]] = [[...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, "0")).join(""), bad[i], grid[i]];
  }
  return out;
})()`);
const digBad = Object.entries(dig).filter(([k, [h, bad, g]]) => h !== VEC.writer_digest[k] || bad);
check("codec: the exhaustive writer (65,536 values x 40 parameters) matches writer_digest, and reads back exactly", digBad.length === 0,
  digBad.map(([k, [h, bad, g]]) => `${k}${g ? " (grid)" : ""}: ${h === VEC.writer_digest[k] ? "" : "digest differs "}${bad ? bad + " misread" : ""}`).join("; "));
report("codec");

// ============================================================================ 2. save, list, reload, rename, delete
await P.ev("localStorage.clear(), 1");
await P.reload();
check("before switch-on: Save sound waits (disabled, 'Switch on first')", await P.ev("(() => { const b = document.getElementById('sndsave'); return b.disabled || b.getAttribute('aria-disabled') === 'true' || /Switch on first/.test(b.title); })()"));
check("before switch-on: the empty list says how to start", /Nothing saved yet\. Shape a sound on the panel, then press Save sound/.test(await allText()));
check("switched on", await switchOn());
await sleep(800);
check("the bar's Save sound button: its title", /Save the sound you're playing into My Sounds/.test(await P.ev("document.getElementById('sndsave').title")), await P.ev("document.getElementById('sndsave').title"));
await P.click("#sndsave");
const pop = await P.until(`!document.querySelector('${savePop}').hidden`, 2000);
const nm0 = pop ? await P.ev(`${nameInput.slice(1)}.value`) : "";
const focused = pop && await P.ev(`document.activeElement === ${nameInput.slice(1)}`);
check("Save sound opens with the working name, focused", pop && nm0 === "SINGING BOWL" && focused, `value ${JSON.stringify(nm0)}, focused ${focused}`);
check("the name hint (§8.4)", /Up to 14 characters: A–Z, 0–9, space and - \. ' & \+ \/ ! \? # \( \) :\. The FM-1 shows names in capitals\./.test(await allText()));
await P.ev(`(() => { const i = ${nameInput.slice(1)}; i.focus(); i.select(); return 1; })()`);
await P.typeKeys("glass*rain");
const typed = await P.ev(`${nameInput.slice(1)}.value`);
check("typing: capitals, and a character outside the set is dropped with the hint", typed === "GLASSRAIN" && /Only A–Z, 0–9, space and - \. ' & \+ \/ ! \? # \( \) : fit the FM-1's screen\./.test(await allText()), `value ${JSON.stringify(typed)}`);
await P.ev(`(() => { const i = ${nameInput.slice(1)}; i.select(); return 1; })()`);
await P.key("Backspace", "Backspace", 8);
await sleep(150);
const emptyBtn = await P.rect(BTN(savePop, "Save to*"));
check("an empty name: Save waits, 'Type a name first.'", (!emptyBtn || emptyBtn.dis) && /Type a name first\./.test(await allText()), JSON.stringify(emptyBtn));
await P.type("glass rain");
check("the counter (10 / 14) and the slot (U01 · empty, Save to U01)", /10 \/ 14/.test(await allText()) && await P.rect(BTN(savePop, "Save to U01")).then((r) => !!r && r.vis) &&
  /U01 · empty/.test(await P.ev(`[...document.querySelectorAll('${savePop} option')].map((o) => o.textContent).join('|')`)));
await P.click(BTN(savePop, "Save to U01"));
await sleep(500);
check("saved: 'Saved GLASS RAIN to U01.', listed, in localStorage", (await barmsg()) === "Saved GLASS RAIN to U01." && (await bankNames()).join() === "U01 GLASS RAIN" && (await lsNames()).join() === "U01 GLASS RAIN",
  `bar "${await barmsg()}", bank ${(await bankNames()).join()}, stored ${(await lsNames()).join()}`);
check("the row: U01 GLASS RAIN, current (aria-current), in an ordered list", await P.ev("(() => { const l = document.getElementById('sndlist'); return l.tagName === 'OL' && /U01/.test(l.textContent) && /GLASS RAIN/.test(l.textContent) && !!l.querySelector('[aria-current=true]'); })()"));
check("NOW PLAYING: GLASS RAIN, U01, not edited", /NOW PLAYING/.test(await allText()) && await P.ev("/GLASS RAIN/.test(document.querySelector('.now, [aria-labelledby=now-h]')?.textContent || document.body.textContent)"));
const savedQ = (await bank())[0].q;
await P.shot("sounds_saved_1280.png");
await P.reload();
check("reload: still listed before switch-on (from localStorage)", (await bankNames()).join() === "U01 GLASS RAIN" && /GLASS RAIN/.test(await text("#sndlist")), (await bankNames()).join());
await switchOn();
await sleep(600);
check("reload: after switch-on, the bank is the engine's and the same", (await bankNames()).join() === "U01 GLASS RAIN" && JSON.stringify((await bank())[0].q) === JSON.stringify(savedQ));
// ⌘S opens Save sound; a second ⌘S saves (into the first free slot: the sound is a factory one now)
await P.ev("window.__cyma.sounds.call('load', { idx: 1, glide: 0 }).then(() => 1)");
await sleep(400);
await P.key("KeyS", "s", 83, 4);
const cmdS = await P.until(`!document.querySelector('${savePop}').hidden`, 1500);
await P.key("KeyS", "s", 83, 4);
await sleep(600);
check("⌘S opens Save sound, a second ⌘S saves (HANDPAN into U02)", cmdS && (await bankNames()).join() === "U01 GLASS RAIN,U02 HANDPAN", (await bankNames()).join());
await P.ev("window.__cyma.sounds.call('load', { idx: 2, glide: 0 }).then(() => 1)");
await sleep(400);
await P.key("KeyS", "s", 83, 2);
const ctrlS = await P.until(`!document.querySelector('${savePop}').hidden`, 1500);
await P.key("Escape", "Escape", 27);
const escClosed = await P.until(`document.querySelector('${savePop}').hidden`, 1000);
const backFocus = await P.ev("document.activeElement && document.activeElement.id");
check("Ctrl+S opens it too; Esc closes it and gives focus back", ctrlS && escClosed, `focus on ${backFocus}`);
// rename
check("rename: the row's menu", await rowMenu("U01", "Rename"));
await sleep(200);
await P.ev("(() => { const i = document.querySelector('#sndlist input'); i.focus(); i.select(); return 1; })()").catch(() => 0);
await P.type("glass rain ii");
await P.key("Enter", "Enter", 13, 0, "\r");
await sleep(500);
check("rename: Enter saves, 'Renamed U01 to GLASS RAIN II.'", (await msg()) === "Renamed U01 to GLASS RAIN II." && (await bankNames())[0] === "U01 GLASS RAIN II" && JSON.stringify((await bank())[0].q) === JSON.stringify(savedQ),
  `msg "${await msg()}", ${(await bankNames()).join()}`);
// delete + Undo
await rowMenu("U01", "Delete");
await sleep(200);
const confirmTxt = await allText();
const cancelFocused = await P.ev("document.activeElement && document.activeElement.textContent.trim() === 'Cancel'");
check("delete: an inline confirm, 'Delete U01 GLASS RAIN II?', focus on Cancel", /Delete U01 GLASS RAIN II\?/.test(confirmTxt) && cancelFocused, `focus on ${await P.ev("document.activeElement && document.activeElement.textContent")}`);
await P.click(BTN("body", "Delete"));
await sleep(500);
check("deleted: 'Deleted GLASS RAIN II from U01.', gone, the next sound's slot untouched", /Deleted GLASS RAIN II from U01\./.test(await msg()) && (await bankNames()).join() === "U02 HANDPAN" && (await lsNames()).join() === "U02 HANDPAN", `${await msg()} / ${(await bankNames()).join()}`);
await P.click(BTN("body", "Undo"));
await sleep(500);
check("delete Undo: U01 back, the same values", (await bankNames()).join() === "U01 GLASS RAIN II,U02 HANDPAN" && JSON.stringify((await bank())[0].q) === JSON.stringify(savedQ), (await bankNames()).join());
// delete all + Undo
const before3 = await bank();
await P.click("@document.querySelector('#sndlist').closest('article, section').querySelector('[aria-haspopup=menu]:not(#sndlist *)')") || await P.click("@[...document.querySelectorAll('[aria-haspopup=menu]')].find((b) => /My Sounds/.test(b.getAttribute('aria-label') || '') && !b.closest('#sndlist'))");
await sleep(150);
await P.click(`@[...document.querySelectorAll('[role=menu]:not([hidden]) [role=menuitem]')].find((b) => b.textContent.trim().startsWith('Delete all my sounds'))`);
await sleep(200);
check("delete all: the confirm names the count and offers a file first", /Delete all 2 sounds from My Sounds\? Save them as a file first if you might want them\./.test(await allText()) &&
  !!(await P.rect(BTN("body", "Save as file first"))));
await P.click(BTN("body", "Delete all"));
await sleep(500);
check("deleted all: the bank is empty", (await bankNames()).length === 0 && (await lsNames()).length === 0, (await bankNames()).join());
await P.click(BTN("body", "Undo"));
await sleep(500);
check("delete all Undo: both back, byte for byte", JSON.stringify(await bank()) === JSON.stringify(before3), (await bankNames()).join());
report("save, rename, delete");

// ============================================================================ 3. share link arrival, Keep it / Undo
const SHARED = codeOf("SHARED BOWL", F[0].q16), OTHER = codeOf("LINK GONG", F[6].q16);
await P.nav(`${BASE}#${SHARED}`);
await sleep(400);
check("link, engine off: the start note", /Switch on to hear SHARED BOWL, shared with you · best with headphones/.test(await allText()), (await allText()).slice(0, 200));
check("link, engine off: the banner, with Switch on to hear it", /SHARED BOWL was shared with you\./.test(await text("#sndbanner")) && !!(await P.rect(BTN("#sndbanner", "Switch on to hear it"))), await text("#sndbanner"));
const bank0 = await bankNames();
await P.click(BTN("#sndbanner", "Switch on to hear it")) || await switchOn();
await P.until("!!(window.__cyma.S && window.__cyma.S.on)", 12000);
await P.until(`/Playing SHARED BOWL from a shared link/.test(document.getElementById('sndbanner').textContent)`, 6000);
await P.until("/SHARED BOWL/.test((document.getElementById('caption') || {}).textContent || '')", 3000);   // (the caption follows the info line)
const st1 = await state();
check("link, after switch-on: plays it (caption), banner with Keep it and Undo; auditioned; nothing stored",
  /SHARED BOWL/.test(await text("#caption")) && /Playing SHARED BOWL from a shared link\. It isn't in My Sounds yet\./.test(await text("#sndbanner")) &&
  (await P.rect("#sndkeep")).vis && (await P.rect("#sndundo")).vis && (st1 >> 13 & 1) && (await bankNames()).join() === bank0.join(),
  `caption "${await text("#caption")}", banner "${await text("#sndbanner")}", state 0x${(st1 >>> 0).toString(16)}`);
check("link: the banner speaks through #sndmsg without taking focus", /SHARED BOWL/.test(await msg()) && await P.ev("document.activeElement === document.body || !document.getElementById('sndbanner').contains(document.activeElement)"));
await P.click("#sndkeep");
await sleep(600);
check("Keep it: 'Kept SHARED BOWL in U03.', the hash cleared", /Kept SHARED BOWL in U03\./.test((await msg()) + " " + (await text("#sndbanner"))) && (await bankNames()).includes("U03 SHARED BOWL") && (await P.ev("location.hash")) === "",
  `${await msg()} / ${(await bankNames()).join()} / hash ${await P.ev("location.hash")}`);
await P.reload();
check("Keep it survives a reload", (await lsNames()).includes("U03 SHARED BOWL") && !(await P.ev("!document.getElementById('sndbanner').hidden")));
await P.nav(`${BASE}#${SHARED}`);
await switchOn();
await P.until("!document.getElementById('sndkeep').hidden", 6000);
await P.click("#sndkeep");
await sleep(500);
check("the same link again: 'SHARED BOWL is already in My Sounds (U03).'", /SHARED BOWL is already in My Sounds \(U03\)\./.test((await msg()) + " " + (await text("#sndbanner"))) && (await bankNames()).filter((n) => /SHARED BOWL/.test(n)).length === 1,
  await msg());
// Undo an arrival: back to what was playing (01 SINGING BOWL after a fresh load)
await P.ev("window.__cyma.sounds.call('load', { idx: 0, glide: 0 }).then(() => 1)");
await sleep(400);
await P.ev(`(location.hash = ${JSON.stringify("#" + OTHER)}, 1)`);
const hc = await P.until(`/Playing LINK GONG from a shared link/.test(document.getElementById('sndbanner').textContent)`, 5000);
check("hashchange: a link opened in the running page arrives too", hc, await text("#sndbanner"));
await P.click("#sndundo");
await sleep(600);
const st2 = await state();
check("Undo: 'Back to 01 SINGING BOWL.', not auditioned, nothing stored, the hash cleared", /Back to 01 SINGING BOWL\./.test((await msg()) + " " + (await text("#sndbanner"))) && !(st2 >> 13 & 1) && (st2 & 0xFF) === 0 &&
  !(await bankNames()).some((n) => /LINK GONG/.test(n)) && (await P.ev("location.hash")) === "", `${await msg()}, state 0x${(st2 >>> 0).toString(16)}`);
// the short link (revision 2026-10-24b): a #cy2.… link arrives as a #cy1.… one does; Share's Copy link makes one
if (VEC.cy2) {
  const c2m = VEC.cy2.factory[3];                       // 04 MARIMBA
  await P.ev(`(location.hash = ${JSON.stringify("#" + c2m.code)}, 1)`);
  const hc2 = await P.until(`/Playing MARIMBA from a shared link/.test(document.getElementById('sndbanner').textContent)`, 5000);
  check(`link (cy2): #${c2m.code.slice(0, 12)}… (${c2m.length} characters) arrives with Keep it / Undo, nothing stored`,
    hc2 && (await P.rect("#sndkeep")).vis && (await P.rect("#sndundo")).vis && !(await bankNames()).some((n) => /MARIMBA/.test(n)), await text("#sndbanner"));
  await P.click("#sndundo");
  await sleep(500);
  // Copy link with the clipboard refused: the link is left selected; it is <this page>#cy2.<the sound's link code>
  await P.ev(`(() => { try { Object.defineProperty(navigator, "clipboard", { configurable: true, value: { writeText: () => Promise.reject(new DOMException("blocked", "NotAllowedError")) } }); } catch { /* */ } document.execCommand = () => false; return 1; })()`);
  await P.ev("window.__cyma.sounds.call('load', { idx: 4, glide: 0 }).then(() => 1)");   // 05 VIBES
  await sleep(400);
  await P.click("#sndshare");
  await P.until("!document.getElementById('sharepop').hidden", 2000);
  const hasLink = (await P.rect("#sharelink")).vis, boxCode = await P.ev("document.getElementById('sharetext').value");
  if (hasLink) await P.click("#sharelink");
  await sleep(400);
  const sel = await P.ev("(() => { const a = document.activeElement; const t = a && 'selectionStart' in a && a.value ? a.value.slice(a.selectionStart, a.selectionEnd) : String(getSelection()); return t.trim(); })()");
  const want = BASE.split("#")[0] + "#" + VEC.cy2.factory[4].code;
  check(`Share: Copy link gives <this page>#cy2.… for 05 VIBES (${VEC.cy2.factory[4].length} characters after the #), the code box keeps the cy1 code`,
    hasLink && sel === want && boxCode === F[4].code, `selected "${sel}"; the box held ${boxCode.slice(0, 12)}… (${boxCode.length})`);
  await P.key("Escape", "Escape", 27);
  await sleep(200);
  // a sound too detailed for a 128-character anchor: no Copy link, the code and the file instead
  const big = VEC.cy2.edges.shortest_that_does_not;
  await P.ev("(document.getElementById('sndpaste').value = '', 1)");
  await P.click("#sndpaste");
  await P.type(big.code);
  await P.click("#sndopen");
  await P.until("/Playing CATHEDRAL GONG from a pasted code/.test(document.getElementById('sndbanner').textContent)", 4000);
  await P.click("#sndshare");
  await P.until("!document.getElementById('sharepop').hidden", 2000);
  const bigPop = await text("#sharepop");
  check(`Share: a sound whose link code is ${big.length} characters (over ${VEC.cy2.anchor_max}) offers no Copy link, and says to share the code`,
    !(await P.rect("#sharelink")).vis && /code/i.test(await text("#sharebase")) && /^cy1\./.test(await P.ev("document.getElementById('sharetext').value")), bigPop.slice(0, 200));
  await P.key("Escape", "Escape", 27);
  await sleep(200);
  await P.click("#sndundo");
  await sleep(400);
  await P.ev("(history.replaceState(null, '', location.pathname), delete navigator.clipboard, 1)");
}
// damaged, too long, pasted
const damaged = SHARED.slice(0, -6) + (SHARED.slice(-6, -5) === "A" ? "B" : "A") + SHARED.slice(-5);
await P.ev(`(location.hash = ${JSON.stringify("#" + damaged)}, 1)`);
await sleep(600);
check("a damaged link: the exact note, nothing loaded", /This link's sound code is damaged or cut off, so nothing was loaded\. Ask for the link again, or for the code\./.test(await allText()) && !(await bankNames()).some((n) => /SHARED/.test(n) && !/U03/.test(n)));
const st3 = await state();
await P.ev(`(location.hash = ${JSON.stringify("#cy1." + "A".repeat(496))}, 1)`);
await sleep(600);
check("a 500-character fragment is ignored (no banner, nothing played)", (await state()) === st3 && !/from a shared link/.test(await text("#sndbanner")));
await P.ev("(history.replaceState(null, '', location.pathname), 1)");
await P.click("#sndpaste");
await P.type("  " + OTHER.slice(0, 40) + "\n" + OTHER.slice(40) + "  ");
await P.click("#sndopen");
await sleep(600);
check("a pasted code (wrapped by a chat app): 'Playing LINK GONG from a pasted code.'", /Playing LINK GONG from a pasted code\. It isn't in My Sounds yet\./.test(await text("#sndbanner")), await text("#sndbanner"));
await P.click("#sndundo");
await P.ev("(document.getElementById('sndpaste').value = '', 1)");
await P.click("#sndpaste");
await P.type("hello there");
await P.click("#sndopen");
await sleep(400);
check("not a code: 'That isn't a CYMA-1 code. Codes start with cy1.'", /That isn't a CYMA-1 code\. Codes start with cy1\./.test(await allText()));
await P.ev("(document.getElementById('sndpaste').value = '', 1)");
await P.click("#sndpaste");
await P.type(codeOf("FUTURE", F[0].q16, 2));
await P.click("#sndopen");
await sleep(400);
check("a newer code: 'This sound needs a newer CYMA-1 page. Nothing was loaded.'", /This sound needs a newer CYMA-1 page\. Nothing was loaded\./.test(await allText()));
await P.ev("(document.getElementById('sndpaste').value = '', 1)");
report("links");

// ============================================================================ 4. files: export, re-import, bad files
const dlFiles = () => fs.readdirSync(DL).filter((f) => !f.endsWith(".crdownload"));
async function waitDl(pred, ms = 5000) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const f = dlFiles().find(pred); if (f) { await sleep(200); return f; } await sleep(100); } return null; }
await rowMenu("U01", "Save as file");
const one = await waitDl((f) => /\.cyma1\.json$/.test(f));
let oneDoc = null;
try { oneDoc = JSON.parse(fs.readFileSync(path.join(DL, one), "utf8")); } catch { /* */ }
check("Save as file: GLASS RAIN II.cyma1.json, a cyma1-sound with its 40 values", one === "GLASS RAIN II.cyma1.json" && oneDoc && oneDoc.format === "cyma1-sound" && oneDoc.name === "GLASS RAIN II" &&
  Object.keys(oneDoc.params).length === 40 && oneDoc.made_with && oneDoc.made_with.where === "web" && /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(oneDoc.created), one || dlFiles().join(", "));
await P.click("#sndexport");
const all = await waitDl((f) => /\.cyma1-bank\.json$/.test(f));
let allDoc = null;
try { allDoc = JSON.parse(fs.readFileSync(path.join(DL, all), "utf8")); } catch { /* */ }
const nBank = (await bankNames()).length;
check("Save all my sounds as a file: CYMA-1 My Sounds YYYY-MM-DD.cyma1-bank.json, every used slot with its slot", /^CYMA-1 My Sounds \d{4}-\d\d-\d\d\.cyma1-bank\.json$/.test(all || "") && allDoc && allDoc.format === "cyma1-bank" &&
  allDoc.sounds.length === nBank && allDoc.sounds.every((s) => s.slot >= 1 && s.slot <= 32), all || dlFiles().join(", "));
check("the export copy says what it is", new RegExp(`One \\.json with all ${nBank}: a backup, or a way to move them to another browser or to the command-line tool\\.`).test(await allText()));
async function importFile(p) {
  const doc = await P.cdp("DOM.getDocument", { depth: 1 });
  const { nodeId } = await P.cdp("DOM.querySelector", { nodeId: doc.root.nodeId, selector: "#sndimport" });
  await P.cdp("DOM.setFileInputFiles", { nodeId, files: [p] });
  await sleep(700);
}
await importFile(path.join(DL, all));
check("re-import the bank: the sheet sees every sound already there", new RegExp(`has ${nBank} sounds\\. ${nBank} are already in My Sounds\\.`).test(await allText()), (await allText()).match(/has \d+ sounds[^.]*\.[^.]*\./)?.[0]);
await P.click(BTN("body", "Cancel"));
await importFile(path.join(DL, one));
check("re-import one sound: it arrives (banner 'from GLASS RAIN II.cyma1.json')", /Playing GLASS RAIN II from GLASS RAIN II\.cyma1\.json\. It isn't in My Sounds yet\./.test(await text("#sndbanner")), await text("#sndbanner"));
await P.click("#sndundo");
const W = (n, d) => { const p = path.join(FIX, n); fs.writeFileSync(p, typeof d === "string" || Buffer.isBuffer(d) ? d : JSON.stringify(d)); return p; };
const good = soundDoc("FILE SOUND", F[4].params, { author: "Mark" });
const fixtures = [
  ["five_mb.json", Buffer.concat([Buffer.alloc(5 * 1024 * 1024, 0x20), Buffer.from(JSON.stringify(good))]), /That file is too big to be a CYMA-1 bank \(over 1 MB\)\./],
  ["format_v2.cyma1.json", { ...good, version: 2 }, /This file was made with a newer CYMA-1\. Open the newest CYMA-1 page to load it\./],
  ["not_cyma.json", { format: "something-else", version: 1 }, /That file isn't a CYMA-1 sound or bank\./],
  ["broken.json", "{ this is not JSON", /That file isn't a CYMA-1 sound or bank\.|Couldn't read that file\./],
  ["deep.json", "[".repeat(200000) + "]".repeat(200000), /That file isn't a CYMA-1 sound or bank\./],
  ["unknown.cyma1.json", soundDoc("UNKNOWN KEYS", { ...F[4].params, flutter: 3, grain: "fine" }), /Made with a newer CYMA-1: 2 of its settings aren't known here and were left out \(FLUTTER, GRAIN\)\./],
  ["clamped.cyma1.json", soundDoc("CLAMPED", { ...F[4].params, decay: 99, level: 40 }), /2 settings were out of range and were brought inside it \(DECAY, LEVEL\)\./],
  ["pollution.cyma1.json", '{"format":"cyma1-sound","version":1,"name":"POLLUTED","__proto__":{"polluted":1},"params":{"__proto__":{"polluted":2},"constructor":{"prototype":{"polluted":3}},"prototype":1,"shape":2}}', /Playing POLLUTED from pollution\.cyma1\.json/],
  ["author.cyma1.json", good, /Playing FILE SOUND \(by Mark\) from author\.cyma1\.json\./],
  ["older.cyma1.json", soundDoc("OLDER", Object.fromEntries(Object.entries(F[4].params).slice(0, 37))), /OLDER/],
];
for (const [n, d, re] of fixtures) {
  P.problems.length = 0;
  await P.ev("(document.getElementById('sndmsg').textContent = '', 1)");
  await importFile(W(n, d));
  await sleep(300);
  const t = await allText();
  const polluted = await P.ev("({}).polluted !== undefined || Object.prototype.polluted !== undefined");
  check(`bad and odd files: ${n} -> its message, no pollution, no console error`, re.test(t) && !polluted && P.problems.length === 0,
    `${(t.match(re) || [""])[0] || (await msg()) + " | " + (await text("#sndbanner"))}${polluted ? " POLLUTED" : ""}${P.problems.length ? " " + P.problems.join(" | ") : ""}`);
  if (await P.ev("!document.getElementById('sndundo').hidden")) await P.click("#sndundo");
}
check("the DOM got the imported text as text only (no element injected from a name)", await P.ev("!document.querySelector('#sndbanner script, #sndlist script, #sndbanner img, #sndlist img')"));
// a text file of codes: several -> the bank sheet
await importFile(W("codes.txt", `Here are three:\n${F[9].code}\n${F[10].code}\n${F[11].code}).\n`));
check("a text file with 3 codes -> the bank sheet (3 sounds)", /has 3 sounds/.test(await allText()));
await P.click(BTN("body", "Cancel"));
report("files");

// ============================================================================ 5. factory reset keeps My Sounds
const userBefore = await lsUser();
await P.click("#reset");
await sleep(150);
await P.click("#reset");
await P.until("/Back to the factory state\\. My Sounds are kept\\./.test(document.body.innerText)", 6000);
await sleep(800);
const lsAfter = await P.ev("JSON.parse(localStorage.getItem('cyma-1-store') || '{}')");
check("factory reset: 'Back to the factory state. My Sounds are kept.'; OBJ_USER kept byte for byte, the settings forgotten",
  /Back to the factory state\. My Sounds are kept\./.test(await allText()) && (await lsUser()) === userBefore && (await bankNames()).length === nBank,
  `stored objects after: ${Object.keys(lsAfter).join(",")}`);
check("the reset button's explanation (§8.4)", /Forget the settings saved in this browser and start again from the factory state\. My Sounds are kept\. Press twice\./.test(await P.ev("(document.getElementById('reset-why') || {}).textContent || ''")));
report("factory reset");

// ============================================================================ 6. two tabs
await switchOn();
const T2 = await Tab.open();
await T2.metrics(1280, 900);
await T2.nav(BASE);
await switchOn(T2);
await sleep(800);
await T2.click("#tabmine");
const freeA = (await bank()).findIndex((s) => !s);
const err = await saveViaUi("FROM TAB A", P);
await sleep(800);
const stale = await T2.until("/My Sounds was changed in another tab\\. Reload this page before you save here\\./.test(document.body.innerText)", 4000);
const lsA = await lsUser(T2);
const bErr = await saveViaUi("FROM TAB B", T2);   // B tries to save anyway: its Save is disabled, so nothing is written
await T2.key("Escape", "Escape", 27);
await sleep(300);
const bWrote = (await lsUser(T2)) !== lsA || (await bankNames(T2)).some((n) => /FROM TAB B/.test(n));
check("two tabs: A saves; B shows the two-tabs note, and B cannot save over it", !err && stale && !bWrote,
  err || `stale ${stale}; B's attempt: ${bErr || "it went through"}, wrote ${bWrote}`);
// B makes its own settings dirty and lets the firmware autosave them (3 s quiet, 30 s since the last): its whole store goes
// to localStorage again, and A's new slot must survive it
const ls0 = await T2.ev("JSON.parse(localStorage.getItem('cyma-1-store') || '{}')['0'] || ''");
await T2.ev("(document.activeElement && document.activeElement.blur && document.activeElement.blur(), 1)");
await T2.key("KeyX", "x", 88, 0, "x");                 // OCT+ on B's panel: its settings change
const wrote = await T2.until(`(JSON.parse(localStorage.getItem('cyma-1-store') || '{}')['0'] || '') !== ${JSON.stringify(ls0)}`, 45000);
const afterB = await lsNames();
check(`two tabs: B's autosave ${wrote ? "ran and" : "(did not run in 45 s:)"} never overwrote A's new slot`, afterB.includes(`U${String(freeA + 1).padStart(2, "0")} FROM TAB A`), afterB.join());
await T2.click(BTN("body", "Reload"));
await sleep(1500);
check("two tabs: B's Reload brings A's sound in", (await bankNames(T2)).includes(`U${String(freeA + 1).padStart(2, "0")} FROM TAB A`));
report("two tabs, A");
report("two tabs, B", T2);
T2.close();

// ============================================================================ 7. phone width
await P.metrics(390, 844);
await sleep(500);
await P.click("#sndshare");
await sleep(400);
await P.shot("sounds_390_share.png");
const sw = await P.ev("[document.documentElement.scrollWidth, innerWidth]");
check("390 px: no horizontal scroll with the Share popover and its code showing", sw[0] <= sw[1], `${sw[0]} vs ${sw[1]}`);
const small = await P.ev(`[...document.querySelectorAll('#sndlist button, #sndsave, #sndshare, #sndopen, #sndexport')].filter((b) => b.offsetParent !== null).map((b) => { const r = b.getBoundingClientRect(); return [b.textContent.trim().slice(0, 14), Math.round(r.width), Math.round(r.height)]; }).filter(([, w, h]) => w < 44 || h < 44)`);
check("390 px: every Sounds target is at least 44 x 44 px", small.length === 0, JSON.stringify(small.slice(0, 6)));
await P.key("Escape", "Escape", 27);
await P.metrics(1280, 900);
report("phone");

// ============================================================================ 7b. the hardening round (spec revision 2026-10-24b)
// At 390 px every control of the Save and Share popovers and of both ⋯ menus is a 44 x 44 px target (§8.1; the bar's
// buttons were the only ones checked before). The code box shows the longest code whole. An Undo offers no second Undo,
// and leaves the keyboard focus on a control, not on the page. "Delete all" with one sound speaks of one sound. A credit
// carrying Unicode format characters (bidi overrides, zero-width) is shown and saved without them.
const HARD = "hardening";
const smallIn = (sel) => P.ev(`[...document.querySelectorAll(${JSON.stringify(sel)})].filter((b) => b.offsetParent !== null && !b.closest("[hidden]") && getComputedStyle(b).visibility !== "hidden")
  .map((b) => { const r = b.getBoundingClientRect(); return [(b.id ? "#" + b.id + " " : "") + (b.textContent || b.value || b.tagName).trim().slice(0, 18), Math.round(r.width * 10) / 10, Math.round(r.height * 10) / 10]; })`);
const tooSmall = (list) => list.filter(([, w, h]) => w < 44 || h < 44);
const visUndos = () => P.ev(`[...document.querySelectorAll("#sndmsg button, #barmsg button, #sounds button, .msgs button")].filter((b) => b.id !== "sndundo" && b.offsetParent !== null && !b.closest("[hidden]") && /^Undo$/.test(b.textContent.trim())).length`);
const focusAt = () => P.ev(`(() => { const a = document.activeElement; if (!a || a === document.body || a === document.documentElement || !a.isConnected) return "the page (body)";
  return (a.id ? "#" + a.id : a.tagName.toLowerCase()) + (a.closest("#sounds") ? " in #sounds" : a.closest(".bar, #bar") ? " in the bar" : a.closest(".msgs") ? " in the messages" : " elsewhere") + ": " + (a.getAttribute("aria-label") || a.textContent || "").trim().slice(0, 30); })()`);
const focusOk = (f) => f !== "the page (body)" && !/ elsewhere:/.test(f);
const CONFIRM_DEL = `@[...document.querySelectorAll('#sndconfirm button')].find((b) => b.offsetParent !== null && /^Delete/.test(b.textContent.trim()))`;
async function menuShut() {                     // every ⋯ menu closed (Esc, then a click on the section's heading)
  for (let i = 0; i < 3 && await P.ev("[...document.querySelectorAll('[role=menu]')].some((m) => !m.hidden)"); i++) {
    await P.key("Escape", "Escape", 27);
    await sleep(150);
    if (await P.ev("[...document.querySelectorAll('[role=menu]')].some((m) => !m.hidden)")) await P.click("#snd-h");
    await sleep(150);
  }
}
async function deleteAllAsk() {                 // My Sounds' ⋯ -> Delete all my sounds… (the inline confirm shows)
  await menuShut();
  await P.click("#sndmore");
  await P.until("!document.getElementById('sndmoremenu').hidden", 1500);
  await P.click("#sndmoredelete");
  if (await P.until("!document.getElementById('sndconfirm').hidden", 1000)) return true;
  // (a menu that opens past the viewport's edge makes a phone zoom out, and a real click then misses: the fit is its
  // own check above; the confirm is still reached, by the item's own click)
  await menuShut();
  await P.ev("(document.getElementById('sndmore').click(), 1)");
  await P.until("!document.getElementById('sndmoremenu').hidden", 1500);
  await P.ev("(document.getElementById('sndmoredelete').click(), 1)");
  return P.until("!document.getElementById('sndconfirm').hidden", 1500);
}
await P.metrics(390, 844);
await sleep(400);
await switchOn();
if ((await bankNames()).length < 2) { await saveViaUi("HARD ONE"); await saveViaUi("HARD TWO"); }
// the Save popover
await P.click("#sndsave");
await P.until(`!document.querySelector('${savePop}').hidden`, 2000);
await sleep(200);
const sv = await smallIn("#savepop button, #savepop input, #savepop select");
check(`${HARD}: 390 px, every control of the Save popover is at least 44 x 44 px (${sv.length} controls)`, sv.length >= 3 && tooSmall(sv).length === 0, JSON.stringify(tooSmall(sv).length ? tooSmall(sv) : sv));
await P.key("Escape", "Escape", 27);
await sleep(200);
// the Share popover
await P.click("#sndshare");
await P.until("!document.getElementById('sharepop').hidden", 2000);
await sleep(200);
const sh = await smallIn("#sharepop button, #sharepop input");
check(`${HARD}: 390 px, every control of the Share popover is at least 44 x 44 px (${sh.length} controls)`, sh.length >= 3 && tooSmall(sh).length === 0, JSON.stringify(tooSmall(sh).length ? tooSmall(sh) : sh));
await P.key("Escape", "Escape", 27);
await sleep(200);
// a row's ⋯ menu, and the card's
const firstRow = (await bankNames())[0].slice(0, 3);
const moreBtn = `@[...document.querySelectorAll('#sndlist [aria-haspopup=menu]')].find((b) => (b.getAttribute('aria-label') || '').includes(${JSON.stringify(firstRow)}))`;
await P.click(moreBtn);
await sleep(250);
const rm = await smallIn("#rowmenu [role=menuitem], #rowmenu button");
const fitOf = (sel) => P.ev(`(() => { const m = document.querySelector(${JSON.stringify(sel)}); const r = m.getBoundingClientRect(); return { open: !m.hidden, left: Math.round(r.left), right: Math.round(r.right), scrollWidth: document.documentElement.scrollWidth }; })()`);
const rmFit = await fitOf("#rowmenu");
check(`${HARD}: 390 px, every item of a row's ⋯ menu is at least 44 x 44 px (${rm.length} items)`, rm.length >= 3 && tooSmall(rm).length === 0, JSON.stringify(tooSmall(rm).length ? tooSmall(rm) : rm));
await menuShut();
await sleep(200);
await P.click("#sndmore");
await sleep(250);
const cm = await smallIn("#sndmoremenu [role=menuitem], #sndmoremenu button");
const cmFit = await fitOf("#sndmoremenu");
check(`${HARD}: 390 px, both ⋯ menus open inside the screen, with no sideways scroll (a row's: ${rmFit.left}-${rmFit.right} px; My Sounds': ${cmFit.left}-${cmFit.right} px)`,
  rmFit.open && cmFit.open && rmFit.left >= 0 && rmFit.right <= 390 && cmFit.left >= 0 && cmFit.right <= 390 && rmFit.scrollWidth <= 390 && cmFit.scrollWidth <= 390,
  `row menu ${JSON.stringify(rmFit)}; My Sounds' menu ${JSON.stringify(cmFit)}`);
check(`${HARD}: 390 px, every item of My Sounds' ⋯ menu is at least 44 x 44 px (${cm.length} items)`, cm.length >= 2 && tooSmall(cm).length === 0, JSON.stringify(tooSmall(cm).length ? tooSmall(cm) : cm));
await menuShut();
await sleep(200);
// the code box: the longest code there is (a 14-character name, every value away from its default) shown whole, at 390
// and at 1280 px (the popover's textarea scrolls nothing out of sight)
const longQ = DEF.map((d, i) => (d === 0 ? 1 : d - 1) & 0xFFFF);
const longCode = await P.ev(`window.__cyma.sounds.codeEncode("CATHEDRAL GONG", ${JSON.stringify(longQ)})`);
await P.ev("(document.getElementById('sndpaste').value = '', 1)");
await P.click("#sndpaste");
await P.type(longCode);
await P.click("#sndopen");
await P.until("/Playing CATHEDRAL GONG from a pasted code/.test(document.getElementById('sndbanner').textContent)", 4000);
const boxAt = async (w, h) => {
  await P.metrics(w, h);
  await sleep(300);
  await P.click("#sndshare");
  await P.until("!document.getElementById('sharepop').hidden", 2000);
  await sleep(250);
  const r = await P.ev(`(() => { const t = document.getElementById("sharetext"); const cs = getComputedStyle(t); const lh = parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.2;
    return { len: t.value.length, code: t.value.slice(0, 4), sh: t.scrollHeight, ch: t.clientHeight, rows: t.rows, lines: Math.round((t.scrollHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom)) / lh) }; })()`);
  await P.shot(`hard_codebox_${w}.png`);
  await P.key("Escape", "Escape", 27);
  await sleep(200);
  return r;
};
const b390 = await boxAt(390, 844), b1280 = await boxAt(1280, 900);
check(`${HARD}: the code box shows the longest code (${b390.len} characters) whole, at 390 px and at 1280 px`,
  b390.len >= 130 && b390.sh <= b390.ch + 1 && b1280.sh <= b1280.ch + 1, `390: ${JSON.stringify(b390)}; 1280: ${JSON.stringify(b1280)}`);
await P.click("#sndundo");
await sleep(400);
await P.metrics(390, 844);
await sleep(300);
// one Undo, and no second one: a single delete
const before7 = await bank();
const delName = (await bankNames())[0];
await rowMenu(delName.slice(0, 3), "Delete");
await sleep(200);
await P.click(BTN("body", "Delete"));
await sleep(500);
await P.click(BTN("#sndmsg, .msgs", "Undo"));
await sleep(600);
const f1 = await focusAt(), u1 = await visUndos();
check(`${HARD}: Undo a delete -> ${delName} back, no second Undo offered, the focus on a control (${f1})`,
  JSON.stringify(await bank()) === JSON.stringify(before7) && u1 === 0 && focusOk(f1), `undo buttons ${u1}; bank ${(await bankNames()).join()}`);
// delete all (several) -> Undo -> no second Undo; the bank stays as it was
await deleteAllAsk();
await P.click(CONFIRM_DEL);
await sleep(500);
const emptied2 = (await bankNames()).length === 0;
await P.click(BTN("#sndmsg, .msgs", "Undo"));
await sleep(600);
const f2 = await focusAt(), u2 = await visUndos();
await sleep(1200);
check(`${HARD}: Undo a delete all -> 'My Sounds is back as it was.', no second Undo, the focus on a control (${f2})`,
  emptied2 && /My Sounds is back as it was\./.test(await msg()) && u2 === 0 && focusOk(f2) && JSON.stringify(await bank()) === JSON.stringify(before7),
  `${emptied2 ? "" : "the delete all did not happen; "}msg "${await msg()}", undo buttons ${u2}, bank ${(await bankNames()).join()}`);
// a save over a used slot (the bar's Undo) -> Undo -> no second Undo
const overSlot = (await bankNames())[0];
await P.click("#sndsave");
await P.until(`!document.querySelector('${savePop}').hidden`, 2000);
await P.ev(`(() => { const sel = document.querySelector('${savePop} select'); if (sel) { sel.value = sel.options[0].value; sel.dispatchEvent(new Event('change', { bubbles: true })); } return 1; })()`);
await P.ev(`(() => { const i = ${nameInput.slice(1)}; i.focus(); i.select(); return 1; })()`);
await P.type("OVER IT");
await P.click(BTN(savePop, "Replace*")) || await P.click(BTN(savePop, "Save to*"));
await sleep(500);
const overMsg = await barmsg();
await P.click(BTN("#barmsg, .msgs", "Undo"));
await sleep(600);
const f3 = await focusAt(), u3 = await visUndos();
check(`${HARD}: Undo a save over ${overSlot.slice(0, 3)} -> the old sound back, no second Undo, the focus on a control (${f3})`,
  /over U\d\d \(was /.test(overMsg) && JSON.stringify(await bank()) === JSON.stringify(before7) && u3 === 0 && focusOk(f3),
  `"${overMsg}" -> "${await barmsg()}" / "${await msg()}", undo buttons ${u3}`);
// "Delete all" with exactly one sound: singular words
await deleteAllAsk();
await P.click(CONFIRM_DEL);
await sleep(500);
await saveViaUi("ONLY ONE");
await sleep(300);
const one1 = (await bankNames()).join();
const asked1 = await deleteAllAsk();
const askLine = await text("#sndconfirm p");
await P.click(CONFIRM_DEL);
await sleep(500);
const doneOne = await msg();
check(`${HARD}: delete all with one sound: singular words ("${askLine}" / "${doneOne.replace(/Undo$/, "").trim()}")`,
  asked1 && one1 === "U01 ONLY ONE" && /\b1 sound\b|ONLY ONE|your sound|the sound/i.test(askLine) && !/\ball 1 sounds?\b|\b1 sounds\b|\bthem\b/i.test(askLine) &&
  (await bankNames()).length === 0 && !/\ball 1 sounds?\b|\b1 sounds\b/i.test(doneOne), `bank before: ${one1}`);
await P.click(BTN("#sndmsg, .msgs", "Undo"));
await sleep(600);
const f4 = await focusAt(), u4 = await visUndos();
check(`${HARD}: ... and its Undo brings ONLY ONE back, with no second Undo and the focus on a control (${f4})`,
  (await bankNames()).join() === "U01 ONLY ONE" && u4 === 0 && focusOk(f4), `undo buttons ${u4}; ${(await bankNames()).join()}`);
await P.metrics(1280, 900);
await sleep(300);
// a credit with Unicode format characters: the right-to-left override that makes "Mark‮exe.gnp" read as "Mark png.exe",
// isolates, zero-width characters and the BOM never reach the banner, and the page's own credit field saves none
const CF = "‮gnp.exe⁦ Mark​‍﻿⁩؜";
P.problems.length = 0;
await importFile(W("cf_author.cyma1.json", soundDoc("CF AUTHOR", F[5].params, { author: CF, notes: "a‮b​c" })));
await sleep(300);
const cfBanner = await text("#sndbanner");
const hasCf = (s) => /[\p{Cf}]/u.test(s);
check(`${HARD}: a file's credit loses its format characters (bidi overrides, zero-width, BOM) before it is shown: "${cfBanner.slice(0, 80)}"`,
  /CF AUTHOR/.test(cfBanner) && /Mark/.test(cfBanner) && !hasCf(cfBanner) && !hasCf(await allText()), [...cfBanner].filter((c) => hasCf(c)).map((c) => "U+" + c.codePointAt(0).toString(16).toUpperCase()).join(" "));
if (await P.ev("!document.getElementById('sndundo').hidden")) await P.click("#sndundo");
await sleep(300);
for (const f of dlFiles()) if (/\.cyma1\.json$/.test(f)) fs.rmSync(path.join(DL, f), { force: true });
await P.click("#sndshare");
await P.until("!document.getElementById('sharepop').hidden", 2000);
await P.ev(`(() => { const i = document.getElementById("shareauthor"); i.focus(); i.value = ${JSON.stringify("Mark" + CF)}; i.dispatchEvent(new Event("input", { bubbles: true })); return 1; })()`);
await P.click("#sharefile");
const cfFile = await waitDl((f) => /\.cyma1\.json$/.test(f));
let cfDoc = null;
try { cfDoc = JSON.parse(fs.readFileSync(path.join(DL, cfFile), "utf8")); } catch { /* */ }
check(`${HARD}: the page's own credit field saves no format characters into a file (author ${JSON.stringify(cfDoc && cfDoc.author)})`,
  cfDoc && typeof cfDoc.author === "string" && /Mark/.test(cfDoc.author) && !hasCf(cfDoc.author), cfFile || dlFiles().join(", "));
await P.key("Escape", "Escape", 27);
await P.ev(`(() => { const i = document.getElementById("shareauthor"); i.value = ""; i.dispatchEvent(new Event("input", { bubbles: true })); return 1; })()`);
report(HARD);

// ============================================================================ 8. the FM-1 over USB, against a fake one
// A fake Web MIDI: one input and one output named like an FM-1. Behind it, a second copy of the firmware (app.wasm)
// renders in real time in the page; HS_QUERY is answered with the identity given; every frame the page sends is logged.
const FAKE = (identity) => `(() => {
  const ID = ${JSON.stringify(identity)};
  const log = window.__fakeLog = [];
  const fk = window.__fake = { answer: "yes", unplugged: false, dieAfter: 0, nData: 0, ex: null, frames: 0, asked: 0 };
  const pack7 = (b) => { const o = []; let acc = 0, nb = 0; for (const x of b) { acc |= x << nb; nb += 8; while (nb >= 7) { o.push(acc & 0x7F); acc >>>= 7; nb -= 7; } } if (nb) o.push(acc & 0x7F); return o; };
  const idFrame = () => { const id = new Uint8Array(27); for (let i = 0; i < Math.min(27, ID.length); i++) id[i] = ID.charCodeAt(i); let s = 0; for (const v of id) s += v;
    return Uint8Array.from([0xF0, ...pack7([0, 0x59, 0x11, 27, 0, 0, ...id, (~s) & 255]), 0xF7]); };
  const listeners = new Set();
  const deliver = (u8) => { const ev = { data: u8, timeStamp: performance.now(), type: "midimessage" }; for (const l of [...listeners]) try { l(ev); } catch (e) { console.error(e); } if (input.onmidimessage) input.onmidimessage(ev); };
  async function boot() {
    if (fk.ex) return;
    const buf = await (await fetch("/build/emu/app.wasm")).arrayBuffer();
    let mem;
    const wasi = { clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(fk.frames / 44100 * 1e9)), true); return 0; },
      fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; }, proc_exit: () => {} };
    const { instance } = await WebAssembly.instantiate(buf, { wasi_snapshot_preview1: new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) }), env: new Proxy({}, { get: () => () => 0 }) });
    const ex = instance.exports; mem = ex.memory; fk.mem = mem;
    if (ex._initialize) ex._initialize();
    if (fk.store) { new Uint8Array(mem.buffer, ex.web_store(2), 3020).set(fk.store); ex.web_store_set_len(2, 3020); }
    ex.web_master(4096); ex.web_boot();
    for (let i = 0; i < 2000 / 23; i++) { ex.web_render(1024); fk.frames += 1024; }
    fk.ex = ex;
    let pressT = 0;
    fk.timer = setInterval(() => {
      for (let i = 0; i < 4; i++) { ex.web_render(128); fk.frames += 128; }   // ~ real time (512 frames per 11.6 ms tick)
      const st = ex.web_cy_state() >>> 0;
      if (st & (1 << 15)) { if (!fk.asked) fk.asked = performance.now(); if (fk.answer !== "none" && performance.now() - fk.asked > 300 && !pressT) { ex.web_buttons(fk.answer === "yes" ? 1 << 7 : 1 << 6); pressT = performance.now(); } }
      else fk.asked = 0;
      if (pressT && performance.now() - pressT > 150) { ex.web_buttons(0); pressT = 0; }
      const n = ex.web_cy_sx_out_len() >>> 0;
      if (n) { const b = new Uint8Array(mem.buffer, ex.web_cy_sx_out(), n).slice(); ex.web_cy_sx_out_clear(); if (!fk.unplugged) { let i = 0; while (i < b.length) { let j = i; while (b[j] !== 0xF7) j++; deliver(b.slice(i, j + 1)); i = j + 1; } } }
    }, 12);
  }
  fk.bank = () => { const ex = fk.ex; if (!ex || ex.web_store_len(2) !== 3020) return null; return [...new Uint8Array(fk.mem.buffer, ex.web_store(2), 3020)]; };
  fk.state = () => fk.ex ? fk.ex.web_cy_state() >>> 0 : -1;
  const input = { id: "fake-in", name: "CYMA-1 FM-1", manufacturer: "CYMA", type: "input", state: "connected", connection: "open", onmidimessage: null,
    addEventListener: (t, l) => { if (t === "midimessage") listeners.add(l); }, removeEventListener: (t, l) => listeners.delete(l),
    open: () => Promise.resolve(input), close: () => Promise.resolve(input) };
  const output = { id: "fake-out", name: "CYMA-1 FM-1", manufacturer: "CYMA", type: "output", state: "connected", connection: "open",
    open: () => Promise.resolve(output), close: () => Promise.resolve(output), clear() {}, addEventListener() {}, removeEventListener() {},
    send(data) {
      const u8 = Uint8Array.from(data); log.push([...u8]);
      if (fk.unplugged) return;
      if (u8.length === 10 && [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].every((b, i) => u8[i] === b)) { setTimeout(() => deliver(idFrame()), 5); return; }
      if (u8[0] !== 0xF0 || u8[u8.length - 1] !== 0xF7 || !/^FM-1_94/.test(ID)) return;     // (a stock unit ignores the rest)
      fk.nData++;
      if (fk.dieAfter && fk.nData >= fk.dieAfter) { fk.unplugged = true; return; }
      boot().then(() => { const ex = fk.ex, inner = u8.subarray(1, u8.length - 1); new Uint8Array(fk.mem.buffer, ex.web_cy_sx_in(), 640).set(inner); ex.web_cy_sx(inner.length); });
    } };
  const access = { inputs: new Map([["fake-in", input]]), outputs: new Map([["fake-out", output]]), sysexEnabled: true, onstatechange: null, addEventListener() {}, removeEventListener() {} };
  window.__fakeRequests = [];
  Object.defineProperty(navigator, "requestMIDIAccess", { configurable: true, value: (opts) => { window.__fakeRequests.push(opts || {}); return opts && opts.sysex ? boot().then(() => access) : Promise.resolve(access); } });
})();`;
const allow = (f) => (f.length === 10 && [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].every((b, i) => f[i] === b)) ||
  (f.length >= 13 && f.length <= 306 && f[0] === 0xF0 && f[1] === 0x7D && f[2] === 0x43 && f[3] === 0x59 && f[4] === 1 && f[f.length - 1] === 0xF7 && f.slice(1, -1).every((x) => x < 0x80));
async function fakePage(identity) {
  const t = await Tab.open();
  await t.cdp("Page.addScriptToEvaluateOnNewDocument", { source: FAKE(identity) });
  await t.metrics(1280, 900);
  await t.nav(BASE.replace("127.0.0.1", "localhost"));   // (an origin of its own: an empty My Sounds)
  return t;
}
const D = await fakePage("FM-1_9400");
await switchOn(D);
await sleep(600);
check("FM-1 card, Web MIDI here: the ready text and Connect, and the warning before the prompt",
  /Send sounds to an FM-1 running CYMA-1, or get the ones on it, over USB\./.test(await text("#fm1card", D)) && (await D.rect("#fm1connect")).vis &&
  /CYMA-1 only reads and writes sounds: it never updates the firmware\./.test(await text("#fm1card", D)), await text("#fm1card", D));
check("FM-1: nothing sent, no sysex access asked, before Connect", (await D.ev("window.__fakeLog.length")) === 0 && !(await D.ev("window.__fakeRequests.some((o) => o.sysex)")));
await D.click("#fm1connect");
const conn = await D.until("/Connected: CYMA-1 on the FM-1 · 0 of 32 slots used\\./.test(document.getElementById('fm1card').textContent)", 15000);
check("FM-1: Connect -> identity, HELLO, 'Connected: CYMA-1 on the FM-1 · 0 of 32 slots used.'", conn, await text("#fm1card", D));
// My Sounds in this page: two sounds to send
await D.ev("localStorage.clear(), 1");
for (const [i, n] of [[0, "ALPHA"], [1, "BETA"]]) {
  await D.ev(`window.__cyma.sounds.call('load', { idx: ${i + 3}, glide: 0 }).then(() => 1)`);
  await sleep(200);
  await saveViaUi(n, D);
}
const pageNames = await bankNames(D);
await D.click(BTN("#fm1card", "Send My Sounds to FM-1*"));
await sleep(300);
check("FM-1: Send My Sounds asks first, naming what will be gone", /Replace all 32 slots on the FM-1 with My Sounds \(2\)\?/.test(await text("#fm1card", D)), await text("#fm1card", D));
await D.click(BTN("#fm1card", "Replace"));
const waiting = await D.until("/Waiting for you on the FM-1: press SAVE to keep them, or HOME to cancel \\(30 s\\)\\./.test(document.getElementById('fm1card').textContent)", 8000);
const sentOk = await D.until("/Saved 2 sounds on the FM-1\\. On the FM-1, turn PRESETS past 24 to find them\\./.test(document.getElementById('fm1card').textContent)", 15000);
const fb = decodeBank(Buffer.from(await D.ev("window.__fake.bank() || []")).toString("base64"));
check("FM-1: 'Waiting for you on the FM-1…', SAVE pressed there -> 'Saved 2 sounds on the FM-1.'; its bank is My Sounds",
  waiting && sentOk && fb && fb.slots && fb.slots.map((s, i) => (s ? `U${String(i + 1).padStart(2, "0")} ${s.name}` : null)).filter(Boolean).join() === pageNames.join(),
  `${await text("#fm1card", D)} | fake: ${fb && fb.slots ? fb.slots.filter(Boolean).map((s) => s.name).join() : "-"}`);
await D.click(BTN("#fm1card", "Get sounds from FM-1"));
const got = await D.until("/The FM-1 has 2 sounds\\./.test(document.body.innerText)", 8000);
check("FM-1: Get sounds -> 'The FM-1 has 2 sounds.' and the bank sheet", got && /has 2 sounds/.test(await allText(D)));
await D.click(BTN("body", "Cancel"));
await D.ev("window.__cyma.sounds.call('load', { idx: 7, glide: 0 }).then(() => 1)");
await sleep(300);
await D.click(BTN("#fm1card", "Play current sound on FM-1"));
const played = await D.until("/Playing .+ on the FM-1\\. Press SAVE there to keep it, or SEL\\+SAVE to go back\\./.test(document.getElementById('fm1card').textContent)", 8000);
check("FM-1: Play current sound -> it plays there (auditioned), nothing saved", played && ((await D.ev("window.__fake.state()")) >> 13 & 1) === 1, await text("#fm1card", D));
// declined
await D.ev("(window.__fake.answer = 'no', 1)");
await D.click(BTN("#fm1card", "Send My Sounds to FM-1*"));
await D.click(BTN("#fm1card", "Replace"));
const declined = await D.until("/You pressed HOME on the FM-1, so nothing on it was changed\\./.test(document.getElementById('fm1card').textContent)", 15000);
check("FM-1: HOME on the FM-1 -> 'You pressed HOME on the FM-1, so nothing on it was changed.'", declined, await text("#fm1card", D));
// unplugged mid-send: the bank on the fake unchanged
await D.ev("(window.__fake.answer = 'yes', 1)");
await saveViaUi("GAMMA", D);
const fakeBefore = await D.ev("JSON.stringify(window.__fake.bank())");
await D.ev("(window.__fake.dieAfter = window.__fake.nData + 3, 1)");
await D.click(BTN("#fm1card", "Send My Sounds to FM-1*"));
await D.click(BTN("#fm1card", "Replace"));
const lost = await D.until("/The FM-1 stopped answering, so nothing on it was changed\\. Check the cable and try again\\./.test(document.getElementById('fm1card').textContent)", 20000);
check("FM-1: unplugged mid-send -> 'The FM-1 stopped answering…', and nothing on it changed", lost && (await D.ev("JSON.stringify(window.__fake.bank())")) === fakeBefore, await text("#fm1card", D));
const logD = await D.ev("window.__fakeLog");
const badD = logD.filter((f) => !allow(f));
check(`FM-1: every one of the ${logD.length} frames the page sent passes the allowlist; the first is HS_QUERY`, badD.length === 0 && logD.length > 0 && allow(logD[0]) && logD[0].length === 10,
  badD.slice(0, 2).map((f) => Buffer.from(f).toString("hex")).join(" "));
report("FM-1 (fake CYMA-1)", D);
D.close();
const S = await fakePage("FM-1_015");
await switchOn(S);
await S.click("#fm1connect");
const stock = await S.until("/This FM-1 runs M-VAVE's own firmware \\(FM-1_015\\)\\. Install CYMA-1 to send sounds\\./.test(document.getElementById('fm1card').textContent)", 15000);
const logS = await S.ev("window.__fakeLog");
check("FM-1: a stock unit (FM-1_015) -> its message, and it received only HS_QUERY", stock && logS.length >= 1 && logS.every((f) => f.length === 10 && allow(f)),
  `${await text("#fm1card", S)} | ${logS.length} frames: ${logS.map((f) => Buffer.from(f).toString("hex").slice(0, 20)).join(" ")}`);
report("FM-1 (fake stock unit)", S);
S.close();
// the identity rule (spec revision 2026-10-24b R5; one rule here and in the CLI): the 27 bytes with the trailing NULs
// removed, nothing else. A byte after an interior NUL stays in the text, so the gate refuses it (it fails closed)
const NUL = String.fromCharCode(0);
const idRule = await P.ev(`(() => { const s = window.__cyma.sounds, pack7 = (b) => { const o = []; let acc = 0, nb = 0; for (const x of b) { acc |= x << nb; nb += 8; while (nb >= 7) { o.push(acc & 0x7F); acc >>>= 7; nb -= 7; } } if (nb) o.push(acc & 0x7F); return o; };
  const fr = (t) => { const id = new Uint8Array(27); for (let i = 0; i < Math.min(27, t.length); i++) id[i] = t.charCodeAt(i); return Uint8Array.from([0xF0, ...pack7([0, 0x59, 0x11, 27, 0, 0, ...id, 0]), 0xF7]); };
  const N = String.fromCharCode(0);
  return [s.sxIdentity(fr("FM-1_9400")), s.sxIdentity(fr("FM-1_9400" + N + "junk")), s.sxIdentity(fr(N + "FM-1_9400")), s.sxIdentity(fr("FM-1_9400" + N + N + "!"))]; })()`);
check("FM-1: the identity rule: trailing NULs removed, nothing else ('FM-1_9400' kept; NUL then bytes keeps the NUL, so the gate refuses it)",
  idRule[0] === "FM-1_9400" && idRule[1] === "FM-1_9400" + NUL + "junk" && idRule[2] === NUL + "FM-1_9400" && idRule[3] === "FM-1_9400" + NUL + NUL + "!" &&
  idRule.slice(1).every((t) => !/^FM-1_94(00|[0-9]{6})$/.test(t)), JSON.stringify(idRule));
const J = await fakePage("FM-1_9400" + NUL + "junk");      // (the fake answers our frames as CYMA-1 would: only the gate stops them)
await switchOn(J);
await J.click("#fm1connect");
const junk = await J.until("/This FM-1 runs other firmware \\(FM-1_9400junk\\), not CYMA-1\\./.test(document.getElementById('fm1card').textContent)", 15000);
const logJ = await J.ev("window.__fakeLog");
check("FM-1: an identity with bytes after its NUL ('FM-1_9400' NUL 'junk') -> 'other firmware', and it received only HS_QUERY",
  junk && logJ.length >= 1 && logJ.every((f) => f.length === 10 && allow(f)),
  `${await text("#fm1card", J)} | ${logJ.length} frames: ${logJ.map((f) => Buffer.from(f).toString("hex").slice(0, 20)).join(" ")}`);
report("FM-1 (fake, identity with bytes after its NUL)", J);
J.close();

fs.writeFileSync(path.join(OUT, "sounds_chrome.json"), JSON.stringify({ chrome: ver.Browser, failures, results }, null, 1));
console.log(failures ? `\nsounds_chrome: ${failures} FAILED of ${results.length}` : `\nsounds_chrome: all ${results.length} passed`);
P.close();
cleanup();
process.exit(failures ? 1 : 0);
