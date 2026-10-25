// SPDX-License-Identifier: MPL-2.0
// CYMA-1's AudioWorklet (spec §7.4), replacing fm1-base's web/emu/worklet.js. The export contract is fm1-base's
// (web/emu/emu_web.c: app.wasm is the whole FM-1 app on a simulated device; web_render(n) runs the device's clock
// until n frames exist) plus CYMA-1's own (web.c: LED levels, key pressure, the info line, the demo hash).
//
// What it adds to the base worklet, and why:
//  - frames to the page at 60 Hz of currentTime (an accumulating deadline, so 5- and 6-quantum gaps average to
//    exactly 1/60 s), with the 42 LED levels and 27 pressures every frame (they are tiny) and the screen only when
//    the firmware blitted something that differs from the last screen sent. While the page is hidden (it says so)
//    frames go at 4 Hz without the screen, and the next visible frame carries the screen again;
//  - MIDI out drained after every quantum, stamped with the context time it was made at (the page's Web MIDI out
//    and its capture need the time, and a 60 Hz drain alone would quantise it to 17 ms);
//  - MIDI in that may carry a context time (`at`): "Play me something" is scheduled ahead, and applying each packet
//    at the quantum it is due keeps the groove on a 2.9 ms grid instead of the page's timer jitter. Every packet
//    that reaches the firmware goes back to the page stamped the same way as MIDI out, so a capture's .mid holds
//    what MIDI in and the phrase played too, on the same clock as the audio;
//  - an always-on 60 s capture ring at the device's own rate. The wasm writes int24 / 2^23, which float32 holds
//    exactly, so a WAV made from the ring re-quantises to the very ints the firmware produced. Device frame j of
//    the ring plays at context time c0 + j / deviceFs (c0: the first quantum after load), in both render paths, so
//    a capture says exactly when its first frame sounded (tStart) and the page lines the .mid up with it;
//  - resampling when the browser will not run the context at 44.1 kHz: app.wasm is built for CY_FS = 44100 (the
//    host build), and played at 48 kHz unconverted it would be 1.5 semitones sharp. A Kaiser-windowed sinc (64 taps
//    at unity, 256 phases with linear interpolation between them) keeps the pitch with images below -95 dB; the
//    capture ring stays at 44.1 kHz and exact;
//  - errors reported to the page instead of a silently dead processor;
//  - CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §7.3): the page's `snd` ops (cur audition load save put clear bank)
//    through web_cy_box() and the web_cy_* exports, between render quanta like every page message, so they never
//    race app_frame; the state word in a frame when it changes; the SysEx loopback (`cysx`: the bytes between F0 and
//    F7 into web_cy_sx, the reply frames drained from web_cy_sx_out every frame); and, at ready, the factory sounds,
//    the parameter table and APP_VERSION. A build without the exports answers every op with rc -9.

const DEVICE_FS_DEFAULT = 44100;          // the host build's CY_FS when web.c has no web_cy_fs
const FRAME_S = 1 / 60;                   // frame period, seconds of context time
const HIDDEN_FRAME_S = 1 / 4;             // ... while the page is hidden: no screen, the rest at 4 Hz
const CAPTURE_S = 60;                     // the capture ring's length
const NKEYS = 27, NLEDS = 42, NB = 14;
const CHUNK = 1024;                       // web_render's most frames per call (emu_web.c out_l / out_r)
const FB_PX = 240 * 240;
const SND_PRESET = 94, SND_BANK = 3020, SND_FACTORY = 24 * 94;   // cy_preset_t, cy_user_bank_t, CY_FACTORY (§7.1)
const SX_IN_MAX = 640;                    // web_cy_sx_in: the bytes between F0 and F7 (fm1-base's receive buffer)
const SND_OPS = ["cur", "audition", "load", "save", "put", "clear", "bank"];

// the resampler's kernel: RS_ZC zero crossings each side at unity scale (64 taps), a Kaiser window of beta 10
// (stopband about -100 dB, transition 0.45..0.55 of the device rate: the firmware's output has nothing above 0.45 fs,
// the ultrasonic gate's -117 dB), RS_PHASES fractional positions with linear interpolation between neighbours
const RS_ZC = 32, RS_PHASES = 256, RS_BETA = 10;

