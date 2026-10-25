// For web/tests/baton/baton_test.cpp. An `__async` import marked `__proxy: 'sync'`: a worker that
// calls it blocks until the promise settles on the browser main thread (Emscripten's
// PROXY_SYNC_ASYNC), with no JSPI and no Asyncify. The baton build's disc reads
// (web/js/library_mkw_dvd.js) and Eggy's waits rely on exactly this. Resolves to 7 only where
// there is a document, so a call that ran on the worker instead is caught.
addToLibrary({
  baton_test_async_on_main__proxy: 'sync',
  baton_test_async_on_main__async: true,
  baton_test_async_on_main: (ms) => new Promise((resolve) => {
    setTimeout(() => resolve(typeof document == 'object' ? 7 : -1), ms);
  }),

  // The same through an `async` function (the form of mkw_dvd_load_manifest and
  // mkw_storage_flush), which the baton library turns into a plain promise-returning one.
  baton_test_async_function__proxy: 'sync',
  baton_test_async_function__async: true,
  baton_test_async_function: async (x) => {
    await new Promise((resolve) => setTimeout(resolve, 5));
    return typeof document == 'object' ? x + 1 : -1;
  },

  // What Eggy's load(), go() and abort() do on the page: write a mailbox and wake the engine. The
  // test calls it before eggy_wait, the order the page and the engine's worker can race into.
  baton_test_wake_eggy__proxy: 'sync',
  baton_test_wake_eggy__deps: ['$Eggy'],
  baton_test_wake_eggy: () => Eggy.wake(),
});
