// SPDX-License-Identifier: MPL-2.0
// CYMA-1 Sounds T4 (docs/CYMA-1-sounds-spec.md §7, §11.1): the Sounds exports of app.wasm in node, with no browser.
//
//   node tools/sounds_smoke.mjs [--wasm build/emu/app.wasm] [--fuzz 100000] [--json OUT.json]
//
// It loads app.wasm with the worklet's WASI stub (as tools/wasm_smoke.mjs does) and checks:
//   exports   every §7.1 export is there; web_cy_version() is 3
//   params    web_cy_params() JSON-equals tools/cyma1_params.json (the drift check of §3.6); web_cy_factory() is its
//             factory list, byte for byte, and tests/sounds/vectors.json's
//   ops       cur / audition / load / save / put / clear / bank through the box, their rc codes (§7.2) and state bits:
//             save(7) lands in web_store(2) with the cleaned name and the targets; audition sets bits 8 and 13 and
//             writes nothing; clear keeps the sound playing; bank() is exactly one store write and zeroes unused slots
//   sysex     the whole T2 flow through web_cy_sx with web_buttons presses (PUT_SLOT x3, COMMIT, SAVE on the panel ->
//             one write; HOME -> declined; 30 s -> timeout; a retry answered from the cache); HELLO's exact reply;
//             GET_DIR's bank CRC (cached by the firmware since revision 2026-10-24b) after every page write
//   locked    a bank of another format (version 2) in the store: HELLO flags b2, E_LOCKED, rc -4, state bit 14, and
//             the stored bytes never change
//   safety    every platform frame (vectors.json not_ours, the update framing's identity / backup / restore / mirror /
//             app commands, the 7D frames of the screen reader, log, remote, WiFi, LOOM-8) -> web_cy_sx returns 0
//   fuzz      10^5 random frames without our prefix -> 0, no reply; 10^5 random frames with it -> no store write, every
//             reply a well-formed frame of ours <= 306 B, 7-bit inside; then 2x10^4 well-formed frames (valid CRC)
//             with random commands and random records, then audio: no NaN, nothing written
// Prints a line per check and a JSON summary; exit 0 when every check passes, 1 otherwise, 2 when the module will not
// load, 77 when this app.wasm has no Sounds exports yet (web_cy_box). Nothing here touches MIDI or hardware.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const app = path.join(here, '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : def; };
const wasmPath = opt('--wasm', path.join(app, 'build', 'emu', 'app.wasm'));
const FUZZ = Number(opt('--fuzz', '100000'));
const jsonOut = opt('--json', null);
const TABLE_PATH = path.join(app, 'tools', 'cyma1_params.json');
const VEC = JSON.parse(fs.readFileSync(path.join(app, 'tests', 'sounds', 'vectors.json'), 'utf8'));

