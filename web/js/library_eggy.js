// Eggy's host API for the WebAssembly build, linked with --js-library (Eggy's own file; see
// EGGY.md). C++ side: runtime/src/eggy/eggy.cpp (runtime/include/eggy/eggy.h).
//
// Eggy runs Mario Kart Wii races in a phone browser: the engine boots to an idle state, loads a
// race from a config, parks on the race's first frame until it is told to go, takes the four pads
// of every race frame from the page, and reports the result. The rule it lives by: JavaScript
// never calls guest code. Everything below either writes a mailbox in a small block of wasm
// memory that the engine reads between frames, or reads a snapshot the engine wrote at the end of
// a frame. The only calls into the page go the other way, from the engine's own frame loop.
//
// The page turns Eggy mode on by setting Module.eggy to an object before the glue runs:
//   Module.eggy = {
//     padSource(frame) -> [4 pads] | Promise<[4 pads]>   pad = {buttons, x, y} (games/mkw/kit/pad.ts)
//                                                        or a Uint8Array(16) of 4-byte pads
//     onFrame(frame)        after every race frame the engine ran (optional)
//     onState(state)        'booting' 'idle' 'loading' 'parked' 'racing' 'finished' 'results'
//                           'failed' ('results': the game's own finish and results screens, only
//                           when the config's word 17 asks for them; see EGGY.md "Race API 2")
//     onReport(report)      the race finished: {endFrame, hash, lines: [{slot, place, finishMs, score}]}
//     onReady()             the engine attached; the methods below work from now on
//   }
// and the library adds the API to that same object (all synchronous, all safe at any time):
//   state() frame() load(Int32Array) go() abort() renderSkip(on) audio(music, sfx)
//   hudInsets(l, t, r, b) away(port, frame) finishAll(frame) pace(factor)
//   kart(port) hud() hash() report() timing() debug()
// and since race API 2 (Module.eggy.raceApi === 2; everything above is unchanged):
//   drawEvery(n)   draw one pass of the frame loop in n (1-4) during a race and its results
//   pacing(on)     keep the race at the wall clock's 60 frames/s (on by default)
//   slowDraw(us)   test only: every drawn race frame takes this much longer
//   perf()         the last second's breakdown, or null (see `perf()` below and EGGY.md)
// kart() and hud() are snapshots of every 4th race frame, hash() of the lockstep's checkpoint
// frames (every 60th: (frame + 1) % 60 == 0), every 120th and the last.
// web/site/eggy/eggy-engine.js wraps these into games/mkw/client/engine/seam.ts's EngineLike.
//
// Shared block layout (little-endian words; keep in step with eggy.cpp `Shared`).

