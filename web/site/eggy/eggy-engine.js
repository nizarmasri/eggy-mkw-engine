// Eggy's engine wrapper: the WiiCompiled build with Module.eggy (web/js/library_eggy.js), shaped
// like Eggy's EngineLike (games/mkw/client/engine/seam.ts), so the app's adapter is a few lines.
// Plain script, no dependencies; defines globalThis.createEggyEngine(options).
//
//   const engine = createEggyEngine({
//     canvas,                      // the canvas the game draws into (required)
//     base: './',                  // where WiiCompiled.js, its wasm and products.json are served
//     module: {},                  // extra Module fields (mkwDiscSource, mkwDiscManifestUrl, print...)
//     preload: async (keys, onProgress) => {},   // optional: the app's own pack preloading
//   });
//   await engine.boot();                 // once per page: the engine reaches its idle state
//   await engine.load(configV1);         // the race is built and parked on its first frame
//   engine.setPadSource((frame) => pads); // 4 pads ({buttons, x, y}) or a promise of them
//   engine.onFrame((frame) => ...);
//   engine.go();
//   const report = await engine.ended(); // {endFrame, hash, lines: [{slot, place, finishMs, score}]}
//
// Nothing here calls into guest code: every method writes a mailbox the engine reads between
// frames, or reads the snapshot it wrote at the end of the last one.
(function (root) {
  'use strict';

  const STATE_MAP = { off: 'booting', booting: 'booting', idle: 'ready', loading: 'loading', parked: 'parked', racing: 'racing', finished: 'finished', results: 'results', failed: 'failed' };

  function whyNot() {
    if (!root.navigator || !root.navigator.gpu) return 'This browser has no WebGPU.';
    if (typeof WebAssembly === 'undefined' || typeof WebAssembly.Suspending !== 'function') return 'This browser has no WebAssembly JSPI.';
    return null;
  }

  function createEggyEngine(options) {
    const opts = options || {};
    const base = (opts.base || './').replace(/\/?$/, '/');
    const log = opts.log || ((...a) => console.log('[eggy-engine]', ...a));
    const frameListeners = new Set();
    const stateListeners = new Set();
    let padSource = null;
    let engineState = 'off';
    let booted = null;
    let endedWaiters = [];
    let lastReport = null;
    let parkedEl = null;

    // Module.eggy: the page side of the shared block (library_eggy.js fills in the methods).
    const eggy = {
      padSource(frame) {
        const source = padSource;
        if (!source) return null;
        return source(frame);
      },
      onFrame(frame) {
        for (const cb of frameListeners) {
          try { cb(frame); } catch (e) { console.error('[eggy-engine] onFrame listener threw', e); }
        }
      },
      onState(state) {
        engineState = state;
        log('state', state);
        for (const cb of [...stateListeners]) cb(state);
        if (state === 'idle' && endedWaiters.length && !lastReport) {
          // Aborted: nobody gets a report.
          const waiters = endedWaiters;
          endedWaiters = [];
          for (const w of waiters) w(null);
        }
      },
      onReport(report) {
        lastReport = report;
        log('report', JSON.stringify(report));
        const waiters = endedWaiters;
        endedWaiters = [];
        for (const w of waiters) w(report);
      },
      onReady() {
        log('attached');
      },
    };

    const Module = Object.assign({
      canvas: opts.canvas,
      eggy: eggy,
      print: (...a) => console.log(...a),
      printErr: (...a) => console.error(...a),
      onAbort: (what) => {
        engineState = 'failed';
        dead = 'The engine stopped: ' + what;
        for (const cb of [...stateListeners]) cb('failed');
      },
      onMkwFatal: (message) => {
        engineState = 'failed';
        dead = 'The engine stopped: ' + message;
        for (const cb of [...stateListeners]) cb('failed');
      },
    }, opts.module || {});

    let dead = whyNot();

    function waitFor(states, failStates) {
      return new Promise((resolve, reject) => {
        const check = (s) => {
          if (states.includes(s)) { stateListeners.delete(check); resolve(s); return true; }
          if ((failStates || ['failed']).includes(s)) { stateListeners.delete(check); reject(new Error(dead || 'the engine failed')); return true; }
          return false;
        };
        if (!check(engineState)) stateListeners.add(check);
      });
    }

    async function loadGlue() {
      let wasmName = null;
      try {
        const products = await (await fetch(base + 'products.json', { cache: 'no-cache' })).json();
        const product = Array.isArray(products) ? products.find((p) => p && p.id === 'WiiCompiled') || products[0] : null;
        if (product && typeof product.wasm === 'string') wasmName = product.wasm;
      } catch (e) {
        log('no products.json, using WiiCompiled.wasm');
      }
      const userLocate = Module.locateFile;
      Module.locateFile = (path, prefix) => {
        if (path === 'WiiCompiled.wasm' && wasmName) return base + wasmName;
        if (userLocate) return userLocate(path, prefix);
        return base + path;
      };
      root.Module = Module;
      await new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = base + 'WiiCompiled.js';
        s.async = true;
        s.onload = resolve;
        s.onerror = () => reject(new Error('Could not load ' + s.src));
        document.body.appendChild(s);
      });
    }

    const engine = {
      get state() { return STATE_MAP[engineState] || 'failed'; },
      get dead() { return dead; },
      get frame() { return eggy.frame ? eggy.frame() : -1; },
      get module() { return Module; },

      preload(keys, onProgress) {
        if (dead) return Promise.reject(new Error(dead));
        return opts.preload ? Promise.resolve(opts.preload(keys, onProgress)) : Promise.resolve();
      },

      boot() {
        if (dead) return Promise.reject(new Error(dead));
        booted = booted || (async () => {
          engineState = 'booting';
          await loadGlue();
          await waitFor(['idle']);
        })();
        return booted;
      },

      async load(config) {
        if (dead) throw new Error(dead);
        await engine.boot();
        lastReport = null;
        eggy.load(config);
        await waitFor(['parked'], ['failed']);
      },

      go() { if (eggy.go) eggy.go(); },

      async abort() {
        if (!eggy.abort || engineState === 'idle' || engineState === 'off') return;
        eggy.abort();
        await waitFor(['idle']);
      },

      ended() {
        if (lastReport) return Promise.resolve(lastReport);
        return new Promise((resolve) => endedWaiters.push(resolve));
      },

      setPadSource(source) { padSource = source || null; },

      onFrame(cb) {
        frameListeners.add(cb);
        return () => frameListeners.delete(cb);
      },

      kart(port) { return eggy.kart ? eggy.kart(port) : null; },
      hud() { return eggy.hud ? eggy.hud() : null; },
      hash() { return eggy.hash ? eggy.hash() : 0; },
      report() { return eggy.report ? eggy.report() : null; },
      debug() { return eggy.debug ? eggy.debug() : null; },
      renderSkip(on) { if (eggy.renderSkip) eggy.renderSkip(on); },
      audio(music, sfx) { if (eggy.audio) eggy.audio(music, sfx); },
      hudInsets(l, t, r, b) { if (eggy.hudInsets) eggy.hudInsets(l, t, r, b); },
      away(port, frame) { if (eggy.away) eggy.away(port, frame); },
      finishAll(frame) { if (eggy.finishAll) eggy.finishAll(frame); },
      pace(factor) { if (eggy.pace) eggy.pace(factor); },
      timing() { return eggy.timing ? eggy.timing() : null; },
      // Race API 2 (library_eggy.js): picture rate, wall-clock pace and the per-frame breakdown.
      drawEvery(n) { if (eggy.drawEvery) eggy.drawEvery(n); },
      pacing(on) { if (eggy.pacing) eggy.pacing(on); },
      slowDraw(us) { if (eggy.slowDraw) eggy.slowDraw(us); },
      hashParts(on) { if (eggy.hashParts) eggy.hashParts(on); },
      perf() { return eggy.perf ? eggy.perf() : null; },

      // The canvas into `el`; park() takes it back off screen (it keeps its WebGPU context).
      attach(el) {
        if (opts.canvas && el && opts.canvas.parentElement !== el) el.appendChild(opts.canvas);
      },
      park() {
        if (!opts.canvas) return;
        if (!parkedEl) {
          parkedEl = document.createElement('div');
          parkedEl.style.cssText = 'position:fixed;left:-10000px;top:0;width:1px;height:1px;overflow:hidden';
          document.body.appendChild(parkedEl);
        }
        parkedEl.appendChild(opts.canvas);
      },
    };
    return engine;
  }

  root.createEggyEngine = createEggyEngine;
})(typeof globalThis !== 'undefined' ? globalThis : window);
