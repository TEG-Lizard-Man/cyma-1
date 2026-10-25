// SPDX-License-Identifier: MPL-2.0
// CYMA-1 browser smoke in node (spec §8.5): app.wasm, the page's whole firmware, run without a browser.
//
//   node tools/wasm_smoke.mjs [--wasm build/emu/app.wasm] [--host-hash 0xXXXXXXXX] [--json OUT.json] [--seconds 5]
//
// It instantiates the module with the same WASI stub the worklet gives it (web/emu/worklet.js), boots it, and plays
// 5 s of scripted input through the exports the page uses (web_key with velocity and pressure, web_enc, web_buttons),
// pulling audio 128 frames at a time as the AudioWorklet would. Checks:
//   exports   the fm1-base contract and CYMA-1's web_cy_* (spec §7.5) are all there
//   audio     RMS of the played part > -40 dBFS; no NaN or Inf anywhere; the peak never past full scale
//   screen    the framebuffer is not all black, and it changes while playing
//   leds      web_cy_leds (42 levels) move: the played key lights, the page buttons follow
//   info      web_cy_info is JSON of <= 300 bytes with the fields the page reads
//   hash      web_cy_demo_hash(1) equals the host's (--host-hash; tools/golden.py passes it)
// Prints a line per check and a JSON summary (also to --json); exit 0 when every check passes, 1 otherwise, 2 when
// the module will not load. Algorithmic, host-equivalent build: nothing here is measured on an FM-1.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : def; };
const wasmPath = opt('--wasm', path.join(here, '..', 'build', 'emu', 'app.wasm'));
const hostHash = opt('--host-hash', null);
const jsonOut = opt('--json', null);
const seconds = Number(opt('--seconds', '5'));