addToLibrary({
  $Eggy__postset: 'Eggy.install();',
  $Eggy: {
    STATES: ['off', 'booting', 'idle', 'loading', 'parked', 'racing', 'finished', 'failed', 'results'],
    W: {
      MAGIC: 0, VERSION: 1, STATE: 2, FRAME: 3, HASH_FRAME: 4, HASH: 5, STAGE: 6, COUNTDOWN: 7,
      REPORT_SEQ: 8, ERROR: 9, SECTION: 10, TICKS: 11, LOGIC_US: 12, DRAW_US: 13, PAD_CALLS: 14,
      RACE_TIMER: 15,
      LOAD_SEQ: 16, GO_SEQ: 17, ABORT_SEQ: 18, RENDER_SKIP: 19, MUSIC: 20, SFX: 21,
      INSETS: 22, PACE: 26, FINISH_ALL: 27, AWAY: 28, DRAW_EVERY: 32, PACING_OFF: 33, DEBUG: 34,
      SLOW_DRAW_US: 35,
      CONFIG: 48, PADS: 168, KARTS: 176, KART_WORDS: 32, HUD: 304, REPORT: 384,
      DIAG: 448, PERF: 480, SIZE: 512,
    },
    CONFIG_WORDS: 120,
    base: 0,
    waiter: null,
    padCancel: null,

    install() {
      const api = Module['eggy'];
      if (!api || typeof api != 'object') return;
      const call = (name, ...args) => {
        const fn = api[name];
        if (typeof fn != 'function') return undefined;
        try {
          return fn.apply(api, args);
        } catch (e) {
          err(`[eggy] Module.eggy.${name} threw: ${e && e.stack || e}`);
          return undefined;
        }
      };
      Eggy.call = call;
      Object.assign(api, {
        'ready': false,
        'raceApi': 2,
        'state': () => Eggy.base ? Eggy.STATES[Eggy.i32(Eggy.W.STATE)] || 'failed' : 'booting',
        'frame': () => Eggy.base ? Eggy.i32(Eggy.W.FRAME) : -1,
        'load': (config) => Eggy.load(config),
        'go': () => Eggy.bump(Eggy.W.GO_SEQ),
        'abort': () => Eggy.bump(Eggy.W.ABORT_SEQ),
        'renderSkip': (on) => Eggy.set(Eggy.W.RENDER_SKIP, on ? 1 : 0),
        'audio': (music, sfx) => {
          Eggy.set(Eggy.W.MUSIC, music ? 1 : 0);
          Eggy.set(Eggy.W.SFX, sfx ? 1 : 0);
        },
        'hudInsets': (l, t, r, b) => {
          if (!Eggy.base) return;
          const f = [l, t, r, b].map((v) => Math.max(0, Number(v) || 0));
          for (let i = 0; i < 4; i++) HEAPF32[(Eggy.base >> 2) + Eggy.W.INSETS + i] = f[i];
          Eggy.wake();
        },
        'away': (port, frame) => {
          if (port >= 0 && port < 4) Eggy.set(Eggy.W.AWAY + port, frame | 0);
        },
        'finishAll': (frame) => Eggy.set(Eggy.W.FINISH_ALL, frame | 0),
        'pace': (factor) => {
          if (!Eggy.base) return;
          const f = Number(factor);
          HEAPF32[(Eggy.base >> 2) + Eggy.W.PACE] = Number.isFinite(f) ? Math.min(1.1, Math.max(0.9, f)) : 1;
        },
        'kart': (port) => Eggy.kart(port),
        'hud': () => Eggy.hud(),
        'hash': () => Eggy.base ? Eggy.u32(Eggy.W.HASH) : 0,
        'report': () => Eggy.report(),
        'timing': () => {
          if (!Eggy.base) return null;
          const logic = Eggy.i32(Eggy.W.LOGIC_US), draw = Eggy.i32(Eggy.W.DRAW_US);
          return logic > 0 ? { 'logicMs': logic / 1000, 'drawMs': draw / 1000 } : null;
        },
        'drawEvery': (n) => Eggy.set(Eggy.W.DRAW_EVERY, Math.max(1, Math.min(4, n | 0))),
        'pacing': (on) => Eggy.set(Eggy.W.PACING_OFF, on ? 0 : 1),
        'slowDraw': (us) => Eggy.set(Eggy.W.SLOW_DRAW_US, Math.max(0, Math.min(200000, us | 0))),
        'perf': () => Eggy.perf(),
        'debug': () => Eggy.debug(),
      });
    },

    i32(w) { return HEAP32[(Eggy.base >> 2) + w]; },
    u32(w) { return HEAPU32[(Eggy.base >> 2) + w]; },
    f32(w) { return HEAPF32[(Eggy.base >> 2) + w]; },
    set(w, v) {
      if (!Eggy.base) return;
      HEAP32[(Eggy.base >> 2) + w] = v;
      Eggy.wake();
    },
    bump(w) {
      if (!Eggy.base) return;
      HEAP32[(Eggy.base >> 2) + w] = (HEAP32[(Eggy.base >> 2) + w] + 1) | 0;
      // Abort and load also end a pad wait (the race they belong to is over).
      if (w === Eggy.W.ABORT_SEQ || w === Eggy.W.LOAD_SEQ) Eggy.padCancel?.();
      Eggy.wake();
    },
    // Resumes the engine if it is parked in eggy_wait (idle or parked on a race's first frame).
    wake() {
      const resolve = Eggy.waiter;
      Eggy.waiter = null;
      if (resolve) resolve();
    },

    load(config) {
      if (!Eggy.base) throw new Error('the engine has not attached yet');
      const words = config instanceof Int32Array ? config : Int32Array.from(config || []);
      const at = (Eggy.base >> 2) + Eggy.W.CONFIG;
      HEAP32.fill(0, at, at + Eggy.CONFIG_WORDS);
      HEAP32.set(words.subarray(0, Eggy.CONFIG_WORDS), at);
      // A new race: nobody is away and nothing is scheduled to finish it yet.
      for (let p = 0; p < 4; p++) HEAP32[(Eggy.base >> 2) + Eggy.W.AWAY + p] = -1;
      HEAP32[(Eggy.base >> 2) + Eggy.W.FINISH_ALL] = -1;
      Eggy.bump(Eggy.W.LOAD_SEQ);
    },

    kart(port) {
      if (!Eggy.base || !(port >= 0 && port < 4)) return null;
      const w = Eggy.W.KARTS + port * Eggy.W.KART_WORDS;
      if (Eggy.i32(w) !== 1) return null;
      const startCharge = Eggy.i32(w + 15) ? Eggy.f32(w + 14) : null;
      const countdown = Eggy.i32(w + 16);
      return {
        'speed': Eggy.f32(w + 2),
        'maxSpeed': Eggy.f32(w + 3),
        'mt': Eggy.i32(w + 4),
        'mtMax': Eggy.i32(w + 5),
        'smt': Eggy.i32(w + 6),
        'smtMax': Eggy.i32(w + 7),
        'drifting': Eggy.i32(w + 8) !== 0,
        'driftDir': Eggy.i32(w + 9),
        'airborne': Eggy.i32(w + 10) !== 0,
        'trickable': Eggy.i32(w + 11) !== 0,
        'bike': Eggy.i32(w + 12) !== 0,
        'wheelie': Eggy.i32(w + 13) !== 0,
        'startCharge': startCharge,
        'countdown': countdown >= 0 ? countdown : null,
        'respawning': Eggy.i32(w + 17) !== 0,
        'finished': Eggy.i32(w + 18) !== 0,
        'place': Eggy.i32(w + 19),
        'lap': Eggy.i32(w + 20),
      };
    },

    hud() {
      if (!Eggy.base) return null;
      const w = Eggy.W.HUD;
      const frame = Eggy.i32(w), count = Eggy.i32(w + 1);
      if (frame < 0 || count <= 0) return null;
      const lines = [];
      for (let i = 0; i < Math.min(12, count); i++) {
        const l = w + 2 + i * 6;
        lines.push({
          'slot': Eggy.i32(l),
          'place': Eggy.i32(l + 1),
          'lap': Eggy.i32(l + 2),
          'score': Eggy.i32(l + 3),
          'item': Eggy.i32(l + 4),
          'finished': Eggy.i32(l + 5) !== 0,
        });
      }
      return { 'frame': frame, 'lines': lines };
    },

    report() {
      if (!Eggy.base || Eggy.i32(Eggy.W.REPORT_SEQ) === 0) return null;
      const w = Eggy.W.REPORT;
      const count = Eggy.i32(w + 2);
      const lines = [];
      for (let i = 0; i < Math.min(12, count); i++) {
        const l = w + 3 + i * 4;
        const ms = Eggy.i32(l + 2);
        lines.push({ 'slot': Eggy.i32(l), 'place': Eggy.i32(l + 1), 'finishMs': ms >= 0 ? ms : null, 'score': Eggy.i32(l + 3) });
      }
      return { 'endFrame': Eggy.i32(w), 'hash': Eggy.u32(w + 1), 'lines': lines };
    },

    // The breakdown of the last window (about a second) of a race or its results: where the wall
    // time went, per game frame, in ms. Each part is counted once (a part inside another is taken
    // out of it); `otherMs` is what no part covers. null before the first window of a race.
    //   windowMs  frames  passes (of the frame loop)  drawn  gameFramesPerSec (60 is real speed)
    //   calcMs drawMs submitMs presentMs audioMs padsWaitMs yieldMs idleMs otherMs  (per frame)
    //   late20 late34 (passes over 20 / 34 ms)  drawEvery  pacing
    //   extraCalcs (catch-up calcs run)  droppedFrames (debt let go)  behindMax (frames)
    perf() {
      if (!Eggy.base) return null;
      const w = Eggy.W.PERF;
      if (Eggy.i32(w) === 0) return null;
      const windowUs = Eggy.i32(w + 1), frames = Eggy.i32(w + 2);
      const per = (us) => (frames > 0 ? us / frames / 1000 : 0);
      const parts = ['calcMs', 'drawMs', 'submitMs', 'presentMs', 'audioMs', 'padsWaitMs', 'yieldMs', 'idleMs'];
      const out = {
        'windowMs': windowUs / 1000,
        'frames': frames,
        'passes': Eggy.i32(w + 3),
        'drawn': Eggy.i32(w + 4),
        'gameFramesPerSec': windowUs > 0 ? (frames * 1e6) / windowUs : 0,
      };
      let counted = 0;
      parts.forEach((name, i) => {
        const us = Eggy.i32(w + 5 + i);
        counted += us;
        out[name] = per(us);
      });
      out['otherMs'] = per(Math.max(0, windowUs - counted));
      out['late20'] = Eggy.i32(w + 13);
      out['late34'] = Eggy.i32(w + 14);
      out['drawEvery'] = Eggy.i32(w + 15);
      out['extraCalcs'] = Eggy.i32(w + 16);
      out['droppedFrames'] = Eggy.i32(w + 17);
      out['behindMax'] = Eggy.i32(w + 18);
      out['pacing'] = Eggy.i32(w + 19) === 1;
      out['seq'] = Eggy.i32(w);
      return out;
    },

    debug() {
      if (!Eggy.base) return null;
      const out = {};
      for (const [k, w] of Object.entries(Eggy.W)) if (w < 48) out[k] = Eggy.i32(w);
      const diag = [];
      for (let i = 0; i < 32; i++) diag.push(Eggy.i32(Eggy.W.DIAG + i));
      out['diag'] = diag;
      return out;
    },

    // 16 bytes of pads -> wasm memory at `dest`. Accepts 4 {buttons, x, y} or a 16-byte array.
    writePads(pads, dest) {
      if (pads && pads.length === 16 && typeof pads[0] == 'number') {
        HEAPU8.set(pads, dest);
        return;
      }
      for (let p = 0; p < 4; p++) {
        const pad = pads && pads[p];
        const b = pad ? (pad['buttons'] | 0) & 0xffff : 0;
        const x = pad ? Math.max(-128, Math.min(127, Math.round(pad['x'] || 0))) : 0;
        const y = pad ? Math.max(-128, Math.min(127, Math.round(pad['y'] || 0))) : 0;
        HEAPU8[dest + p * 4] = b & 0xff;
        HEAPU8[dest + p * 4 + 1] = b >> 8;
        HEAP8[dest + p * 4 + 2] = x;
        HEAP8[dest + p * 4 + 3] = y;
      }
    },
  },

  // 1 when the page asked for Eggy mode (Module.eggy is set). Read once at startup.
  eggy_enabled__deps: ['$Eggy'],
  eggy_enabled: () => (Module['eggy'] && typeof Module['eggy'] == 'object') ? 1 : 0,

  // The engine's shared block, once, from main before the game starts.
  eggy_attach__deps: ['$Eggy'],
  eggy_attach: (ptr, size) => {
    Eggy.base = ptr;
    const api = Module['eggy'];
    if (!api) return;
    api['ready'] = true;
    setTimeout(() => Eggy.call('onReady'), 0);
  },

  // Parks the engine's stack until a mailbox is written (Eggy.wake) or `ms` passes (ms <= 0:
  // no timeout). Used while idle and while a loaded race waits for go: nothing of the game runs.
  eggy_wait__deps: ['$Eggy'],
  eggy_wait__async: true,
  eggy_wait: (ms) => new Promise((resolve) => {
    Eggy.waiter = resolve;
    if (ms > 0) setTimeout(() => { if (Eggy.waiter === resolve) Eggy.wake(); }, ms);
  }),

  // The four pads of race frame `frame` into `dest` (16 bytes). 1 when the page's pad source
  // supplied them, 0 when there is none (the engine then uses neutral pads), -1 when abort() or
  // load() ended the wait. A source that has the pads already answers at once, with no
  // suspension (most frames: a solo race always, a linked one whenever its packets are in); the
  // engine's stack parks only while the source's promise is pending: that is the lockstep's wait.
  eggy_pads__deps: ['$Eggy'],
  eggy_pads__async: true,
  eggy_pads: (frame, dest) => {
    const api = Module['eggy'];
    const source = api && api['padSource'];
    if (typeof source != 'function') return 0;
    let pads;
    try {
      pads = source.call(api, frame);
    } catch (e) {
      err(`[eggy] padSource(${frame}) threw: ${e && e.stack || e}`);
      return 0;
    }
    if (pads && typeof pads.then == 'function') {
      return new Promise((resolve) => {
        let done = false;
        const finish = (value) => {
          if (done) return;
          done = true;
          if (Eggy.padCancel === cancel) Eggy.padCancel = null;
          resolve(value);
        };
        const cancel = () => finish(-1);
        Eggy.padCancel = cancel;
        pads.then((value) => { if (!done) { Eggy.writePads(value, dest); finish(1); } },
                  (e) => { err(`[eggy] padSource(${frame}) rejected: ${e && e.stack || e}`); finish(0); });
      });
    }
    Eggy.writePads(pads, dest);
    return 1;
  },

  // After every race frame the engine ran (snapshots and hash already written for it).
  eggy_frame_done__deps: ['$Eggy'],
  eggy_frame_done: (frame) => { Eggy.call('onFrame', frame); },

  // The engine's state changed (eggy.cpp State). Page callbacks run on a later task.
  eggy_state__deps: ['$Eggy'],
  eggy_state: (state) => {
    const name = Eggy.STATES[state] || 'failed';
    setTimeout(() => Eggy.call('onState', name), 0);
  },

  // A race finished and its report is in the shared block.
  eggy_report_ready__deps: ['$Eggy'],
  eggy_report_ready: () => {
    const report = Eggy.report();
    setTimeout(() => Eggy.call('onReport', report), 0);
  },
});