const checks = [];
const check = (name, pass, detail) => {
  checks.push({ name, pass: !!pass, detail: String(detail) });
  console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${name}: ${detail}`);
  return !!pass;
};

// ---------------------------------------------------------------- the codec (spec §3.4, §3.7, §5.2), for checking
const NAME_SET = new Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.'&+/!?#():");
function cleanName(s) {
  s = String(s).normalize('NFKD').replace(/[̀-ͯ]/g, '');
  let o = '';
  for (let c of s) {
    if (c >= 'a' && c <= 'z') c = c.toUpperCase();
    else if ('_\t\r\n'.includes(c)) c = ' ';
    if (NAME_SET.has(c)) o += c;
  }
  return o.replace(/ +/g, ' ').trim().slice(0, 14).trimEnd() || 'UNTITLED';
}
const CRC_T = new Uint32Array(256).map((_, i) => { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = (u8) => { let c = 0xFFFFFFFF; for (const b of u8) c = CRC_T[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function pack7(b) { const o = []; let acc = 0, nb = 0; for (const x of b) { acc |= x << nb; nb += 8; while (nb >= 7) { o.push(acc & 0x7F); acc >>>= 7; nb -= 7; } } if (nb) o.push(acc & 0x7F); return o; }
function unpack7(s) {
  const m = s.length, n = Math.floor(7 * m / 8);
  if (s.some((x) => x & 0x80) || m !== Math.ceil(8 * n / 7)) return null;
  const o = []; let acc = 0, nb = 0;
  for (const x of s) { acc |= x << nb; nb += 7; if (nb >= 8) { o.push(acc & 0xFF); acc >>>= 8; nb -= 8; } }
  return acc ? null : Uint8Array.from(o);
}
const u32 = (x) => [x & 255, (x >>> 8) & 255, (x >>> 16) & 255, (x >>> 24) & 255];
function sxFrame(cmd, seq, payload = []) {
  const hdr = [1, cmd, seq], body = [...payload, ...u32(crc32(Uint8Array.from([...hdr, ...payload])))];
  return Uint8Array.from([0xF0, 0x7D, 0x43, 0x59, ...hdr, ...pack7(body), 0xF7]);
}
function sxParse(f) {   // -> {cmd, seq, payload} | null
  if (f.length < 13 || f[0] !== 0xF0 || f[f.length - 1] !== 0xF7 || f[1] !== 0x7D || f[2] !== 0x43 || f[3] !== 0x59 || f[4] !== 1) return null;
  const body = unpack7([...f.subarray(7, f.length - 1)]);
  if (!body || body.length < 4 || body.length - 4 > 256) return null;
  const pl = body.subarray(0, body.length - 4);
  const want = body[body.length - 4] | body[body.length - 3] << 8 | body[body.length - 2] << 16 | body[body.length - 1] << 24;
  if ((want >>> 0) !== crc32(Uint8Array.from([f[4], f[5], f[6], ...pl]))) return null;
  return { cmd: f[5], seq: f[6], payload: pl };
}
function recEncode(name, q) {
  const nb = [...cleanName(name)].map((c) => c.charCodeAt(0));
  const out = [1, 1, q.length, nb.length, ...nb];
  for (const v of q) out.push(v & 255, v >>> 8);
  return out;
}
const hex = (u8) => Buffer.from(u8).toString('hex').toUpperCase();
const ascii = (s) => [...s].map((c) => c.charCodeAt(0));
const helloReply = (flags) => [0, 1, 1, 40, 32, 14, 0, 1, flags, 0, 6, ...ascii('CYMA-1'), 3, ...ascii('DEV')];
const ST = { OK: 0, PENDING: 1, E_FRAME: 0x10, E_VERSION: 0x11, E_CMD: 0x12, E_ARG: 0x13, E_PARAM_SET: 0x14, E_BUSY: 0x31,
  E_HOLD: 0x32, E_NOTHING: 0x33, E_ASKING: 0x34, E_LOCKED: 0x35, E_DECLINED: 0x40, E_TIMEOUT: 0x41, E_FLASH: 0x42 };
const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
function upd(cmd, body = []) {   // the updater's framing: F0 pack7(00 59 cmd len24 body ~sum) F7
  let s = 0; for (const b of body) s += b;
  const m = [0, 0x59, cmd, body.length & 255, (body.length >> 8) & 255, 0, ...body, (~s) & 255];
  return [0xF0, ...pack7(m), 0xF7];
}

// ------------------------------------------------------------------------------------------------ the module
const B = { FX: 0, SEL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11, OCTDN: 12, OCTUP: 13 };
let wasmBuf;
try { wasmBuf = fs.readFileSync(wasmPath); } catch (e) { console.log(`sounds_smoke: no ${wasmPath} (${e.message})`); process.exit(2); }

async function instance(storeObjs = {}) {
  let mem, frames = 0;
  const wasi = {
    clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(frames / 44100 * 1e9)), true); return 0; },
    fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
    proc_exit: () => {},
  };
  const stub = new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) });
  const env = new Proxy({}, { get: () => () => 0 });
  const { instance: inst } = await WebAssembly.instantiate(wasmBuf, { wasi_snapshot_preview1: stub, env });
  const ex = inst.exports;
  mem = ex.memory;
  if (ex._initialize) ex._initialize();
  const D = {
    ex,
    get mem() { return mem; },
    nan: 0, peak: 0,
    u8: (ptr, n) => new Uint8Array(mem.buffer, ptr, n).slice(),
    cstr(ptr) { const m = new Uint8Array(mem.buffer); let s = ''; for (let i = ptr; m[i] && s.length < 65536; i++) s += String.fromCharCode(m[i]); return s; },
    render(ms) {
      const blocks = Math.max(1, Math.round(ms * 44.1 / 128));
      for (let b = 0; b < blocks; b++) {
        ex.web_render(128);
        frames += 128;
        const l = new Float32Array(mem.buffer, ex.web_out_l(), 128), r = new Float32Array(mem.buffer, ex.web_out_r(), 128);
        for (let i = 0; i < 128; i++) {
          if (!Number.isFinite(l[i]) || !Number.isFinite(r[i])) D.nan++;
          else D.peak = Math.max(D.peak, Math.abs(l[i]), Math.abs(r[i]));
        }
        D.collect();
      }
    },
    out: [],                     // every reply frame the module has sent, in order
    collect() {
      const n = ex.web_cy_sx_out_len();
      if (!n) return;
      const b = D.u8(ex.web_cy_sx_out(), n);
      ex.web_cy_sx_out_clear();
      let i = 0;
      while (i < b.length) {
        let j = i;
        while (j < b.length && b[j] !== 0xF7) j++;
        D.out.push(b.subarray(i, j + 1));
        i = j + 1;
      }
    },
    sx(frame) {                  // one F0..F7 frame in -> {taken, replies}
      const inner = frame.slice(1, frame.length - 1);
      new Uint8Array(mem.buffer, ex.web_cy_sx_in(), 640).set(inner.slice(0, 640));
      const before = D.out.length;
      const taken = ex.web_cy_sx(inner.length);
      D.collect();
      return { taken, replies: D.out.slice(before) };
    },
    rawsx(inner) {               // the bytes between F0 and F7 as given (any length up to 640)
      new Uint8Array(mem.buffer, ex.web_cy_sx_in(), 640).set(inner.slice(0, 640));
      const before = D.out.length;
      const taken = ex.web_cy_sx(inner.length);
      D.collect();
      return { taken, replies: D.out.slice(before) };
    },
    state: () => ex.web_cy_state() >>> 0,
    setBox(name, q) {
      const b = new Uint8Array(mem.buffer, ex.web_cy_box(), 94);
      b.fill(0);
      const nb = ascii(name).slice(0, 14);
      b.set(nb, 0);
      const dv = new DataView(mem.buffer, ex.web_cy_box(), 94);
      q.forEach((v, i) => dv.setUint16(14 + 2 * i, v, true));
    },
    setBoxBytes(bytes) { new Uint8Array(mem.buffer, ex.web_cy_box(), 3072).set(bytes); },
    box() { return decodePreset(D.u8(ex.web_cy_box(), 94)); },
    cur() { const rc = ex.web_cy_cur(); return { rc, ...D.box() }; },
    bank() { const n = ex.web_store_len(2); return n === 3020 ? decodeBank(D.u8(ex.web_store(2), 3020)) : null; },
    bankBytes() { const n = ex.web_store_len(2); return n > 0 ? D.u8(ex.web_store(2), n) : new Uint8Array(0); },
    writes: () => ex.web_store_writes() >>> 0,
    press(bits, ms = 120) { ex.web_buttons(bits); D.render(ms); ex.web_buttons(0); D.render(60); },
  };
  for (const [obj, bytes] of Object.entries(storeObjs)) {
    new Uint8Array(mem.buffer, ex.web_store(Number(obj)), bytes.length).set(bytes);
    ex.web_store_set_len(Number(obj), bytes.length);
  }
  ex.web_master(4096);
  ex.web_boot();
  return D;
}
function decodePreset(b) {
  let name = '';
  for (let i = 0; i < 14 && b[i]; i++) name += String.fromCharCode(b[i]);
  const dv = new DataView(b.buffer, b.byteOffset, 94), q = [];
  for (let i = 0; i < 40; i++) q.push(dv.getUint16(14 + 2 * i, true));
  return { name, q, raw: b };
}
function decodeBank(b) {
  const dv = new DataView(b.buffer, b.byteOffset, 3020);
  const slots = [];
  for (let s = 0; s < 32; s++) slots.push(decodePreset(b.subarray(12 + 94 * s, 12 + 94 * (s + 1))));
  return { magic: dv.getUint32(0, true), version: dv.getUint16(4, true), rsv: dv.getUint16(6, true), used: dv.getUint32(8, true) >>> 0, slots, raw: b };
}
function makeBank(entries, { magic = 0x31414D43, version = 1, used = null } = {}) {   // entries: {slot: [name, q]}
  const b = new Uint8Array(3020), dv = new DataView(b.buffer);
  let mask = 0;
  dv.setUint32(0, magic, true); dv.setUint16(4, version, true);
  for (const [s, [name, q]] of Object.entries(entries)) {
    const o = 12 + 94 * Number(s);
    b.set(ascii(name).slice(0, 14), o);
    q.forEach((v, i) => dv.setUint16(o + 14 + 2 * i, v, true));
    mask |= 1 << Number(s);
  }
  dv.setUint32(8, (used === null ? mask : used) >>> 0, true);
  return b;
}
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
let rngState = 0x9E3779B9;
const rnd = () => { rngState ^= rngState << 13; rngState >>>= 0; rngState ^= rngState >>> 17; rngState ^= rngState << 5; return (rngState >>>= 0); };
const rndn = (n) => (n ? rnd() % n : 0);

// =============================================================================================== the checks
const D = await instance();
const ex = D.ex;
const NEED = ['web_cy_box', 'web_cy_state', 'web_cy_cur', 'web_cy_audition', 'web_cy_load', 'web_cy_save', 'web_cy_put',
  'web_cy_clear', 'web_cy_bank', 'web_cy_factory', 'web_cy_params', 'web_cy_app_version', 'web_cy_sx_in', 'web_cy_sx',
  'web_cy_sx_out', 'web_cy_sx_out_len', 'web_cy_sx_out_clear', 'web_cy_version'];
const missing = NEED.filter((n) => typeof ex[n] !== 'function');
if (missing.includes('web_cy_box')) {
  console.log(`sounds_smoke: ${wasmPath} has no Sounds exports yet (web_cy_box): SKIP`);
  process.exit(77);
}
check('exports', !missing.length && ex.web_cy_version() === 3,
  missing.length ? `missing ${missing.join(' ')}` : `all ${NEED.length} of §7.1, web_cy_version ${ex.web_cy_version()}`);

// ---- the table and the factory sounds
const fileTable = JSON.parse(fs.readFileSync(TABLE_PATH, 'utf8'));
let wasmTable = null;
try { wasmTable = JSON.parse(D.cstr(ex.web_cy_params())); } catch (e) { wasmTable = null; }
const deepEq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
check('params', wasmTable && deepEq(wasmTable, fileTable),
  wasmTable ? (deepEq(wasmTable, fileTable) ? `web_cy_params() JSON-equals tools/cyma1_params.json (${wasmTable.params.length} params, ${wasmTable.factory.length} factory)`
    : 'web_cy_params() differs from tools/cyma1_params.json: regenerate it (host command params_json)') : 'web_cy_params() is not JSON');
const fac = D.u8(ex.web_cy_factory(), 24 * 94);
let facBad = [];
for (let i = 0; i < 24; i++) {
  const p = decodePreset(fac.subarray(94 * i, 94 * (i + 1)));
  const t = fileTable.factory[i], v = VEC.factory[i];
  if (p.name !== t.name || !same(p.q, t.q16) || p.name !== v.name || !same(p.q, v.q16)) facBad.push(i + 1);
}
check('factory', !facBad.length, facBad.length ? `differs at ${facBad.join(' ')}` : 'web_cy_factory(): 24 x 94 B, equal to the table and to vectors.json');
const appVer = D.cstr(ex.web_cy_app_version());
check('app_version', /^[A-Za-z0-9._-]{1,32}$/.test(appVer), `"${appVer}"`);
const DEF = fileTable.params.map((p) => p.def_q16);

// ---- the ops
D.render(1800);                                                       // past the splash
let c = D.cur();
check('cur', c.rc === 0 && c.name === 'SINGING BOWL' && same(c.q, VEC.factory[0].q16) && (D.state() & 0xFF) === 0,
  `rc ${c.rc}, ${c.name}, state 0x${D.state().toString(16)}: the targets of 01`);
let w0 = D.writes();
D.setBox('smoke save', [0, 0, 0]);                                    // only the name is read
let rc = ex.web_cy_save(7);
D.render(300);
let bank = D.bank();
c = D.cur();
check('save', rc === 0 && bank && (bank.used >>> 7 & 1) && bank.slots[7].name === 'SMOKE SAVE' && same(bank.slots[7].q, c.q) &&
  (D.state() & 0xFF) === 31 && !(D.state() & (1 << 8)) && D.writes() > w0,
  `rc ${rc}; U08 ${bank ? bank.slots[7].name : '-'} (cleaned), used 0x${bank ? bank.used.toString(16) : '-'}, its values the targets: ${bank ? same(bank.slots[7].q, c.q) : '-'}; loaded ${D.state() & 0xFF}`);
const t5 = VEC.factory[4];
w0 = D.writes();
D.setBox('audition', t5.q16);
rc = ex.web_cy_audition(1);
let st = D.state();
D.render(400);
c = D.cur();
check('audition', rc === 0 && (st & (1 << 8)) && (st & (1 << 13)) && (st & 0xFF) === 31 && c.name === 'AUDITION' && same(c.q, t5.q16) && D.writes() === w0,
  `rc ${rc}, state 0x${st.toString(16)} (bit 8 edited ${st >> 8 & 1}, bit 13 auditioned ${st >> 13 & 1}), still U08 loaded; playing ${c.name}; ${D.writes() - w0} writes`);
rc = ex.web_cy_load(31, 0);
D.render(100);
const before = D.cur();
rc = [rc, ex.web_cy_clear(7)];
D.render(200);
c = D.cur();
bank = D.bank();
check('clear', rc[0] === 0 && rc[1] === 0 && c.rc === 31 && same(c.q, before.q) && bank && !(bank.used >>> 7 & 1),
  `load rc ${rc[0]}, clear rc ${rc[1]}; U08 gone from the bank (used 0x${bank ? bank.used.toString(16) : '-'}), still loaded (${c.rc}) and the same sound`);
// put: a rename / a factory copy (the name cleaned in place)
D.setBox('put test', VEC.factory[2].q16);
rc = ex.web_cy_put(20);
D.render(50);
bank = D.bank();
check('put', rc === 0 && bank.slots[20].name === 'PUT TEST' && same(bank.slots[20].q, VEC.factory[2].q16) && (bank.used >>> 20 & 1) &&
  ex.web_cy_put(32) === -1 && ex.web_cy_put(-1) === -1,
  `rc ${rc}; U21 ${bank.slots[20].name}; put(32) and put(-1) are -1`);
// bank: one write; unused slots zeroed; names cleaned
const dirty = makeBank({ 0: ['one', VEC.factory[0].q16], 3: ['FOUR', VEC.factory[3].q16], 31: ['Last one!', VEC.factory[23].q16] });
dirty.set([0x41, 0x42, 0x43, 1, 2, 3], 12 + 94 * 10);                 // junk in a slot whose used bit is 0
D.setBoxBytes(dirty);
w0 = D.writes();
rc = ex.web_cy_bank();
const w1 = D.writes();
D.render(50);
bank = D.bank();
check('bank', rc === 0 && w1 - w0 === 1 && bank.used === ((1 | 8 | 0x80000000) >>> 0) && bank.slots[0].name === 'ONE' &&
  bank.slots[31].name === 'LAST ONE!' && bank.slots[10].raw.every((x) => x === 0) && bank.version === 1 && bank.magic === 0x31414D43,
  `rc ${rc}, ${w1 - w0} store write(s) for the call; used 0x${bank.used.toString(16)}; U01 ${bank.slots[0].name}, U32 ${bank.slots[31].name}; U11 zeroed ${bank.slots[10].raw.every((x) => x === 0)}`);
D.setBoxBytes(makeBank({ 0: ['X', DEF] }, { magic: 0x12345678 }));
const rcMagic = ex.web_cy_bank();
D.setBoxBytes(makeBank({ 0: ['X', DEF] }, { version: 2 }));
const rcVer = ex.web_cy_bank();
check('bank_refuses', rcMagic === -1 && rcVer === -1 && D.bank().used === bank.used, `bad magic rc ${rcMagic}, version 2 rc ${rcVer}; the bank unchanged`);
// rc codes
const rcs = { 'save(32)': ex.web_cy_save(32), 'save(-1)': ex.web_cy_save(-1), 'load(56)': ex.web_cy_load(56, 0), 'load(-1)': ex.web_cy_load(-1, 0),
  'load(empty U10)': ex.web_cy_load(24 + 9, 0), 'clear(32)': ex.web_cy_clear(32) };
check('rc', Object.values(rcs).every((v) => v === -1), Object.entries(rcs).map(([k, v]) => `${k} ${v}`).join(', '));
rc = ex.web_cy_load(0, 1);
st = D.state();
D.render(400);
check('load', rc === 0 && (st & 0xFF) === 0 && (st & (1 << 12)) && !(D.state() & (1 << 12)) && !(D.state() & (1 << 13)),
  `load(0, glide) rc ${rc}, state 0x${st.toString(16)} (gliding bit 12), then 0x${D.state().toString(16)}`);
// COMPARE held: -3, state bit 9; the SAVE dialog: bit 11
ex.web_buttons(1 << B.SEL); D.render(30); ex.web_buttons((1 << B.SEL) | (1 << B.HOME)); D.render(60);
st = D.state();
D.setBox('BUSY', VEC.factory[1].q16);
const rcCmp = [ex.web_cy_audition(0), ex.web_cy_save(5)];
ex.web_buttons(0); D.render(100);
check('compare', (st & (1 << 9)) && rcCmp[0] === -3 && rcCmp[1] === -3, `state 0x${st.toString(16)} (bit 9), audition rc ${rcCmp[0]}, save rc ${rcCmp[1]}`);
D.press(1 << B.SAVE);
st = D.state();
D.press(1 << B.HOME);
check('save_dialog', (st & (1 << 11)) && !(D.state() & (1 << 11)), `SAVE: state 0x${st.toString(16)} (bit 11); HOME closes it`);

// ---- SysEx through web_cy_sx (the T2 flow, with the panel's buttons)
let seq = 0;
const next = () => (seq = (seq + 1) & 0x7F);
const send = (cmd, pl = []) => { const f = sxFrame(cmd, next(), pl); return { f, ...D.sx(f) }; };
const status = (r) => { const p = r.replies.length ? sxParse(r.replies[r.replies.length - 1]) : null; return p ? p.payload[0] : -1; };
let r = send(0x01, [1, 3]);
check('hello', r.taken === 1 && r.replies.length === 1 && hex(r.replies[0]) === hex(sxFrame(0x41, seq, helloReply(0x03))),
  `taken ${r.taken}, reply ${r.replies.length ? hex(r.replies[0]).slice(0, 40) + '...' : 'none'} (flags 0x03: confirm, emulator)`);
const trio = [[4, 'MARIMBA', VEC.factory[3].q16], [5, 'VIBES', VEC.factory[4].q16], [6, 'lower case 07', VEC.factory[5].q16]];
const bankBefore = D.bank();
w0 = D.writes();
for (const [s, n, q] of trio) r = send(0x05, [s, ...recEncode(n, q)]);
const stagedOk = status(r) === 0;
r = send(0x08, [0]);
const pend = status(r);
st = D.state();
const retry = D.sx(r.f);                                              // the same frame again: the cached PENDING
D.render(100);
const noWrite = D.writes() === w0;
D.press(1 << B.HOME);
let async1 = D.out.length ? sxParse(D.out[D.out.length - 1]) : null;
check('sx_decline', stagedOk && pend === ST.PENDING && (st & (1 << 15)) && retry.replies.length === 1 && sxParse(retry.replies[0]).payload[0] === ST.PENDING &&
  noWrite && async1 && async1.cmd === 0x48 && async1.payload[0] === ST.E_DECLINED && D.writes() === w0 && D.bank().used === bankBefore.used,
  `staged, COMMIT ${pend} (state bit 15 ${st >> 15 & 1}), a retry -> the cached ${retry.replies.length ? sxParse(retry.replies[0]).payload[0] : '-'}; HOME -> 0x48 status ${async1 ? async1.payload[0] : '-'}; ${D.writes() - w0} writes`);
for (const [s, n, q] of trio) r = send(0x05, [s, ...recEncode(n, q)]);
r = send(0x08, [0]);
const commitSeq = seq;
D.render(50);
w0 = D.writes();
D.press(1 << B.SAVE);
D.render(300);
async1 = D.out.length ? sxParse(D.out[D.out.length - 1]) : null;
bank = D.bank();
const mask = (bankBefore.used | 0x70) >>> 0;
check('sx_commit', async1 && async1.cmd === 0x48 && async1.seq === commitSeq && async1.payload[0] === 0 && same([...async1.payload.subarray(1, 5)], u32(mask)) &&
  bank.used === mask && bank.slots[6].name === 'LOWER CASE 07' && same(bank.slots[4].q, trio[0][2]) && D.writes() - w0 >= 1 && !(D.state() & (1 << 15)),
  `SAVE on the panel -> 0x48 OK (seq ${async1 ? async1.seq : '-'}), used 0x${bank.used.toString(16)}; U07 ${bank.slots[6].name}; ${D.writes() - w0} store write(s)`);
r = send(0x04, [6]);
check('sx_get_slot', status(r) === 0 && hex(r.replies[0]) === hex(sxFrame(0x44, seq, [0, 6, 1, ...recEncode('LOWER CASE 07', trio[2][2])])),
  'GET_SLOT U07 reads back the record byte for byte');
r = send(0x05, [7, ...recEncode('NOBODY', DEF)]);
r = send(0x08, [0]);
w0 = D.writes();
D.render(31000);
async1 = D.out.length ? sxParse(D.out[D.out.length - 1]) : null;
check('sx_timeout', async1 && async1.cmd === 0x48 && async1.payload[0] === ST.E_TIMEOUT && !(D.state() & (1 << 15)) && D.bank().used === mask,
  `31 s with no answer -> 0x48 status ${async1 ? async1.payload[0] : '-'}; nothing written (the settings may autosave: ${D.writes() - w0} writes)`);
r = send(0x02);
const ge = r.replies.length ? sxParse(r.replies[0]) : null;
c = D.cur();
check('sx_get_edit', ge && ge.payload[0] === 0 && hex(ge.payload.subarray(3)) === hex(recEncode(c.name, c.q)) && ge.payload[1] === (D.state() & 0xFF),
  `GET_EDIT = web_cy_cur(): ${c.name}, preset ${ge ? ge.payload[1] : '-'}`);
r = send(0x03, [1, ...recEncode('from usb', VEC.factory[6].q16)]);
st = D.state();
D.render(400);
c = D.cur();
check('sx_put_edit', status(r) === 0 && (st & (1 << 13)) && c.name === 'FROM USB' && same(c.q, VEC.factory[6].q16) && D.bank().used === mask,
  `PUT_EDIT plays ${c.name} (auditioned ${st >> 13 & 1}), nothing written`);
// GET_DIR's bank CRC (the hardening round's firmware keeps it until the bank changes): after every one of the page's
// bank writes (save, put, clear, bank) it is the CRC of the bank as stored, never the one GET_DIR saw before
{
  const dirCrc = () => { const p = sxParse(send(0x07).replies.slice(-1)[0] || []); return p && p.payload[0] === 0 ? (p.payload[13] | p.payload[14] << 8 | p.payload[15] << 16 | p.payload[16] << 24) >>> 0 : -1; };
  const steps = [], bad = [];
  const step = (what, fn) => {
    dirCrc();                                                         // (GET_DIR before: a cached CRC exists)
    const rcx = fn();
    D.render(50);
    const got = dirCrc(), want = crc32(D.bankBytes());
    steps.push(`${what} rc ${rcx}`);
    if (got !== want) bad.push(`${what}: GET_DIR CRC ${got.toString(16)}, the stored bank's ${want.toString(16)}`);
  };
  step('save(9)', () => { D.setBox('crc save', [0]); return ex.web_cy_save(9); });
  step('put(10)', () => { D.setBox('crc put', VEC.factory[8].q16); return ex.web_cy_put(10); });
  step('clear(9)', () => ex.web_cy_clear(9));
  step('bank', () => { D.setBoxBytes(makeBank({ 2: ['CRC BANK', VEC.factory[9].q16], 30: ['CRC TWO', VEC.factory[10].q16] })); return ex.web_cy_bank(); });
  step('bank again (other bytes, same used mask)', () => { D.setBoxBytes(makeBank({ 2: ['CRC BANK', VEC.factory[11].q16], 30: ['CRC TWO', VEC.factory[10].q16] })); return ex.web_cy_bank(); });
  check('sx_dir_crc', bad.length === 0, bad.length ? bad.join('; ') : `GET_DIR's CRC is the stored bank's after ${steps.join(', ')}`);
}

