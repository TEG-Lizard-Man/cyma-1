// SPDX-License-Identifier: MPL-2.0
// CYMA-1's virtual FM-1 (docs/CYMA-1-sounds-spec.md §9.4; the line protocol is frozen, I9): the firmware's own
// app.wasm, the page's whole FM-1, run in node and spoken to over stdin/stdout instead of USB-MIDI. tools/cyma_sounds.py
// --virtual drives it, so every sound transfer can be tried and tested with no hardware and no MIDI port at all.
//
//   node tools/sx_bridge.mjs [--wasm build/emu/app.wasm] [--identity FM-1_9400] [--answer yes|no|none]
//                            [--answer-ms 500] [--store FILE.json]
//
//   stdin  (one per line):  <HEX frame F0..F7>  |  !press SAVE|HOME|SEL+SAVE [ms]  |  !advance MS  |  !quit
//   stdout (one per line):  <HEX frame F0..F7>  |  #state <web_cy_state as decimal> (when it changes)  |  #ready
//
// How it behaves:
//  - it instantiates app.wasm with the worklet's WASI stub (as tools/wasm_smoke.mjs does: the clock is the device's,
//    from the frames rendered), gives back the saved objects (--store), boots, plays its first 2 s at full speed
//    (most of the splash, which is the screen's alone), prints #ready, and from then on renders in real time: 441
//    frames every 10 ms, as the AudioWorklet would.
//    The device's own clock (web_now) is the rendered audio, so its 30 s question and 60 s staging timeouts run as
//    on the FM-1, and !advance MS fast-forwards them;
//  - every frame goes to web_cy_sx (app_sysex, the firmware's real handler) first, as on the FM-1, where the platform's
//    own handlers see only what app_sysex declines. HS_QUERY, fm1-base's identity query, once declined, is answered
//    here with a correctly framed identity reply for --identity (the wasm has no ota_service). A frame app_sysex takes
//    that is not CYMA-1's (7D 43 59) is reported on stderr as a SAFETY problem. The replies are drained from
//    web_cy_sx_out after every call and every render chunk, and printed whole;
//  - frames the FM-1's USB layer would drop are dropped here too: more than 640 bytes between F0 and F7, or a byte
//    of 0x80 or more inside (a status byte aborts a SysEx frame);
//  - --answer yes|no presses SAVE or HOME --answer-ms (device time) after state bit 15 (a USB question open) rises.
//    none (the default) leaves it to !press, or to the 30 s timeout;
//  - !press NAMES [ms] holds the named buttons (B_* names joined by +: SAVE, HOME, SEL+SAVE, ...; the modifiers go
//    down 40 ms first) for ms of device time (default 150);
//  - --store FILE loads the store objects at start and saves them after every write, in the page's own format (the
//    value of localStorage["cyma-1-store"]: {"0": base64, "1": ..., "2": ...}), so a bank survives between runs;
//  - an app.wasm without the sound exports (an older CYMA-1) still answers the identity query and nothing else, which
//    is exactly what an FM-1 with an older CYMA-1 does.
// Test hooks: --die-after N exits at once, saving nothing and answering nothing, when the Nth frame other than HS_QUERY
// arrives (a cable pulled mid-transfer). --identity-hex HEX answers the identity query with exactly these bytes (at
// most 27, zero-filled as ota_reply_identity does) instead of --identity's text: a NUL with bytes after it, control
// bytes, bytes >= 0x80, so the tools' identity gate can be tested on what no real fm1-base unit sends.
//
// Diagnostics go to stderr only. Exit 0 on !quit or the end of stdin, 2 when app.wasm will not load.
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.includes("-h") || args.includes("--help")) {
  const src = fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n");
  process.stdout.write(src.slice(1, src.findIndex((l) => l.startsWith("import "))).map((l) => l.replace(/^\/\/ ?/, "")).join("\n") + "\n");
  process.exit(0);
}
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : def; };
const wasmPath = opt("--wasm", path.join(here, "..", "build", "emu", "app.wasm"));
const identity = opt("--identity", "FM-1_9400");
const identityHex = opt("--identity-hex", null);
const answer = opt("--answer", "none");
const answerMs = Number(opt("--answer-ms", "500"));
const storePath = opt("--store", null);
const dieAfter = opt("--die-after", null) === null ? 0 : Number(opt("--die-after", "0"));
const log = (m) => process.stderr.write(`sx_bridge: ${m}\n`);