const checks = [];
const check = (name, pass, detail) => { checks.push({ name, pass: !!pass, detail: String(detail) }); console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${name}: ${detail}`); };
const hex = (v) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

let ex, mem, frames = 0;
try {
  const buf = fs.readFileSync(wasmPath);
  // the worklet's WASI stub: the device's clock from audio time (here: the frames rendered), writes dropped
  const wasi = {
    clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(frames / 44100 * 1e9)), true); return 0; },
    fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
    proc_exit: () => {},
  };
  const stub = new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) });
  const env = new Proxy({}, { get: () => () => 0 });
  const { instance } = await WebAssembly.instantiate(buf, { wasi_snapshot_preview1: stub, env });
  ex = instance.exports;
  mem = ex.memory;
} catch (e) {
  console.log(`wasm_smoke: cannot load ${wasmPath}: ${e.message}`);
  if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify({ pass: false, error: e.message, checks: [] }));
  process.exit(2);
}

const need = ['web_boot', 'web_render', 'web_out_l', 'web_out_r', 'web_key', 'web_keys', 'web_enc', 'web_buttons',
  'web_master', 'web_fb', 'web_blits', 'web_midi_in', 'web_lit_buttons', 'web_store_writes',
  'web_cy_leds', 'web_cy_press', 'web_cy_info', 'web_cy_info_seq', 'web_cy_demo_hash', 'web_cy_version'];
const missing = need.filter((n) => typeof ex[n] !== 'function');
check('exports', missing.length === 0, missing.length ? `missing ${missing.join(' ')}` : `${need.length} present, web_cy_version ${ex.web_cy_version ? ex.web_cy_version() : '?'}`);
if (missing.includes('web_boot') || missing.includes('web_render')) {
  if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify({ pass: false, checks }));
  process.exit(1);
}

if (ex._initialize) ex._initialize();
ex.web_master(4096);
ex.web_boot();

const ENC = { SELECT: 0, ALGO: 1, PRESET: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const B = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11 };
const u8 = (ptr, n) => new Uint8Array(mem.buffer, ptr, n).slice();
const leds = () => Array.from(u8(ex.web_cy_leds(), 42));
const cstr = (ptr) => { const m = new Uint8Array(mem.buffer); let s = ''; for (let i = ptr; m[i] && s.length < 4096; i++) s += String.fromCharCode(m[i]); return s; };
const fbStats = () => {
  const fb = new Uint16Array(mem.buffer, ex.web_fb(), 240 * 240);
  let nz = 0; const colours = new Set();
  for (let i = 0; i < fb.length; i++) { if (fb[i]) nz++; if (colours.size < 4096) colours.add(fb[i]); }
  let h = 0x811C9DC5; for (let i = 0; i < fb.length; i += 7) h = Math.imul(h ^ fb[i], 0x01000193) >>> 0;
  return { nz, colours: colours.size, sig: h };
};

// the script: [time s, action], played as someone would: something always sounding. The splash takes the first
// 1.6 s; the tour, if it starts, is simply played through
const SCRIPT = [
  [1.8, () => ex.web_key(7, 120, 0)],                         // C4, loud
  [2.0, 'leds_key'],
  [2.2, () => ex.web_key(7, 120, 900)],                       // lean in
  [2.6, () => { ex.web_key(11, 115, 0); ex.web_key(14, 110, 0); }],   // E4 G4 over it
  [3.0, () => ex.web_enc(ENC.SELECT, 4)],                     // the shape melts
  [3.2, () => ex.web_buttons(1 << B.ENV)],                    // page EXCITE
  [3.3, () => ex.web_buttons(0)],
  [3.4, 'leds_page'],
  [3.5, () => ex.web_enc(ENC.K2, 5)],                         // POS X
  [3.6, () => { ex.web_key(7, 0, 0); ex.web_key(11, 0, 0); ex.web_key(14, 0, 0); ex.web_key(9, 127, 0); ex.web_key(12, 120, 0); }],
  [3.9, () => ex.web_enc(ENC.ALGO, 1)],                       // BOW
  [4.0, () => { ex.web_key(9, 0, 0); ex.web_key(12, 0, 0); }],
  [4.1, () => { ex.web_key(9, 110, 600); ex.web_key(16, 110, 600); }],   // bowed, leaned on
  [4.6, () => { ex.web_key(9, 110, 1000); ex.web_key(16, 110, 1000); }],
  [5.3, () => { ex.web_key(9, 0, 0); ex.web_key(16, 0, 0); ex.web_key(12, 120, 800); }],
  [5.4, () => ex.web_midi_in(0, 0x09 | 0x90 << 8 | 60 << 16 | 120 << 24)],   // a note over Web MIDI
  [6.0, () => { ex.web_midi_in(0, 0x08 | 0x80 << 8 | 60 << 16); ex.web_key(12, 0, 0); }],
  [6.1, () => ex.web_enc(ENC.ALGO, -1)],                      // MALLET again
  [6.2, () => ex.web_enc(ENC.PRESET, 1)],                     // 02 HANDPAN
  [6.6, () => { ex.web_key(9, 120, 0); ex.web_key(16, 110, 0); }],
  [6.8, () => { ex.web_key(9, 0, 0); ex.web_key(16, 0, 0); ex.web_key(12, 127, 0); }],
];
const start = 1.6, end = start + seconds + 0.4;
let si = 0, t = 0, n = 0, nan = 0, peak = 0, sum2 = 0, cnt = 0;
const snaps = { boot: leds() }, fbs = [];
const BLOCK = 128;
while (t < end) {
  while (si < SCRIPT.length && SCRIPT[si][0] <= t) {
    const a = SCRIPT[si][1];
    if (a === 'leds_key') snaps.key = leds();
    else if (a === 'leds_page') snaps.page = leds();
    else a();
    si++;
  }
  ex.web_render(BLOCK);
  frames += BLOCK;
  const l = new Float32Array(mem.buffer, ex.web_out_l(), BLOCK), r = new Float32Array(mem.buffer, ex.web_out_r(), BLOCK);
  for (let i = 0; i < BLOCK; i++) {
    const a = l[i], b = r[i];
    if (!Number.isFinite(a) || !Number.isFinite(b)) { nan++; continue; }
    const m = Math.max(Math.abs(a), Math.abs(b));
    if (m > peak) peak = m;
    if (t >= start) { sum2 += a * a + b * b; cnt += 2; }
  }
  n += BLOCK; t = n / 44100;
  if (fbs.length < 64 && Math.abs(t * 4 - Math.round(t * 4)) < BLOCK / 44100 * 2) fbs.push(fbStats());
}
snaps.end = leds();

const rms = cnt ? Math.sqrt(sum2 / cnt) : 0;
const rmsDb = rms > 0 ? 20 * Math.log10(rms) : -300;
check('audio', rmsDb > -40 && nan === 0 && peak <= 1.0, `RMS ${rmsDb.toFixed(1)} dBFS over ${seconds} s of play, peak ${(peak > 0 ? 20 * Math.log10(peak) : -300).toFixed(2)} dBFS, ${nan} non-finite samples`);
const last = fbStats();
const sigs = new Set(fbs.map((f) => f.sig));
check('screen', last.nz > 0 && sigs.size > 1, `${last.nz} of 57600 pixels lit, ${last.colours} colours, ${sigs.size} distinct frames of ${fbs.length} sampled, ${ex.web_blits()} blits`);
const diff = (a, b) => a && b ? a.reduce((s, v, i) => s + (v !== b[i] ? 1 : 0), 0) : 0;
const keyLit = snaps.key ? snaps.key[7] : 0;
const pageOk = snaps.page ? snaps.page[27 + 2] === 3 : false;   // ENV (B_ENV = 2) bright on its page
check('leds', diff(snaps.boot, snaps.key) > 0 && keyLit >= 2 && pageOk,
  `key 7 at ${keyLit} while held (want >= 2); ENV at ${snaps.page ? snaps.page[29] : '?'} on its page (want 3); ${diff(snaps.boot, snaps.end)} of 42 changed boot -> end`);
let info = null, infoText = '';
try { infoText = cstr(ex.web_cy_info()); info = JSON.parse(infoText); } catch (e) { info = null; }
const fields = ['preset', 'num', 'page', 'exciter', 'shape', 'keys', 'view', 'octave', 'frozen', 'latched', 'voices', 'est', 'say'];
const lacking = info ? fields.filter((f) => !(f in info)) : fields;
check('info', info && infoText.length <= 300 && lacking.length === 0, info ? `${infoText.length} B` + (lacking.length ? `, lacking ${lacking.join(' ')}` : `: ${info.num} ${info.preset} · ${info.exciter} · ${info.shape} · ${info.keys}`) : `not JSON: ${infoText.slice(0, 80)}`);
const h = hex(ex.web_cy_demo_hash(1));
const h2 = hex(ex.web_cy_demo_hash(1));
check('hash', h === h2 && (!hostHash || h.toUpperCase() === hostHash.toUpperCase()),
  `wasm ${h}${h !== h2 ? ` then ${h2}` : ''}` + (hostHash ? `, host ${hostHash}` : ' (no host hash given)'));

const pass = checks.every((c) => c.pass);
const summary = { pass, demo_hash: h, rms_dbfs: Number(rmsDb.toFixed(2)), peak_dbfs: Number((peak > 0 ? 20 * Math.log10(peak) : -300).toFixed(3)), nan, checks, wasm: wasmPath, label: 'algorithmic, host-equivalent wasm build, not device-measured' };
console.log(JSON.stringify(summary));
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(summary, null, 1));
process.exit(pass ? 0 : 1);