// ---- the platform's frames: never taken
const platform = [
  ...Object.entries(VEC.not_ours).map(([k, h]) => [k, [0xF0, ...Buffer.from(h, 'hex'), 0xF7]]),
  ['identity (cmd 0x11)', HS_QUERY], ['backup BU_REQ 0x42', upd(0x42, u32(0x314D5943))], ['restore RS_REQ 0x41', upd(0x41, [...u32(3100), ...u32(0x12345678)])],
  ['BU_END 0x43', upd(0x43)], ['mirror MR_REQ 0x50', upd(0x50, [1])], ['mirror MR_REMOTE 0x54', upd(0x54, [1, 0, 0, 0, 0, 0])],
  ['update read 0x30', upd(0x30, [...Array(16).keys()])], ['APP_USB_CMD 0x60', upd(0x60, ascii('CYMA'))],
  ['UBOOT key body', [0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]], ['OTA key body', [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7]],
  ['screen reader HELLO', [0xF0, 0x7D, 0x53, 0x52, 1, 1, 0xF7]], ['log', [0xF0, 0x7D, 0x4C, 0x47, 1, 0xF7]],
  ['remote buttons', [0xF0, 0x7D, 0x4C, 0x43, 1, 0x7F, 1, 0xF7]], ['wifi', [0xF0, 0x7D, 0x4C, 0x57, 1, 0xF7]],
  ['LOOM-8', [0xF0, 0x7D, 0x46, 0x4C, 1, 0xF7]], ['7D 43 58', [0xF0, 0x7D, 0x43, 0x58, 1, 1, 1, 0xF7]],
];
const taken = platform.filter(([, f]) => { const x = D.sx(Uint8Array.from(f)); return x.taken !== 0 || x.replies.length; }).map(([k]) => k);
check('platform_frames', !taken.length, taken.length ? `taken or answered: ${taken.join(', ')}` : `${platform.length} platform frames: web_cy_sx returned 0 for every one, no reply`);
// our damaged frames: a NAK, nothing changes; reply-range frames: claimed, never answered
const good = sxFrame(0x05, 0x40, [8, ...recEncode('NAK ME', DEF)]);
const bad = Uint8Array.from(good); bad[bad.length - 3] ^= 1;
r = D.sx(bad);
const nak = r.replies.length ? sxParse(r.replies[0]) : null;
const v2 = Uint8Array.from(good); v2[4] = 2;
const r2 = D.sx(v2);
const nak2 = r2.replies.length ? sxParse(r2.replies[0]) : null;
const echo = [0x41, 0x45, 0x48, 0x7F].map((cmd) => D.sx(sxFrame(cmd, 9, [0, 1, 2])));
check('nak_and_echo', r.taken === 1 && nak && nak.cmd === 0x7F && same([...nak.payload], [ST.E_FRAME, 0x05, 1]) && nak2 && same([...nak2.payload], [ST.E_VERSION, 0x05, 1]) &&
  echo.every((x) => x.taken === 1 && !x.replies.length),
  `bad CRC -> NAK ${nak ? [...nak.payload].join(',') : '-'}; VER 2 -> NAK ${nak2 ? [...nak2.payload].join(',') : '-'}; 0x41/0x45/0x48/0x7F claimed, ${echo.reduce((s, x) => s + x.replies.length, 0)} replies`);