if (!["yes", "no", "none"].includes(answer)) { log(`--answer ${answer}: yes, no or none`); process.exit(2); }
if (!/^[\x20-\x7E]{1,27}$/.test(identity)) { log(`--identity ${identity}: up to 27 printable ASCII characters`); process.exit(2); }
if (identityHex !== null && !/^(?:[0-9A-Fa-f]{2}){0,27}$/.test(identityHex)) { log(`--identity-hex ${identityHex}: up to 27 bytes as hex`); process.exit(2); }
if (!(answerMs >= 0 && answerMs <= 60000)) { log(`--answer-ms ${opt("--answer-ms")}: 0..60000`); process.exit(2); }
if (!(dieAfter >= 0)) { log(`--die-after ${opt("--die-after")}: a count of frames`); process.exit(2); }

const FS = 44100, TICK_MS = 10, CHUNK = 1024, BOOT_MS = 2000, SX_IN_MAX = 640;
const B = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11,
            OCTDN: 12, OCTUP: 13, "OCT-": 12, "OCT+": 13 };
const HS_QUERY = "F000324500000040" + "7FF7";
const STATE_ASKING = 1 << 15;

// ------------------------------------------------------------------------------------------------ the module
let ex, mem, frames = 0;
try {
  const buf = fs.readFileSync(wasmPath);
  const wasi = {          // the worklet's WASI stub; the clock from the frames rendered (deterministic)
    clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(frames / FS * 1e9)), true); return 0; },
    fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
    proc_exit: () => {},
  };
  const stub = new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) });
  const env = new Proxy({}, { get: () => () => 0 });
  const { instance } = await WebAssembly.instantiate(buf, { wasi_snapshot_preview1: stub, env });
  ex = instance.exports;
  mem = ex.memory;
  for (const f of ["web_boot", "web_render", "web_buttons", "web_now"])
    if (typeof ex[f] !== "function") throw new Error(`no ${f}: not an fm1-base emulator build`);
} catch (e) {
  log(`cannot load ${wasmPath}: ${e.message}`);
  process.exit(2);
}
const SX = ["web_cy_sx_in", "web_cy_sx", "web_cy_sx_out", "web_cy_sx_out_len", "web_cy_sx_out_clear"].every((f) => typeof ex[f] === "function");
const STATE = typeof ex.web_cy_state === "function";
if (!SX) log("this app.wasm has no sound transfer (web_cy_sx): an older CYMA-1, which answers the identity query only");

// ------------------------------------------------------------------------------------------------ the store
const b64 = (u8) => Buffer.from(u8).toString("base64");
let storeWrites = 0;
function loadStore() {
  if (!storePath || !fs.existsSync(storePath) || typeof ex.web_store !== "function") return;
  let raw;
  try { raw = JSON.parse(fs.readFileSync(storePath, "utf8")); } catch (e) { log(`--store ${storePath}: not JSON (${e.message}); starting empty`); return; }
  if (raw && typeof raw === "object" && raw["cyma-1-store"] && typeof raw["cyma-1-store"] === "object") raw = raw["cyma-1-store"];
  const max = ex.web_store_max();
  for (const [obj, s] of Object.entries(raw || {})) {
    if (!/^\d+$/.test(obj) || typeof s !== "string") continue;
    const b = Buffer.from(s, "base64").subarray(0, max);
    const p = ex.web_store(+obj);
    if (!p) continue;
    new Uint8Array(mem.buffer).set(b, p);
    ex.web_store_set_len(+obj, b.length);
  }
}
function saveStore() {
  if (!storePath || typeof ex.web_store_writes !== "function") return;
  const w = ex.web_store_writes() >>> 0;
  if (w === storeWrites) return;
  storeWrites = w;
  const out = {};
  const nobj = typeof ex.web_nobj === "function" ? ex.web_nobj() : 3;
  for (let o = 0; o < nobj; o++) {
    const n = ex.web_store_len(o);
    if (n >= 0) out[o] = b64(new Uint8Array(mem.buffer, ex.web_store(o), n));
  }
  const tmp = storePath + ".tmp";
  try {
    fs.writeFileSync(tmp, JSON.stringify(out) + "\n");
    fs.renameSync(tmp, storePath);
  } catch (e) { log(`--store ${storePath}: ${e.message}`); }
}

