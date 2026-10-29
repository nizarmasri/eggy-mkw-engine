// [Eggy] The render thread (EGGY.md, "The render thread"): every emdawnwebgpu import runs on the
// one pthread that owns the canvas and WebGPU (aurora-main/lib/webgpu/web_render_thread.cpp).
// Linked only into pthreads builds (runtime/cmake/PublicProducts.cmake), after the other
// --js-library files.
//
// A browser keeps each WebGPU object in the JS realm of the thread that created it, and
// emdawnwebgpu keeps its table of them (WebGPU.Internals.jsObjects) per thread, so a WebGPU call
// has to run where the object lives. Once the render thread exists, a decorated import called on
// any other thread (the guest's, while it decodes GX: texture, bind group and pipeline cache
// misses; aurora's initialization) runs there while the caller waits: its arguments are
// serialized as Emscripten's own main-thread proxying does, mkw_render_run_js proxies the call,
// and mkw_render_receive_js runs the original function on the render thread. One that returns a
// promise (emwgpuWaitAny, a timed wait) finishes when it settles. Called on the browser main
// thread, that one never blocks it: Chrome sets up a worker's WebGPU connection with a task on the
// main thread, so a main thread blocked on the render thread's requestAdapter would wait forever.
// It returns a promise instead (the import is a JSPI suspending one, so the guest's wasm suspends
// and the main thread runs its event loop), resolved by mkw_render_async_done_js. On the render
// thread itself, or before it exists (or in a page that turned it off), the import is the
// original function plus one comparison.
//
// Port libraries are processed after --js-library files, so the decoration happens while
// emdawnwebgpu merges its library, as web/js/baton/library_mkw_baton.js does on the ios26 branch.
(() => {
  const mergeIntoLibrary = globalThis.mergeInto;
  let next = 0;
  globalThis.mergeInto = (target, library, options) => {
    if (library && typeof library == 'object' && Object.prototype.hasOwnProperty.call(library, '$WebGPU')) {
      for (const key of Object.keys(library)) {
        if (typeof library[key] != 'function' || isDecorator(key)) continue;
        if (!/^(wgpu|emwgpu|emscripten_webgpu_)/.test(key)) continue;
        if (library[key + '__proxy'] !== undefined) {
          error(`[render] ${key} already has a __proxy decorator; web/js/render/library_mkw_render.js cannot also send it to the render thread`);
          continue;
        }
        let src = library[key].toString();
        // Method shorthand (`key(a) {}`) is only valid inside an object: make it a function.
        if (src.startsWith(key)) src = 'function ' + src;
        else if (src.startsWith('async ' + key)) src = 'async function ' + src.slice(6);
        const m = /^(?:async\s*)?(?:function\b[^(]*)?\(([^)]*)\)/.exec(src) || /^(?:async\s+)?([A-Za-z_$][\w$]*)\s*=>/.exec(src);
        if (!m) {
          error(`[render] ${key}: cannot read its parameters; web/js/render/library_mkw_render.js cannot proxy it`);
          continue;
        }
        const params = m[1].split(',').map((p) => p.trim()).filter(Boolean);
        if (params.some((p) => !/^[A-Za-z_$][\w$]*$/.test(p))) {
          error(`[render] ${key}: parameters ${m[1]} are not plain names`);
          continue;
        }
        const index = next++;
        const original = '$mkwr_' + key;
        library[original] = src;
        library[original + '__deps'] = library[key + '__deps'] ?? [];
        library[original + '__postset'] = `MkwRender.fns[${index}] = mkwr_${key};`;
        const args = params.join(', ');
        // An import that can suspend (emwgpuWaitAny, a timed wait) must not hand the render thread
        // a promise: nothing there runs under a JSPI promising export, so suspending would trap.
        // There it returns 0 (no future completed: the wait timed out), which only aurora's
        // release_surface can meet (it waits for the queue before dropping the surface).
        const async = !!library[key + '__async'];
        const local = async
          ? `{ var r = mkwr_${key}(${args}); return MkwRender.onRenderThread() ? MkwRender.noWait(r, '${key}') : r; }`
          : `return mkwr_${key}(${args});`;
        const remote = async
          ? `return ENVIRONMENT_IS_PTHREAD ? MkwRender.call(${index}, [${args}]) : MkwRender.callAsync(${index}, [${args}]);`
          : `return MkwRender.call(${index}, [${args}]);`;
        library[key] = (0, eval)(`(function (${args}) {
          if (MkwRender.local()) ${local}
          ${remote}
        })`);
        library[key + '__deps'] = [original, '$MkwRender'];
      }
    }
    return mergeIntoLibrary(target, library, options);
  };
})();