// the largest frame (306 B, 256 B payload): a well-formed refusal; 257 B: a NAK
const big = sxFrame(0x05, 0x41, [0, 1, 1, 40, 0, ...Array(251).fill(0)]);   // layout, param_set, N, L right; 251 B is no record's length
r = D.sx(big);
const bigR = r.replies.length ? sxParse(r.replies[0]) : null;
const over = (() => { const hdr = [1, 0x05, 0x42], pl = Array(257).fill(0); const body = [...pl, ...u32(crc32(Uint8Array.from([...hdr, ...pl])))]; return Uint8Array.from([0xF0, 0x7D, 0x43, 0x59, ...hdr, ...pack7(body), 0xF7]); })();
const rOver = D.sx(over);
const overR = rOver.replies.length ? sxParse(rOver.replies[0]) : null;
const full = D.rawsx(Uint8Array.from([0x7D, 0x43, 0x59, 1, 5, 0x43, ...Array(634).fill(0x11)]));
check('sizes', big.length === 306 && bigR && bigR.payload[0] === ST.E_ARG && overR && overR.cmd === 0x7F && overR.payload[0] === ST.E_FRAME && full.taken === 1,
  `306 B frame -> status ${bigR ? bigR.payload[0] : '-'} (E_ARG: no record that long); ${over.length} B (257 B payload) -> NAK ${overR ? overR.payload[0] : '-'}; 640 B between F0/F7 handled`);