// ------------------------------------------------------------------------------------------------ framing
const hex = (u8) => Buffer.from(u8).toString("hex").toUpperCase();
function pack7(b) { const o = []; let acc = 0, nb = 0; for (const x of b) { acc |= x << nb; nb += 8; while (nb >= 7) { o.push(acc & 0x7F); acc >>>= 7; nb -= 7; } } if (nb) o.push(acc & 0x7F); return o; }
function identityFrame(text) {      // fm1-base ota.c ota_reply_identity: pack7(00 59 11 len16 00, 27 bytes, ~sum)
  const id = new Uint8Array(27);
  if (identityHex !== null) id.set(Buffer.from(identityHex, "hex").subarray(0, 27));
  else for (let i = 0; i < Math.min(27, text.length); i++) id[i] = text.charCodeAt(i) & 0xFF;
  const sum = id.reduce((s, v) => s + v, 0);
  const msg = [0x00, 0x59, 0x11, 27, 0, 0, ...id, (~sum) & 0xFF];
  return Uint8Array.from([0xF0, ...pack7(msg), 0xF7]);
}
const out = (line) => process.stdout.write(line + "\n");

// ------------------------------------------------------------------------------------------------ the device
let lastState = null, answerAt = null, toSx = 0, quitting = false, ready = false;
const releases = [];                               // [device ms, mask]: buttons to let go of
let held = 0;
const devNow = () => ex.web_now() >>> 0;
const buttons = (m) => { held = m >>> 0; ex.web_buttons(held); };

function drain() {
  if (!SX) return;
  const n = ex.web_cy_sx_out_len() >>> 0;
  if (!n) return;
  const u8 = new Uint8Array(mem.buffer, ex.web_cy_sx_out(), n).slice();
  ex.web_cy_sx_out_clear();
  for (let i = 0; i < n; ) {                       // whole F0..F7 frames, back to back
    if (u8[i] !== 0xF0) { i++; continue; }
    let j = i + 1;
    while (j < n && u8[j] !== 0xF7) j++;
    if (j >= n) { log("a reply frame without its F7 was dropped"); break; }
    out(hex(u8.subarray(i, j + 1)));
    i = j + 1;
  }
}

function press(names, ms) {
  const bits = names.map((n) => B[n]);
  if (!names.length || bits.some((b) => b === undefined)) return false;
  const mods = bits.slice(0, -1).reduce((m, b) => m | (1 << b), 0), last = 1 << bits[bits.length - 1];
  const t = devNow();
  if (mods) {
    buttons(held | mods);
    releases.push([t + 40, -last]);                // (negative: press the last one then)
    releases.push([t + 40 + ms, mods | last]);
  } else {
    buttons(held | last);
    releases.push([t + ms, last]);
  }
  return true;
}

function service() {
  drain();
  const t = devNow();
  for (let i = 0; i < releases.length; ) {
    const [at, m] = releases[i];
    if (t >= at) {
      if (m < 0) buttons(held | -m); else buttons(held & ~m);
      releases.splice(i, 1);
    } else i++;
  }
  if (STATE && ready) {
    const st = ex.web_cy_state() >>> 0;
    if (st !== lastState) {
      const rose = (st & STATE_ASKING) && !(lastState & STATE_ASKING);
      lastState = st;
      out(`#state ${st}`);
      if (rose) answerAt = answer === "none" ? null : t + answerMs;
      if (!(st & STATE_ASKING)) answerAt = null;
    }
    if (answerAt !== null && t >= answerAt) {
      answerAt = null;
      press([answer === "yes" ? "SAVE" : "HOME"], 150);
    }
  }
  saveStore();
}

function render(n) {                               // n device frames, in chunks, servicing after each
  while (n > 0) {
    const k = Math.min(CHUNK, n);
    ex.web_render(k);
    frames += k;
    n -= k;
    service();
  }
}