// a clock for the platform's render timing (plat_host's now_ns and plat_ticks24 feed the screen's LOAD meter and
// the perf counters only, never the sound): the real one where the worklet scope has it, else context time
// (deterministic, and the meter then reads 0)
const realClock = typeof globalThis.performance === "object" && typeof globalThis.performance.now === "function";
const clockMs = realClock ? () => globalThis.performance.now() : () => currentTime * 1000;
// the page's CPU figure: performance.now where there is one, else Date.now (Chrome's worklet scope has no
// performance). Its 1 ms steps are coarse for one 2.9 ms quantum, but the steps fall at random against the quanta,
// so a second's sum (344 quanta) is an unbiased estimate
const loadMs = realClock ? clockMs : () => Date.now();
const stampMs = () => Math.round(currentTime * 10000) / 10;   // a packet's context time, ms in 0.1 ms steps

class Cyma extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ex = null;
    this.dead = false;
    this.queue = [];                      // timed MIDI in: [{ at, pkt }], ascending
    this.midiOut = [];                    // [pkt, t ms] since the last frame
    this.midiInLog = [];                  // [pkt, t ms] of MIDI in that reached the firmware, since the last frame
    this.nextFrame = 0;
    this.sentBlits = -1;
    this.sentWrites = -1;
    this.infoSeq = -1;
    this.visible = true;
    this.forceFb = true;
    this.busy = 0;                        // ms spent in the wasm since the last load report
    this.made = 0;                        // device frames made since then
    this.loads = 0;                       // a load in flight (a factory reset loads a fresh instance over this one)
    this.snd = false;                     // the sounds exports are there (web_cy_box)
    this.sndState = -1;                   // the state word last sent
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  // ------------------------------------------------------------------ memory views
  // INITIAL_MEMORY is fixed (no growth), but a view is cheap to remake and a grown buffer would detach the old
  views() {
    const b = this.ex.memory.buffer;
    if (b !== this.buf) {
      this.buf = b;
      this.u8 = new Uint8Array(b);
      this.dv = new DataView(b);
      this.outL = new Float32Array(b, this.ex.web_out_l(), CHUNK);
      this.outR = new Float32Array(b, this.ex.web_out_r(), CHUNK);
      const fb = this.ex.web_fb();        // the screen, compared a word at a time where it is 4-byte aligned
      this.fbView = fb & 3 ? new Uint16Array(b, fb, FB_PX) : new Uint32Array(b, fb, FB_PX / 2);
      if (!this.lastFb || this.lastFb.constructor !== this.fbView.constructor) {
        this.lastFb = new this.fbView.constructor(this.fbView.length);
        this.forceFb = true;
      }
    }
  }

  cstr(ptr, max) {                         // a C string (ASCII JSON): TextDecoder is not in every worklet scope
    const u8 = this.u8;
    let s = "";
    for (let i = ptr, end = ptr + max; i < end && u8[i]; i++) s += String.fromCharCode(u8[i]);
    return s;
  }

  // ------------------------------------------------------------------ loading
  // Also a reload: a factory reset sends `load` again with an empty store. The old instance plays on until the new
  // one is ready, then everything that belonged to it (the queue, the ring, the resampler's history) starts again.
  async load(m) {
    const self = this;
    const wasi = {
      clock_time_get(id, prec, out) {
        self.views();
        self.dv.setBigUint64(out, BigInt(Math.round(clockMs() * 1e6)), true);
        return 0;
      },
      fd_write(fd, iov, n, out) { self.views(); self.dv.setUint32(out, 0, true); return 0; },
      proc_exit() {},
    };
    const stub = new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) });
    const env = new Proxy({}, { get: () => () => 0 });
    const { instance } = await WebAssembly.instantiate(m.wasm, { wasi_snapshot_preview1: stub, env });
    const ex = instance.exports;
    for (const f of ["web_render", "web_out_l", "web_out_r", "web_boot", "web_fb", "web_blits", "web_key", "web_buttons", "web_enc", "web_master"])
      if (typeof ex[f] !== "function") throw new Error(`app.wasm has no ${f}: not an fm1-base emulator build`);
    if (ex._initialize) ex._initialize();
    this.ex = ex;
    this.buf = null;
    this.views();
    this.cy = ["web_cy_leds", "web_cy_press", "web_cy_info", "web_cy_info_seq"].every((f) => typeof ex[f] === "function");
    this.snd = typeof ex.web_cy_box === "function" && ["web_cy_state", "web_cy_cur", "web_cy_audition", "web_cy_load",
      "web_cy_save", "web_cy_put", "web_cy_clear", "web_cy_bank"].every((f) => typeof ex[f] === "function");
    this.sndState = -1;
    this.deviceFs = typeof ex.web_cy_fs === "function" ? ex.web_cy_fs() >>> 0 : DEVICE_FS_DEFAULT;
    this.ratio = this.deviceFs / sampleRate;          // device frames per context frame
    this.resampling = Math.abs(this.ratio - 1) > 1e-9;
    if (this.resampling) {
      if (!this.rs || this.rs.ratio !== this.ratio) this.rs = sincKernel(this.ratio);
      const L = this.rs.L;
      this.srcL = new Float32Array(CHUNK * 2 + 2 * L + 8);
      this.srcR = new Float32Array(CHUNK * 2 + 2 * L + 8);
      this.srcN = L;                                  // frames held: [0 .. L-1] are the (zero) history
      this.pos = L;                                   // read position in src, frames: src[L] is device frame 0
    }
    const cap = Math.round(CAPTURE_S * this.deviceFs);
    if (!this.capL || this.capL.length !== cap) {
      this.capL = new Float32Array(cap);              // ~21 MB for the pair: always on (spec §7.4)
      this.capR = new Float32Array(cap);
    }
    this.capW = 0;                                    // frames ever written
    this.c0 = null;                                   // the context time of device frame 0 (the first quantum)
    this.queue.length = 0;
    this.midiOut = [];
    this.midiInLog = [];
    this.sentBlits = -1;
    this.infoSeq = -1;
    this.forceFb = true;
    this.busy = this.made = 0;

    // saved objects (the page keeps them in localStorage) go back before boot, as the base worklet does
    if (typeof ex.web_store === "function") {
      const max = ex.web_store_max();
      for (const [obj, bytes] of Object.entries(m.store || {})) {
        const b = new Uint8Array(bytes).subarray(0, max);
        const p = ex.web_store(+obj);
        if (!p) continue;
        this.views();
        this.u8.set(b, p);
        ex.web_store_set_len(+obj, b.length);
      }
      this.sentWrites = ex.web_store_writes();
    }
    ex.web_master(clampInt(m.master ?? 3072, 0, 4096));
    ex.web_boot();
    // the viewer asked their system for less motion and this browser has no saved settings yet: the firmware's own
    // MOTION setting starts at REDUCE (saved with the other settings, so the GLOBAL page can turn it back)
    if (m.reduceMotion && typeof ex.web_cy_reduce_motion === "function") ex.web_cy_reduce_motion(1);
    this.nextFrame = currentTime;
    const ready = { type: "ready", sampleRate, deviceRate: this.deviceFs, resampling: this.resampling,
                    cy: this.cy, version: typeof ex.web_cy_version === "function" ? ex.web_cy_version() : 0, snd: this.snd };
    if (this.snd) {                                   // the factory sounds, the table and APP_VERSION, for the page
      this.views();
      if (typeof ex.web_cy_factory === "function") ready.factory = this.u8.slice(ex.web_cy_factory(), ex.web_cy_factory() + SND_FACTORY);
      if (typeof ex.web_cy_params === "function") ready.params = this.cstr(ex.web_cy_params(), 32768);
      if (typeof ex.web_cy_app_version === "function") ready.appVersion = this.cstr(ex.web_cy_app_version(), 64);
    }
    this.port.postMessage(ready);
  }

  // ------------------------------------------------------------------ the page's messages
  onMessage(m) {
    if (m.type === "load") {
      this.loads++;
      this.load(m).catch((err) => {                   // (a failed reload leaves the playing instance as it was)
        this.port.postMessage({ type: "error", where: "load", message: String(err && err.message || err) });
      }).finally(() => { this.loads--; });
      return;
    }
    if (m.type === "visible") {                     // the page is shown or hidden (the tab, the window)
      this.visible = !!m.on;
      if (this.visible) { this.forceFb = true; this.nextFrame = currentTime; }
      return;
    }
    if (!this.ex || this.dead) return;
    const ex = this.ex;
    try {
      switch (m.type) {
        case "buttons": ex.web_buttons(m.mask >>> 0); break;
        case "keys": ex.web_keys(m.mask >>> 0); break;                       // (fm1-base's: every key at vel 100)
        case "key": ex.web_key(m.k >>> 0, clampInt(m.vel, 0, 127), clampInt(m.press, 0, 1000)); break;
        case "enc": ex.web_enc(m.role | 0, m.n | 0); break;
        case "master": ex.web_master(clampInt(m.value, 0, 4096)); break;
        case "midi":
          if (typeof m.at === "number" && m.at > currentTime) this.enqueue(m.at, m.pkt >>> 0);
          else this.midiIn(m.pkt >>> 0);
          break;
        case "midiclear": this.queue.length = 0; break;
        case "capture": this.capture(m.seconds); break;
        case "demohash": {
          const id = m.id | 0;
          const hash = typeof ex.web_cy_demo_hash === "function" ? ex.web_cy_demo_hash(id) >>> 0 : null;
          this.port.postMessage({ type: "demohash", id, hash });
          break;
        }
        case "snd": this.sndOp(m); break;                                     // CYMA-1 Sounds (§7.3)
        case "cysx": this.cysx(m.bytes); break;
        case "sysex": {                                                       // (fm1-base's screen-reader frames)
          if (typeof ex.web_sysex !== "function") break;
          const b = Uint8Array.from(m.bytes).subarray(0, 64);
          this.views();
          this.u8.set(b, ex.web_sysex_buf());
          ex.web_sysex(b.length);
          break;
        }
      }
    } catch (err) {
      this.fail("message " + m.type, err);
    }
  }

  // ------------------------------------------------------------------ CYMA-1 Sounds (§7.3)
  // one op: the sound (94 B) or the bank (3,020 B) into web_cy_box(), the export, and a reply with its rc and the state
  // word (and, for cur, the box's sound). rc -9: this app.wasm has no sounds exports; -8: an op it does not know; -1:
  // a box of the wrong size. The store write an op makes goes to the page in the next frame's `store`, as SAVE's does
  sndOp(m) {
    const ex = this.ex, reply = { type: "snd", id: m.id, rc: -9, state: 0 };
    if (this.snd) {
      const box = ex.web_cy_box();
      const fill = (bytes, n) => {
        if (bytes == null) return true;
        const b = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes);
        if (b.length !== n) return false;
        this.views();
        this.u8.set(b, box);
        return true;
      };
      if (!SND_OPS.includes(m.op)) reply.rc = -8;
      else if (!fill(m.sound, SND_PRESET) || !fill(m.bank, SND_BANK)) reply.rc = -1;
      else {
        const slot = m.slot | 0;
        switch (m.op) {
          case "cur": reply.rc = ex.web_cy_cur() | 0; this.views(); reply.sound = this.u8.slice(box, box + SND_PRESET); break;
          case "audition": reply.rc = ex.web_cy_audition(m.flags & 3) | 0; break;
          case "load": reply.rc = ex.web_cy_load(m.idx | 0, m.glide ? 1 : 0) | 0; break;
          case "save": reply.rc = ex.web_cy_save(slot) | 0; break;
          case "put": reply.rc = ex.web_cy_put(slot) | 0; break;
          case "clear": reply.rc = ex.web_cy_clear(slot) | 0; break;
          case "bank": reply.rc = ex.web_cy_bank() | 0; break;
        }
      }
      reply.state = ex.web_cy_state() >>> 0;
    }
    this.port.postMessage(reply);
  }

  // the SysEx loopback: the bytes between F0 and F7 into the firmware's own handler (app_sysex); its replies are
  // drained into the next frame (drainSx), so the asynchronous COMMIT result arrives the same way
  cysx(bytes) {
    const ex = this.ex;
    if (typeof ex.web_cy_sx !== "function" || typeof ex.web_cy_sx_in !== "function" || bytes == null) return;
    const b = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes);
    if (!b.length || b.length > SX_IN_MAX) return;
    this.views();
    this.u8.set(b, ex.web_cy_sx_in());
    ex.web_cy_sx(b.length);
  }

  // whole F0..F7 reply frames, back to back in web_cy_sx_out: split, copied, and the buffer cleared
  drainSx() {
    const ex = this.ex;
    if (typeof ex.web_cy_sx_out_len !== "function") return null;
    const n = ex.web_cy_sx_out_len() >>> 0;
    if (!n) return null;
    this.views();
    const base = ex.web_cy_sx_out(), out = [];
    let start = -1;
    for (let i = 0; i < n && i < 8192; i++) {
      const c = this.u8[base + i];
      if (c === 0xF0) start = i;
      else if (c === 0xF7 && start >= 0) { out.push(this.u8.slice(base + start, base + i + 1)); start = -1; }
    }
    ex.web_cy_sx_out_clear();
    return out.length ? out : null;
  }

  // a packet into the firmware (it polls MIDI in every millisecond of device time), and into the log the page's
  // capture reads: note on/off, poly and channel pressure, CC and bend, at the quantum it reached the device
  midiIn(pkt) {
    if (typeof this.ex.web_midi_in === "function") this.ex.web_midi_in(0, pkt);
    const kind = (pkt >>> 8) & 0xf0;
    if (kind >= 0x80 && kind <= 0xe0 && kind !== 0xc0 && this.midiInLog.length < 2048) this.midiInLog.push([pkt, stampMs()]);
  }

  enqueue(at, pkt) {
    const q = this.queue;
    let i = q.length;
    while (i > 0 && q[i - 1].at > at) i--;          // stable: equal times keep their order (note-off before on)
    q.splice(i, 0, { at, pkt });
  }

  // ------------------------------------------------------------------ capture
  capture(seconds) {
    const cap = this.capL.length;
    const want = Math.max(0, Math.min(cap, Math.round((+seconds || CAPTURE_S) * this.deviceFs)));
    const n = Math.min(want, this.capW);
    const l = new Float32Array(n), r = new Float32Array(n);
    const start = (this.capW - n) % cap;              // the oldest frame wanted: the ring reads out in time order
    const first = Math.min(n, cap - start);
    l.set(this.capL.subarray(start, start + first));
    r.set(this.capR.subarray(start, start + first));
    if (first < n) {
      l.set(this.capL.subarray(0, n - first), first);
      r.set(this.capR.subarray(0, n - first), first);
    }
    // tStart: the context time the first captured frame sounded at (exact: c0 + its frame number / deviceFs);
    // tBoot: the device's boot, frame 0 (its boot "ting" sounds 1.5 s later); t1: the context time now
    const c0 = this.c0 === null ? currentTime : this.c0;
    this.port.postMessage({ type: "captured", l, r, sampleRate: this.deviceFs, t1: currentTime,
                            tStart: c0 + (this.capW - n) / this.deviceFs, tBoot: c0 }, [l.buffer, r.buffer]);
  }

  record(l, r, n) {                                  // n device frames into the ring
    const cap = this.capL.length;
    let w = this.capW % cap, i = 0;
    while (i < n) {
      const k = Math.min(n - i, cap - w);
      this.capL.set(l.subarray(i, i + k), w);
      this.capR.set(r.subarray(i, i + k), w);
      i += k;
      w = (w + k) % cap;
    }
    this.capW += n;
  }

  // ------------------------------------------------------------------ rendering
  // n device frames: the wasm runs until they exist (web_render), then they are recorded and counted
  make(n) {
    const t0 = loadMs();
    this.ex.web_render(n);
    this.busy += loadMs() - t0;
    this.made += n;
    this.views();
    const l = this.outL.subarray(0, n), r = this.outR.subarray(0, n);
    this.record(l, r, n);
    return [l, r];
  }

  renderDirect(oL, oR, n) {
    for (let i = 0; i < n; ) {
      const k = Math.min(CHUNK, n - i);
      const [l, r] = this.make(k);
      oL.set(l, i);
      if (oR) oR.set(r, i);
      i += k;
    }
  }

  // the device rate to the context's through the windowed sinc: output frame i reads src around pos_i (tap k at
  // src[ip - L + 1 + k]), the phase table at the fraction, interpolated between its two nearest phases
  renderResampled(oL, oR, n) {
    const { L, N, P, T, D } = this.rs;
    const step = this.ratio;
    let pos = this.pos;
    const need = Math.floor(pos + step * (n - 1)) + L + 1;   // the last read touches ip + L
    if (need + CHUNK > this.srcL.length) {           // (a render quantum bigger than 128: grow, keep what is held)
      const gL = new Float32Array(need + CHUNK), gR = new Float32Array(need + CHUNK);
      gL.set(this.srcL.subarray(0, this.srcN));
      gR.set(this.srcR.subarray(0, this.srcN));
      this.srcL = gL;
      this.srcR = gR;
    }
    const sL = this.srcL, sR = this.srcR;
    while (this.srcN < need) {
      const k = Math.min(CHUNK, need - this.srcN, sL.length - this.srcN);
      const [l, r] = this.make(k);
      sL.set(l, this.srcN);
      sR.set(r, this.srcN);
      this.srcN += k;
    }
    for (let i = 0; i < n; i++, pos += step) {
      const ip = Math.floor(pos), fp = (pos - ip) * P, p = fp | 0, a = fp - p;
      let t = p * N, b = ip - L + 1, accL = 0, accR = 0;
      for (let k = 0; k < N; k++, t++, b++) {
        const w = T[t] + a * D[t];
        accL += sL[b] * w;
        accR += sR[b] * w;
      }
      oL[i] = accL;
      if (oR) oR[i] = accR;
    }
    const drop = Math.floor(pos) - L + 1;            // keep the oldest frame the next read needs, onwards
    if (drop > 0) {
      sL.copyWithin(0, drop, this.srcN);
      sR.copyWithin(0, drop, this.srcN);
      this.srcN -= drop;
      pos -= drop;
    }
    this.pos = pos;
  }

  drainMidi() {
    const ex = this.ex;
    if (typeof ex.web_out_n !== "function") return;
    const n = ex.web_out_n();
    if (!n) return;
    this.views();
    const base = ex.web_out_pkts() >>> 2;
    const u32 = new Uint32Array(this.buf);
    const t = stampMs();
    for (let i = 0; i < n && i < 4096; i++) {
      const pkt = u32[base + i] >>> 0;
      if (((pkt >>> 8) & 0xff) >= 0xf8) continue;      // clock and other real-time: not the page's business
      if (this.midiOut.length < 2048) this.midiOut.push([pkt, t]);
    }
    ex.web_out_clear();
  }

  // ------------------------------------------------------------------ the frame
  // the screen when the firmware blitted and the picture differs from the last one sent: with the tour waiting the
  // rails redraw at 30 Hz, and two thirds of those blits change no pixel. A compare is ~29k words (about 20 µs)
  screenChanged() {
    const a = this.fbView, b = this.lastFb, n = a.length;
    let i = 0;
    while (i < n && a[i] === b[i]) i++;
    if (i === n) return false;
    b.set(a);
    return true;
  }

  publish() {
    const ex = this.ex;
    this.views();
    const msg = { type: "frame", now: typeof ex.web_now === "function" ? ex.web_now() >>> 0 : 0 };
    const transfer = [];
    if (this.visible) {
      const blits = ex.web_blits() >>> 0;
      if (blits !== this.sentBlits || this.forceFb) {
        this.sentBlits = blits;
        if (this.screenChanged() || this.forceFb) {
          msg.fb = new Uint16Array(this.buf, ex.web_fb(), FB_PX).slice();
          transfer.push(msg.fb.buffer);
          if (this.forceFb) { this.lastFb.set(this.fbView); this.forceFb = false; }
        }
      }
    }
    if (this.cy) {
      msg.leds = this.u8.slice(ex.web_cy_leds(), ex.web_cy_leds() + NLEDS);
      msg.press = this.u8.slice(ex.web_cy_press(), ex.web_cy_press() + NKEYS);
      const seq = ex.web_cy_info_seq() >>> 0;
      if (seq !== this.infoSeq) {
        this.infoSeq = seq;
        msg.info = this.cstr(ex.web_cy_info(), 512);
      }
    } else {                                          // another fm1-base app: lit -> 3, dim -> 1
      const lb = ex.web_lit_buttons() >>> 0, lk = ex.web_lit_keys() >>> 0;
      const db = ex.web_dim_buttons() >>> 0, dk = ex.web_dim_keys() >>> 0;
      msg.leds = new Uint8Array(NLEDS);
      for (let k = 0; k < NKEYS; k++) msg.leds[k] = (lk >>> k) & 1 ? 3 : (dk >>> k) & 1 ? 1 : 0;
      for (let b = 0; b < NB; b++) msg.leds[NKEYS + b] = (lb >>> b) & 1 ? 3 : (db >>> b) & 1 ? 1 : 0;
      msg.leds[NLEDS - 1] = 0;                       // (PLAY's green is unknown: lit PLAY shows as 3)
      msg.press = new Uint8Array(NKEYS);
    }
    if (this.snd) {                                   // the sounds state word, when it changed; SysEx replies
      const st = ex.web_cy_state() >>> 0;
      if (st !== this.sndState) { this.sndState = st; msg.snd = st; }
    }
    const sx = this.drainSx();
    if (sx) msg.cysx = sx;
    if (this.midiOut.length) {
      msg.midi = this.midiOut;
      this.midiOut = [];
    }
    if (this.midiInLog.length) {
      msg.midiIn = this.midiInLog;
      this.midiInLog = [];
    }
    if (typeof ex.web_store_writes === "function") {
      const writes = ex.web_store_writes() >>> 0;
      if (writes !== this.sentWrites) {
        this.sentWrites = writes;
        msg.store = {};
        for (let o = 0; o < ex.web_nobj(); o++) {
          const n = ex.web_store_len(o);
          if (n >= 0) msg.store[o] = this.u8.slice(ex.web_store(o), ex.web_store(o) + n);
        }
      }
    }
    if (this.made >= this.deviceFs) {                // once a second: the share of real time the device took
      msg.load = Math.round(100 * this.busy / (this.made / this.deviceFs * 1000));
      msg.realClock = realClock;
      this.busy = this.made = 0;
    }
    this.port.postMessage(msg, transfer);
  }

  fail(where, err) {
    this.dead = true;
    this.port.postMessage({ type: "error", where, message: String(err && err.message || err) });
  }

  process(inputs, outputs) {
    if (!this.ex || this.dead) return true;          // the outputs come zeroed: silence
    const out = outputs[0];
    const oL = out[0], oR = out[1] || null;
    const n = oL.length;
    try {
      if (this.c0 === null) this.c0 = currentTime;
      const due = currentTime + 0.5 * n / sampleRate;
      while (this.queue.length && this.queue[0].at <= due) this.midiIn(this.queue.shift().pkt);
      if (this.resampling) this.renderResampled(oL, oR, n);
      else this.renderDirect(oL, oR, n);
      this.drainMidi();
      if (currentTime >= this.nextFrame) {
        // the next deadline a frame on; after a stall (a background tab) start again from now, no burst
        const period = this.visible ? FRAME_S : HIDDEN_FRAME_S;
        this.nextFrame = currentTime - this.nextFrame > 0.25 ? currentTime + period : this.nextFrame + period;
        this.publish();
      }
    } catch (err) {
      oL.fill(0);
      if (oR) oR.fill(0);
      this.fail("render", err);
    }
    return true;
  }
}