// Every worker runs the libraries' postsets too. A worker's copy of the disc library must not start
// manifest downloads (or mount the save store) of its own: only the page's thread reads the disc.
(() => {
  const lib = LibraryManager.library;
  if (lib['$MkwDvd__postset'] === 'MkwDvd.init();') {
    lib['$MkwDvd__postset'] = 'if (!ENVIRONMENT_IS_PTHREAD) MkwDvd.init();';
  } else if (lib['$MkwDvd__postset'] !== undefined) {
    error('[render] library_mkw_dvd.js MkwDvd__postset changed form; update web/js/render/library_mkw_render.js');
  }
})();

addToLibrary({
  $MkwRender__deps: ['$stackSave', '$stackRestore', '$stackAlloc', 'malloc', 'free', 'mkw_render_run_js',
                     'mkw_render_run_js_async', 'mkw_render_thread_id', 'pthread_self'],
  $MkwRender: {
    // The original imports by index, filled in on every thread by their postsets.
    fns: [],
    // The render thread's pthread_t once it exists (0 until then, and in a page without one),
    // and this thread's own.
    thread: 0,
    self: 0,
    noWaitNoted: false,
    local() {
      if (!MkwRender.thread) {
        MkwRender.thread = _mkw_render_thread_id();
        if (!MkwRender.thread) return true;
      }
      if (!MkwRender.self) MkwRender.self = _pthread_self();
      return MkwRender.self === MkwRender.thread;
    },
    onRenderThread() {
      return MkwRender.thread !== 0 && MkwRender.self === MkwRender.thread;
    },
    // A suspending import's promise on the render thread: let it settle unobserved, answer 0.
    noWait(rtn, name) {
      if (!rtn || typeof rtn.then != 'function') return rtn;
      rtn.then(() => {}, () => {});
      if (!MkwRender.noWaitNoted) {
        MkwRender.noWaitNoted = true;
        out(`render thread: ${name} cannot wait there and answers 0 (timed out)`);
      }
      return 0;
    },
    // The arguments as mkw_render_receive_js reads them: 16 bytes each, a BigInt flag then the
    // value; and the result's flag cleared.
    put(buf, args) {
      for (var i = 0; i < args.length; i++) {
        var arg = args[i];
        if (typeof arg == 'bigint') {
          {{{ makeSetValue('buf', '16 * i', '1n', 'i64') }}};
          {{{ makeSetValue('buf', '16 * i + 8', 'arg', 'i64') }}};
        } else {
          {{{ makeSetValue('buf', '16 * i', '0n', 'i64') }}};
          {{{ makeSetValue('buf', '16 * i + 8', '+arg', 'double') }}};
        }
      }
      var result = buf + 16 * args.length;
      {{{ makeSetValue('result', 16, 0, 'i32') }}};
      return result;
    },
    get(result) {
      return {{{ makeGetValue('result', 16, 'i32') }}} ? {{{ makeGetValue('result', 8, 'i64') }}}
                                                     : {{{ makeGetValue('result', 0, 'double') }}};
    },
    // Runs import `index` on the render thread with `args` (numbers or BigInts) and returns its
    // result, waiting for it.
    call(index, args) {
      var sp = stackSave();
      var buf = stackAlloc(16 * args.length + 24);
      var result = MkwRender.put(buf, args);
      if (!_mkw_render_run_js(index, args.length, buf, result)) {
        stackRestore(sp);
        abort(`[render] could not run WebGPU import ${index} on the render thread`);
      }
      var rtn = MkwRender.get(result);
      stackRestore(sp);
      return rtn;
    },
    // The same without blocking: a promise of the result, settled from this thread's event loop.
    // The buffer is malloc'd, since the caller's wasm stack is suspended meanwhile.
    // Plain data only: the library object is written into the module as source text.
    pending: {},
    lastToken: 0,
    callAsync(index, args) {
      var buf = _malloc(16 * args.length + 24);
      var result = MkwRender.put(buf, args);
      return new Promise((resolve) => {
        var token = ++MkwRender.lastToken;
        MkwRender.pending[token] = (ok) => {
          var rtn = ok ? MkwRender.get(result) : 0;
          _free(buf);
          if (!ok) abort(`[render] WebGPU import ${index} was cancelled on the render thread`);
          resolve(rtn);
        };
        if (!_mkw_render_run_js_async(index, args.length, buf, result, token)) {
          delete MkwRender.pending[token];
          _free(buf);
          abort(`[render] could not send WebGPU import ${index} to the render thread`);
        }
      });
    },
    store(result, rtn) {
      if (typeof rtn == 'bigint') {
        {{{ makeSetValue('result', 8, 'rtn', 'i64') }}};
        {{{ makeSetValue('result', 16, 1, 'i32') }}};
      } else {
        {{{ makeSetValue('result', 0, 'rtn === undefined ? 0 : +rtn', 'double') }}};
      }
    },
  },

  // The browser main thread's wait without spinning (web_render_thread.cpp
  // aurora_web_wait_changed): the guest's wasm suspends until the word changes (a notify from any
  // thread wakes it) or the timeout passes. A word that already changed answers at once.
  mkw_wait_changed_async__async: true,
  mkw_wait_changed_async__sig: 'ipid',
  mkw_wait_changed_async: (word, seen, timeoutMs) => {
    var r = Atomics.waitAsync(HEAP32, word >>> 2, seen, timeoutMs);
    return r.async ? r.value.then(() => 1) : 1;
  },

  // On the calling thread, from its event loop: an asynchronous call finished (ok) or was
  // cancelled (web_render_thread.cpp mkw_render_run_js_async).
  mkw_render_async_done_js__deps: ['$MkwRender'],
  mkw_render_async_done_js: (token, ok) => {
    var settle = MkwRender.pending[token];
    delete MkwRender.pending[token];
    if (settle) settle(ok);
  },

  // On the render thread: the proxied call (web_render_thread.cpp mkw_render_run_js).
  mkw_render_receive_js__deps: ['$MkwRender', 'mkw_render_js_done'],
  // Pointers as 'p', so they arrive unsigned (the module may address up to 4 GB).
  mkw_render_receive_js__sig: 'viippp',
  mkw_render_receive_js: (index, nargs, buf, result, call) => {
    var args = [];
    for (var i = 0; i < nargs; i++) {
      args.push({{{ makeGetValue('buf', '16 * i', 'i64') }}} ? {{{ makeGetValue('buf', '16 * i + 8', 'i64') }}}
                                                          : {{{ makeGetValue('buf', '16 * i + 8', 'double') }}});
    }
    var fn = MkwRender.fns[index];
    var rtn;
    try {
      rtn = fn(...args);
    } catch (e) {
      // The calling thread would wait forever; the module is broken either way, as a WebGPU
      // binding that throws on the calling thread is.
      err(`[render] WebGPU import ${index} threw on the render thread: ${e && e.stack || e}`);
      _mkw_render_js_done(call);
      abort(e);
    }
    if (rtn && typeof rtn.then == 'function') {
      rtn.then((value) => {
        MkwRender.store(result, value);
        _mkw_render_js_done(call);
      }, (e) => {
        err(`[render] WebGPU import ${index} failed on the render thread: ${e && e.stack || e}`);
        _mkw_render_js_done(call);
        abort(e);
      });
      return;
    }
    MkwRender.store(result, rtn);
    _mkw_render_js_done(call);
  },
});