function frameIn(s) {
  if (!/^(?:[0-9A-F]{2})+$/.test(s) || !s.startsWith("F0") || !s.endsWith("F7") || s.length < 6) { log(`not a frame: ${s.slice(0, 32)}`); return; }
  const u8 = Uint8Array.from(Buffer.from(s, "hex"));
  const inner = u8.subarray(1, u8.length - 1);
  if (inner.length > SX_IN_MAX || inner.some((x) => x >= 0x80)) { log(`dropped (as the FM-1's USB layer does): ${inner.length} B`); return; }
  const isQuery = s === HS_QUERY, ours = inner.length >= 3 && inner[0] === 0x7D && inner[1] === 0x43 && inner[2] === 0x59;
  if (!isQuery) {
    toSx++;
    if (dieAfter > 0 && toSx >= dieAfter) { log(`--die-after ${dieAfter}: gone`); process.exit(3); }
  }
  // the device's order (main_fm1.c): the frame is offered to app_sysex first, and only what it declines reaches the
  // platform's own handlers; here that is the identity reply, which ota_service would send
  let taken = 0;
  if (SX) {
    new Uint8Array(mem.buffer).set(inner, ex.web_cy_sx_in());
    taken = ex.web_cy_sx(inner.length) | 0;
  }
  if (taken && !ours) log(`SAFETY: app_sysex took a frame that is not CYMA-1's (${s.slice(0, 24)}...): on an FM-1 the platform would never see it`);
  if (isQuery && !taken) out(hex(identityFrame(identity)));
  service();
}

function command(line) {
  const [cmd, ...rest] = line.slice(1).trim().split(/\s+/);
  switch (cmd.toLowerCase()) {
    case "press": {
      const names = (rest[0] || "").toUpperCase().split("+").filter(Boolean);
      const ms = rest[1] !== undefined ? Number(rest[1]) : 150;
      if (!Number.isFinite(ms) || ms < 1 || ms > 60000 || !press(names, ms)) log(`!press ${rest.join(" ")}: names from ${Object.keys(B).join(" ")}, joined by +; ms 1..60000`);
      service();
      break;
    }
    case "advance": {
      const ms = Number(rest[0]);
      if (!Number.isFinite(ms) || ms < 0 || ms > 3600000) { log(`!advance ${rest[0]}: 0..3600000 ms`); break; }
      render(Math.round(ms * FS / 1000));
      rebase();
      break;
    }
    case "quit": quit(); break;
    default: log(`unknown command ${line.slice(0, 40)}`);
  }
}

// ------------------------------------------------------------------------------------------------ boot and run
if (typeof ex._initialize === "function") ex._initialize();
loadStore();
if (typeof ex.web_store_writes === "function") storeWrites = ex.web_store_writes() >>> 0;
if (typeof ex.web_master === "function") ex.web_master(3072);
ex.web_boot();
render(Math.round(BOOT_MS * FS / 1000));          // the boot, at full speed
out("#ready");
ready = true;
service();                                         // (the first #state)

let t0 = performance.now(), done = 0;              // real time: frames due since t0
function rebase() { t0 = performance.now(); done = 0; }
const timer = setInterval(() => {
  const due = Math.floor((performance.now() - t0) * FS / 1000);
  let n = due - done;
  if (n > FS) { n = FS; rebase(); }                // (after a stall: catch up a second at most, then real time)
  else done = due;
  if (n > 0) render(n);
}, TICK_MS);

const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
rl.on("line", (line) => {
  if (quitting) return;
  const s = line.trim();
  if (!s) return;
  try {
    if (s[0] === "!") command(s);
    else frameIn(s.toUpperCase());
  } catch (e) {
    log(`error: ${e && e.stack || e}`);
  }
});
rl.on("close", quit);

function quit() {
  if (quitting) return;
  quitting = true;
  clearInterval(timer);
  service();
  saveStore();
  rl.close();
  process.stdin.destroy();                         // (the caller may keep its end open)
  process.exitCode = 0;                            // (stdout drains, then node exits on its own)
  setTimeout(() => process.exit(0), 2000).unref();
}