// ---- fuzz
const usedNow = D.bank().used;
w0 = D.writes();
let fz = { not: 0, notBad: 0, ours: 0, replies: 0, badReplies: 0, maxReply: 0 };
const checkReply = (f) => { fz.replies++; fz.maxReply = Math.max(fz.maxReply, f.length); if (f.length > 306 || !sxParse(f) || f.subarray(1, f.length - 1).some((x) => x & 0x80)) fz.badReplies++; };
for (let i = 0; i < FUZZ; i++) {
  const n = rndn(641), inner = new Uint8Array(n);
  for (let k = 0; k < n; k++) inner[k] = rndn(8) ? rnd() & 0x7F : rnd() & 0xFF;
  if (n >= 3 && inner[0] === 0x7D && inner[1] === 0x43 && inner[2] === 0x59) inner[2] = 0x58;
  const x = D.rawsx(inner);
  fz.not++;
  if (x.taken !== 0 || x.replies.length) fz.notBad++;
}
for (let i = 0; i < FUZZ; i++) {
  const n = 3 + rndn(320), inner = new Uint8Array(n);
  inner.set([0x7D, 0x43, 0x59]);
  for (let k = 3; k < n; k++) inner[k] = rndn(16) ? rnd() & 0x7F : rnd() & 0xFF;
  if (n > 3 && rndn(2)) inner[3] = 1;
  const x = D.rawsx(inner);
  fz.ours += x.taken === 1;
  x.replies.forEach(checkReply);
}
// well-formed frames: random commands, slots and records (any u16 is a valid value: §3.1)
let wf = 0;
for (let i = 0; i < FUZZ / 5; i++) {
  const cmd = rndn(4) ? [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09][rndn(9)] : rndn(0x40);
  let pl;
  if (cmd === 0x03 || cmd === 0x05) {
    const N = rndn(3) ? 40 : 1 + rndn(64), L = rndn(33), rec = [1, 1, N, L];
    for (let k = 0; k < L; k++) rec.push(rndn(256));
    for (let k = 0; k < N; k++) { const v = rnd() & 0xFFFF; rec.push(v & 255, v >> 8); }
    if (!rndn(10)) rec.pop();
    pl = [cmd === 0x03 ? rndn(4) : rndn(36), ...rec];
  } else {
    pl = Array.from({ length: rndn(3) ? [0, 2, 0, 0, 1, 0, 1, 0, 1, 0][cmd] ?? rndn(4) : rndn(12) }, () => rndn(256));
  }
  const x = D.sx(sxFrame(cmd, rndn(128), pl));
  wf += x.taken === 1;
  x.replies.forEach(checkReply);
}
const fuzzWrites = D.writes() - w0;
D.sx(sxFrame(0x09, 0x7E));                                            // ABORT whatever is staged
ex.web_key(7, 110, 0); D.render(300); ex.web_key(7, 0, 0); D.render(700);   // audio after the random PUT_EDITs
check('fuzz', fz.notBad === 0 && fz.ours === FUZZ && wf === FUZZ / 5 && fz.badReplies === 0 && fuzzWrites === 0 && D.nan === 0 && D.bank().used === usedNow,
  `${fz.not} without our prefix: ${fz.notBad} taken; ${FUZZ} with it and ${FUZZ / 5} well formed: all taken, ${fz.replies} replies (${fz.badReplies} malformed, longest ${fz.maxReply} B); ${fuzzWrites} store writes; ${D.nan} non-finite samples after; peak ${D.peak.toFixed(3)}`);

