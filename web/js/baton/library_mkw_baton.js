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
})();

addToLibrary({
  // Diagnostics (runtime/src/host_context.cpp mkw_baton_counters(), WebHost::BatonCounter):
  // every JS call a worker proxies to the main thread is counted in slot 0 and the time the worker
  // spent blocked on it (microseconds) in slot 1. The main thread prints a heartbeat of all the
  // counters, which shows whether the guest is still handing the baton around even before (or
  // without) a presented frame.
  $MkwBaton__deps: ['$proxyToMainThread', 'mkw_baton_counters'],
  $MkwBaton__postset: 'MkwBaton.install();',
  $MkwBaton: {
    counters: 0,
    heartbeat: 0,

    install() {
      if (!ENVIRONMENT_IS_PTHREAD) {
        out(`[ios26] baton glue on the browser main thread: JSPI ${typeof WebAssembly.Suspending == 'function' ? 'present (unused)' : 'absent'}, ` +
            `crossOriginIsolated ${globalThis.crossOriginIsolated === true}, ` +
            `${globalThis.navigator?.hardwareConcurrency ?? '?'} logical cores`);
        return;
      }
      const proxy = proxyToMainThread;
      proxyToMainThread = (...args) => {
        if (!MkwBaton.counters) MkwBaton.counters = _mkw_baton_counters() >> 2;
        const start = performance.now();
        const result = proxy(...args);
        Atomics.add(HEAPU32, MkwBaton.counters, 1);
        Atomics.add(HEAPU32, MkwBaton.counters + 1, Math.round((performance.now() - start) * 1000) >>> 0);
        return result;
      };
    },

    startHeartbeat() {
      if (MkwBaton.heartbeat) return;
      const base = _mkw_baton_counters() >> 2;
      const read = () => Array.from({ length: 7 }, (_, i) => Atomics.load(HEAPU32, base + i));
      const start = performance.now();
      let last = read();
      let beats = 0;
      MkwBaton.heartbeat = setInterval(() => {
        const now = read();
        const d = now.map((v, i) => (v - last[i]) >>> 0);
        last = now;
        ++beats;
        // Every 5 s for the first minute, then every 30 s.
        if (beats > 12 && beats % 6) return;
        out(`[ios26] heartbeat ${((performance.now() - start) / 1000).toFixed(0)} s (last 5 s): ` +
            `${d[2]} hand-offs, ${d[0]} JS calls on the main thread, ${d[5]} aurora calls on the main ` +
            `thread, ${d[4]} frames; totals ${now[2]} hand-offs, ${now[4]} frames`);
      }, 5000);
    },
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
    MkwBaton.startHeartbeat();
  },
});
