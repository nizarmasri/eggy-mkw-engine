// [Eggy] The baton browser build (MKW_WEB_THREADS=baton): Mario Kart Wii without JSPI, for
// Safari before iOS 27. Linked only into that build, after every other --js-library
// (runtime/cmake/WebBaton.cmake), so the default JSPI build never sees this file.
// See EGGY.md ("iOS 26: the baton build").
//
// The guest runs on pthreads (Web Workers), main() included (-sPROXY_TO_PTHREAD). A worker's copy
// of Module is not the page's: everything that reads the page (Module.mkwDiscSource, the save
// store, Module.eggy, the input panel) or owns a JS object table (emdawnwebgpu) runs on the browser
// main thread. A JS library function marked `__proxy: 'sync'` does that: called from a worker, it
// runs on the main thread while the worker blocks; an `__async` one (a promise under JSPI) blocks
// the worker until the main thread's promise settles (Emscripten's PROXY_SYNC_ASYNC), which is
// the JSPI contract without JSPI.

// emdawnwebgpu keeps its JS objects in a table on the thread that created them, so all of its
// imports run on the main thread; a call from the main thread itself stays a plain call. Port
// libraries are processed after --js-library files, so the decorators are added while
// emdawnwebgpu merges its library. A proxied call cannot return a 64-bit value: the three getters
// that do (wgpuBufferGetSize/GetUsage, wgpuTextureGetUsage) are left alone, aurora uses none.
(() => {
  const mergeIntoLibrary = globalThis.mergeInto;
  globalThis.mergeInto = (target, library, options) => {
    if (library && typeof library == 'object' && Object.prototype.hasOwnProperty.call(library, '$WebGPU')) {
      for (const key of Object.keys(library)) {
        if (typeof library[key] != 'function' || isDecorator(key)) continue;
        if (!/^(wgpu|emwgpu|emscripten_webgpu_)/.test(key)) continue;
        if (library[key + '__proxy'] !== undefined) continue;
        const sig = library[key + '__sig'] ?? LibraryManager.library[key + '__sig'];
        if (typeof sig == 'string' && sig[0] == 'j') continue;
        library[key + '__proxy'] = 'sync';
      }
    }
    return mergeIntoLibrary(target, library, options);
  };
})();