// the resampler's phase table for `ratio` device frames per output frame. Upsampling (ratio < 1) keeps the device's
// whole band and cuts at its Nyquist (the band above 0.45 fs is empty, so the images start at 0.55 fs and fall in
// the stopband); downsampling scales the kernel to the output's rate and pulls the cutoff in by half a transition, so
// the stopband starts at the output's Nyquist and nothing above it folds back. Each phase is normalised to unit DC
// gain. T: (P + 1) phases of N taps; D: each phase's step to the next.
function sincKernel(ratio) {
  const scale = Math.min(1, 1 / ratio), fc = scale < 1 ? scale * 0.899 : 1;
  const L = Math.ceil(RS_ZC / scale), N = 2 * L, P = RS_PHASES;
  const T = new Float32Array((P + 1) * N), D = new Float32Array(P * N), i0b = besselI0(RS_BETA);
  for (let p = 0; p <= P; p++) {
    const f = p / P;
    let sum = 0;
    for (let k = 0; k < N; k++) {
      const x = k - (L - 1) - f, r = x / L;
      const w = Math.abs(r) >= 1 ? 0 : besselI0(RS_BETA * Math.sqrt(1 - r * r)) / i0b;
      const a = Math.PI * fc * x;
      const v = (x === 0 ? 1 : Math.sin(a) / a) * w;
      T[p * N + k] = v;
      sum += v;
    }
    for (let k = 0; k < N; k++) T[p * N + k] /= sum;
  }
  for (let i = 0; i < P * N; i++) D[i] = T[i + N] - T[i];
  return { ratio, L, N, P, T, D };
}

function besselI0(x) {
  let s = 1, t = 1;
  for (let k = 1; k < 60; k++) {
    t *= (x / (2 * k)) * (x / (2 * k));
    s += t;
    if (t < 1e-14 * s) break;
  }
  return s;
}

function clampInt(v, lo, hi) {
  v = Math.round(+v || 0);
  return v < lo ? lo : v > hi ? hi : v;
}

registerProcessor("cyma-1", Cyma);