// ---- a bank of another format: locked, never written
const foreign = makeBank({ 0: ['NEWER FORMAT', DEF], 1: ['KEEP ME', VEC.factory[1].q16] }, { version: 2 });
const L = await instance({ 2: foreign });
L.render(1800);
r = L.sx(sxFrame(0x01, 1, [1, 3]));
const lh = r.replies.length ? sxParse(r.replies[0]) : null;
const lget = L.sx(sxFrame(0x04, 2, [1]));
L.sx(sxFrame(0x05, 3, [2, ...recEncode('INTRUDER', DEF)]));
const lcom = L.sx(sxFrame(0x08, 4, [0]));
L.setBox('INTRUDER', DEF);
const lrc = { save: L.ex.web_cy_save(0), put: L.ex.web_cy_put(3), clear: L.ex.web_cy_clear(1) };
L.setBoxBytes(makeBank({ 0: ['INTRUDER', DEF] }));
lrc.bank = L.ex.web_cy_bank();
L.render(500);
const lst = L.state();
const unchanged = same([...L.bankBytes()], [...foreign]);
const lgetS = lget.replies.length ? sxParse(lget.replies[0]).payload[0] : -1, lcomS = lcom.replies.length ? sxParse(lcom.replies[0]).payload[0] : -1;
check('locked', lh && (lh.payload[8] & 4) && lgetS === ST.E_LOCKED && lcomS === ST.E_LOCKED && (lst & (1 << 14)) &&
  lrc.save === -4 && lrc.put === -4 && lrc.clear === -4 && lrc.bank === -4 && unchanged,
  `HELLO flags 0x${lh ? lh.payload[8].toString(16) : '-'}; GET_SLOT ${lgetS}, COMMIT ${lcomS} (E_LOCKED 53); save ${lrc.save}, put ${lrc.put}, clear ${lrc.clear}, bank ${lrc.bank} (-4); state bit 14 ${lst >> 14 & 1}; the stored bank byte-identical: ${unchanged}`);

const pass = checks.every((x) => x.pass);
const summary = { pass, checks: checks.length, failed: checks.filter((x) => !x.pass).map((x) => x.name), fuzz: FUZZ, wasm: wasmPath,
  label: 'algorithmic, host-equivalent wasm build, not device-measured' };
console.log(JSON.stringify(summary));
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify({ ...summary, detail: checks }, null, 1));
console.log(`sounds_smoke: ${checks.filter((x) => x.pass).length}/${checks.length} checks pass`);
process.exit(pass ? 0 : 1);