// The other web libraries, already merged: the imports the guest calls that belong to the page.
// Only what this link actually has is decorated (a test links a subset).
(() => {
  const lib = LibraryManager.library;
  const proxy = (key, mode) => {
    if (typeof lib[key] == 'function' && lib[key + '__proxy'] === undefined) lib[key + '__proxy'] = mode;
  };
  // A proxied import whose result is a promise (`__async`, or an `async` function) needs two
  // things Emscripten does not do for it without JSPI/Asyncify: the function itself must not be
  // `async` (Emscripten would then wrap the proxying stub in an async function, and the worker
  // would get a Promise object where it expects the value), and a rejection must not leave the
  // worker waiting forever (Emscripten only handles fulfilment). So it becomes a plain function
  // returning the original's promise, and a rejection aborts the module with the reason, which
  // is what the JSPI build does when the promise of a suspending import rejects.
  const promiseImport = (key) => {
    const src = lib[key].toString();
    const m = /^(?:async\s*)?(?:function\b[^(]*)?\(([^)]*)\)/.exec(src) || /^(?:async\s+)?([A-Za-z_$][\w$]*)\s*=>/.exec(src);
    if (!m) {
      error(`[ios26] ${key}: cannot make this function form a proxied promise import`);
      return;
    }
    const args = m[1].trim();
    lib[key] = (0, eval)(`(${args}) => Promise.resolve((${src})(${args})).catch((e) => {
      err('[ios26] ${key} failed on the main thread: ' + (e && e.stack || e));
      abort(e);
    })`);
  };
  // web/js/library_mkw_dvd.js: the disc (fetch or Module.mkwDiscSource) and the save store.
  for (const key of ['mkw_dvd_load_manifest', 'mkw_dvd_read', 'mkw_dvd_mod_mount_name',
                     'mkw_storage_flush', 'mkw_storage_is_persistent', 'mkw_storage_is_readonly']) {
    proxy(key, 'sync');
  }
  // A worker's copy of the disc library must not start manifest downloads of its own.
  if (lib['$MkwDvd__postset'] === 'MkwDvd.init();') {
    lib['$MkwDvd__postset'] = 'if (!ENVIRONMENT_IS_PTHREAD) MkwDvd.init();';
  }
  // web/js/library_mkw_input.js: Module.mkwInput and the input panel.
  proxy('mkw_input_attach', 'sync');
  proxy('mkw_input_describe', 'sync');
  // web/js/library_mkw_host.js: the page's frame counter and error overlay.
  proxy('mkw_host_note_frame', 'async');
  proxy('mkw_host_report_fatal', 'sync');
  // web/js/library_eggy.js: every eggy_* import talks to Module.eggy on the page.
  for (const key of Object.keys(lib)) {
    if (!key.startsWith('eggy_') || isDecorator(key)) continue;
    const sig = lib[key + '__sig'];
    if (typeof sig == 'string' && sig[0] == 'j') {
      warn(`[ios26] ${key} returns a 64-bit value and cannot be proxied to the main thread`);
      continue;
    }
    proxy(key, 'sync');
  }
  // A wake-up must not be lost between the engine reading its mailboxes and parking. Under JSPI the
  // page can only write a mailbox while the engine is parked (one thread), but here the engine runs
  // on a worker while the page runs: load(), go() or abort() can call Eggy.wake() after the engine
  // looked and before its eggy_wait reached the main thread, which then waited for a wake that
  // already happened (a smoke's race never loaded). So a wake that finds nobody waiting is kept,
  // and the next eggy_wait returns at once; the engine re-reads its mailboxes and parks again if
  // nothing is new (eggy.cpp ParkUntilMailbox loops). Both functions are rewritten from their own
  // source, so library_eggy.js stays the one place they are written.
  if (lib['$Eggy'] && typeof lib['$Eggy'].wake == 'function' && typeof lib['eggy_wait'] == 'function') {
    const wake = lib['$Eggy'].wake.toString();
    if (!/^wake\s*\(\s*\)\s*\{/.test(wake)) {
      error('[ios26] library_eggy.js Eggy.wake() changed form; update web/js/baton/library_mkw_baton.js');
    } else {
      lib['$Eggy'].missedWake = false;
      lib['$Eggy'].wake = (0, eval)(`(function () {
        if (!Eggy.waiter) Eggy.missedWake = true;
        ${wake.slice(wake.indexOf('{') + 1, wake.lastIndexOf('}'))}
      })`);
      const wait = lib['eggy_wait'].toString();
      lib['eggy_wait'] = (0, eval)(`(ms) => {
        if (Eggy.missedWake) {
          Eggy.missedWake = false;
          return Promise.resolve();
        }
        return (${wait})(ms);
      }`);
    }
  }
  for (const key of Object.keys(lib)) {
    if (!/^(mkw_|eggy_|baton_test_)/.test(key) || isDecorator(key) || typeof lib[key] != 'function') continue;
    if (lib[key + '__proxy'] !== 'sync') continue;
    if (lib[key + '__async'] || /^async\b/.test(lib[key].toString())) promiseImport(key);
  }
})();

addToLibrary({
  // Diagnostics (runtime/src/host_context.cpp mkw_baton_counters(), WebHost::BatonCounter):
  // every JS call a worker proxies to the main thread is counted in slot 0 and the time the worker
  // spent blocked on it (microseconds) in slot 1, and the same by import in
  // WebHost::BatonImportStats (index in Emscripten's proxiedFunctionTable), where the main thread
  // adds the time it spent running the call. The main thread prints a heartbeat of all the
  // counters, which shows whether the guest is still handing the baton around even before (or
  // without) a presented frame.
  $MkwBaton__deps: ['$proxyToMainThread', 'mkw_baton_counters', 'mkw_baton_import_stats',
                    'mkw_baton_main_call_stats'],
  $MkwBaton__postset: 'MkwBaton.install();',
  $MkwBaton: {
    counters: 0,
    imports: 0,
    heartbeat: 0,
    // Import names by slot, kept on the main thread before its table entries are wrapped.
    names: null,
    // WebHost::kBatonImportSlots; the last slot counts MAIN_THREAD_EM_ASM code.
    importSlots: 512,

    slotOf(funcIndex, emAsmAddr) {
      return emAsmAddr ? MkwBaton.importSlots - 1 : Math.min(funcIndex, MkwBaton.importSlots - 2);
    },

    install() {
      if (!ENVIRONMENT_IS_PTHREAD) {
        out(`[ios26] baton glue on the browser main thread: JSPI ${typeof WebAssembly.Suspending == 'function' ? 'present (unused)' : 'absent'}, ` +
            `crossOriginIsolated ${globalThis.crossOriginIsolated === true}, ` +
            `${globalThis.navigator?.hardwareConcurrency ?? '?'} logical cores`);
        return;
      }
      const proxy = proxyToMainThread;
      proxyToMainThread = (funcIndex, emAsmAddr, proxyMode, ...callArgs) => {
        if (!MkwBaton.counters) {
          MkwBaton.counters = _mkw_baton_counters() >> 2;
          MkwBaton.imports = _mkw_baton_import_stats() >> 2;
        }
        const start = performance.now();
        const result = proxy(funcIndex, emAsmAddr, proxyMode, ...callArgs);
        const micros = Math.round((performance.now() - start) * 1000) >>> 0;
        const slot = MkwBaton.imports + 3 * MkwBaton.slotOf(funcIndex, emAsmAddr);
        Atomics.add(HEAPU32, MkwBaton.counters, 1);
        Atomics.add(HEAPU32, MkwBaton.counters + 1, micros);
        Atomics.add(HEAPU32, slot, 1);
        Atomics.add(HEAPU32, slot + 1, micros);
        return result;
      };
    },

    // On the main thread: each proxied function adds the time it ran (until its promise settled,
    // for a promise import) to its import's third word. Only the table the proxying reads is
    // wrapped; the wasm imports stay the functions themselves.
    timeProxiedCalls() {
      if (typeof proxiedFunctionTable == 'undefined' || MkwBaton.names) return;
      const base = _mkw_baton_import_stats() >> 2;
      MkwBaton.names = proxiedFunctionTable.map((fn) => fn?.name ?? '');
      proxiedFunctionTable.forEach((fn, index) => {
        if (typeof fn != 'function') return;
        const word = base + 3 * MkwBaton.slotOf(index, 0) + 2;
        proxiedFunctionTable[index] = function (...args) {
          const start = performance.now();
          const done = () => { Atomics.add(HEAPU32, word, Math.round((performance.now() - start) * 1000) >>> 0); };
          const result = fn.apply(this, args);
          if (result && typeof result.then == 'function') result.then(done, done);
          else done();
          return result;
        };
      });
    },

    // Sum of one word (0 calls, 1 wait, 2 run) over `count` call-stats entries at `base` (words).
    sumStats(base, count, word) {
      let total = 0;
      for (let i = 0; i < count; ++i) total += Atomics.load(HEAPU32, base + 3 * i + word);
      return total >>> 0;
    },

    startHeartbeat() {
      if (MkwBaton.heartbeat) return;
      const base = _mkw_baton_counters() >> 2;
      const imports = _mkw_baton_import_stats() >> 2;
      const mainCalls = _mkw_baton_main_call_stats() >> 2;
      // The counters, then the main thread's own time on proxied JS and on aurora's entry points.
      const read = () => [
        ...Array.from({ length: 7 }, (_, i) => Atomics.load(HEAPU32, base + i)),
        MkwBaton.sumStats(imports, MkwBaton.importSlots, 2),
        MkwBaton.sumStats(mainCalls, 4, 2),
      ];
      const start = performance.now();
      let last = read();
      let beats = 0;
      // How late a 20 ms timer fires: the longest the main thread was busy in one go.
      let stall = 0;
      let due = performance.now() + 20;
      const probe = () => {
        const now = performance.now();
        if (globalThis.document?.visibilityState !== 'hidden') stall = Math.max(stall, now - due);
        due = now + 20;
        setTimeout(probe, 20);
      };
      setTimeout(probe, 20);
      MkwBaton.heartbeat = setInterval(() => {
        const now = read();
        const d = now.map((v, i) => (v - last[i]) >>> 0);
        last = now;
        const worst = stall;
        stall = 0;
        ++beats;
        // Every 5 s for the first minute, then every 30 s.
        if (beats > 12 && beats % 6) return;
        out(`[ios26] heartbeat ${((performance.now() - start) / 1000).toFixed(0)} s (last 5 s): ` +
            `${d[2]} hand-offs, ${d[0]} JS calls on the main thread, ${d[5]} aurora calls on the main ` +
            `thread, ${d[4]} frames; totals ${now[2]} hand-offs, ${now[4]} frames; main thread ran ` +
            `proxied JS ${(d[7] / 1000).toFixed(0)} ms and aurora ${(d[8] / 1000).toFixed(0)} ms, ` +
            `longest stall ${worst.toFixed(0)} ms`);
      }, 5000);
    },
  },

  // The name of the proxied JS import counted in `slot` of WebHost::BatonImportStats, for the
  // per-frame breakdown host_context.cpp prints. Runs on the calling worker (its own copy of the
  // table has the same order).
  mkw_baton_import_name__deps: ['$MkwBaton', '$stringToUTF8'],
  mkw_baton_import_name: (slot, outPtr, size) => {
    let name = '';
    if (slot == MkwBaton.importSlots - 1) {
      name = 'MAIN_THREAD_EM_ASM';
    } else if (typeof proxiedFunctionTable != 'undefined') {
      name = MkwBaton.names?.[slot] || proxiedFunctionTable[slot]?.name || '';
    }
    stringToUTF8(name.replace(/^_+/, '') || `#${slot}`, outPtr, size);
  },

  // Keeps the 32-bit word at `hidden` equal to (document.visibilityState == 'hidden') and wakes
  // its futex on every change, so a guest thread can test it for free and block on it
  // (WebHost::WaitWhilePageHidden). Module.mkwPaused / mkwParkCount as in the JSPI build. Called
  // once, when the scheduler starts; it also starts the heartbeat.
  mkw_host_watch_visibility__proxy: 'sync',
  mkw_host_watch_visibility__deps: ['$MkwBaton'],
  mkw_host_watch_visibility: (hidden) => {
    const index = hidden >> 2;
    const update = () => {
      const isHidden = typeof document != 'undefined' && document.visibilityState === 'hidden';
      if (isHidden && !Module['mkwPaused']) {
        Module['mkwParkCount'] = (Module['mkwParkCount'] || 0) + 1;
      }
      Module['mkwPaused'] = isHidden;
      Atomics.store(HEAP32, index, isHidden ? 1 : 0);
      Atomics.notify(HEAP32, index);
    };
    update();
    if (typeof document != 'undefined') document.addEventListener('visibilitychange', update);
    MkwBaton.timeProxiedCalls();
    MkwBaton.startHeartbeat();
  },
});
