// [eggy] The browser render thread (EGGY.md, "The render thread").
//
// A pthread build of the game in the browser has a second core to spare, and before this file
// everything WebGPU did ran on the guest's own thread: sealing the recorded frame, encoding its
// render passes through the JS bindings, submitting, presenting and preparing the next frame.
// Here a dedicated pthread owns the page's canvas (transferred to it as an OffscreenCanvas when
// it is created) and every WebGPU object, and aurora's frame worker (aurora.cpp) runs its cycle
// on it as one job per frame, like the native builds' frame worker thread.
//
// A browser keeps a WebGPU object in the JS realm of the thread that created it, so the guest
// thread can no longer call WebGPU itself: web/js/render/library_mkw_render.js makes every
// emdawnwebgpu import, called anywhere else, run on this thread while the caller waits (the
// texture, bind group and pipeline cache misses of GX decoding, initialization). The render
// thread itself never blocks between jobs: it returns to its event loop, which is where the
// browser presents its canvas and settles WebGPU's promises (asynchronous pipelines).

#include "web_render_thread.hpp"

#include <aurora/web_render_thread.h>

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)

#include "../internal.hpp"

#include <SDL3/SDL_hints.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>

namespace aurora::webgpu::render_thread {
namespace {
Module Log("aurora::render_thread");

std::atomic<pthread_t> g_thread{0};
std::atomic<bool> g_running{false};
std::thread::id g_threadId{};
// The WebGPU calls other threads proxy here (mkw_render_run_js), apart from the jobs post() sends
// on the system queue: lock_serviced() runs only these while it waits, never a nested job.
em_proxying_queue* g_jsQueue = nullptr;

std::atomic<uint64_t> g_cycles{0};
std::atomic<uint64_t> g_cycleMicros{0};
std::atomic<uint64_t> g_proxiedCalls{0};
std::atomic<uint64_t> g_proxiedMicros{0};
std::atomic<uint64_t> g_sealWaitMicros{0};
std::atomic<uint64_t> g_doneWaitMicros{0};
std::atomic<uint64_t> g_servicedWaits{0};

void* thread_main(void*) {
  g_threadId = std::this_thread::get_id();
  g_running.store(true, std::memory_order_release);
  // Back to the worker's event loop with the thread kept alive: from here on it only runs what
  // is proxied to it (post() and the JS proxy), one task at a time.
  emscripten_exit_with_live_runtime();
  return nullptr;
}
} // namespace
} // namespace aurora::webgpu::render_thread

// Whether this page can and wants a render thread: OffscreenCanvas transfer exists, the canvas is
// in the document and not yet transferred, and neither Module.mkwRenderThread === false nor
// ?renderThread=0 turned it off. pthread_create resolves "#canvas" through Module.canvas, so that
// is set to the element when the page did not.
EM_JS(int, aurora_web_render_thread_wanted, (const char* selectorPtr), {
  if (typeof HTMLCanvasElement == 'undefined' || typeof OffscreenCanvas == 'undefined' ||
      !HTMLCanvasElement.prototype.transferControlToOffscreen) {
    return 0;
  }
  if (Module['mkwRenderThread'] === false) return -1;
  try {
    if (/[?&]renderThread=0(&|$)/.test(location.search)) return -1;
  } catch (e) {
  }
  var selector = UTF8ToString(selectorPtr);
  var canvas = null;
  try {
    canvas = document.querySelector(selector);
  } catch (e) {
  }
  if (!canvas || canvas.controlTransferredOffscreen) return -2;
  if (selector == '#canvas' && !Module['canvas']) Module['canvas'] = canvas;
  return 1;
});

namespace aurora::webgpu::render_thread {

bool start() noexcept {
  if (g_thread.load(std::memory_order_acquire) != 0) {
    return true;
  }
  if (!emscripten_is_main_browser_thread()) {
    // The canvas belongs to the browser main thread, which is where it is transferred from.
    Log.info("Render thread not started: not on the browser main thread");
    return false;
  }
  const char* selector = SDL_GetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR);
  if (selector == nullptr || *selector == '\0') {
    selector = "#canvas";
  }
  const int wanted = aurora_web_render_thread_wanted(selector);
  if (wanted <= 0) {
    Log.info("Render thread off: {}", wanted == -1 ? "the page turned it off"
                                      : wanted == -2 ? "no canvas to transfer"
                                                     : "no OffscreenCanvas transfer");
    return false;
  }
  g_jsQueue = em_proxying_queue_create();
  if (g_jsQueue == nullptr) {
    Log.warn("Render thread not started: no proxying queue");
    return false;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  emscripten_pthread_attr_settransferredcanvases(&attr, selector);
  pthread_t thread = 0;
  const int rc = pthread_create(&thread, &attr, thread_main, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    Log.warn("Render thread not started (pthread_create {}); rendering on the calling thread", rc);
    return false;
  }
  // A pooled worker (-sPTHREAD_POOL_SIZE) runs it at once; anything proxied before that waits in
  // its mailbox, so this wait only makes the log line truthful.
  const double deadline = emscripten_get_now() + 5000.0;
  while (!g_running.load(std::memory_order_acquire) && emscripten_get_now() < deadline) {
  }
  g_thread.store(thread, std::memory_order_release);
  Log.info("Render thread started: it owns {} and WebGPU ({})", selector,
           g_running.load(std::memory_order_acquire) ? "running" : "not running yet");
  return true;
}

bool active() noexcept { return g_thread.load(std::memory_order_acquire) != 0; }

bool on_render_thread() noexcept {
  const pthread_t thread = g_thread.load(std::memory_order_acquire);
  return thread != 0 && pthread_equal(thread, pthread_self());
}

std::thread::id thread_id() noexcept { return g_threadId; }

bool post(void (*fn)(void*), void* arg) noexcept {
  const pthread_t thread = g_thread.load(std::memory_order_acquire);
  if (thread == 0) {
    return false;
  }
  return emscripten_proxy_async(emscripten_proxy_get_system_queue(), thread, fn, arg) != 0;
}

void note_cycle(uint64_t micros, bool frame) noexcept {
  if (frame) {
    g_cycles.fetch_add(1, std::memory_order_relaxed);
  }
  g_cycleMicros.fetch_add(micros, std::memory_order_relaxed);
}
void note_seal_wait(uint64_t micros) noexcept { g_sealWaitMicros.fetch_add(micros, std::memory_order_relaxed); }

void lock_serviced(std::recursive_mutex& mutex) noexcept {
  if (mutex.try_lock()) {
    return;
  }
  // A worker blocked on a futex runs nothing proxied to it, so a holder that is itself waiting for
  // a WebGPU call here would never get it. The frame protocol keeps the guest out of the renderer
  // lock while a job runs; this keeps a path that does not from freezing the game.
  g_servicedWaits.fetch_add(1, std::memory_order_relaxed);
  while (!mutex.try_lock()) {
    emscripten_proxy_execute_queue(g_jsQueue);
  }
}
void note_done_wait(uint64_t micros) noexcept { g_doneWaitMicros.fetch_add(micros, std::memory_order_relaxed); }

} // namespace aurora::webgpu::render_thread

// ---- The JS proxy (web/js/render/library_mkw_render.js) ---------------------------------------
// A decorated emdawnwebgpu import called off the render thread serializes its arguments (16
// bytes each: a BigInt flag, then the value) and calls mkw_render_run_js, which runs
// mkw_render_receive_js on the render thread while the caller waits. That one calls the original
// import there and writes its result to `result` (a number at +0, a BigInt at +8, the flag at
// +16); an import that returns a promise finishes when the promise settles (the caller keeps
// waiting).
extern "C" {
void mkw_render_receive_js(int index, int nargs, const void* args, void* result, void* call);

struct MkwRenderJsCall {
  em_proxying_ctx* ctx;
  int index;
  int nargs;
  const void* args;
  void* result;
};

EMSCRIPTEN_KEEPALIVE uintptr_t mkw_render_thread_id(void) {
  return static_cast<uintptr_t>(aurora::webgpu::render_thread::g_thread.load(std::memory_order_acquire));
}

EMSCRIPTEN_KEEPALIVE void mkw_render_js_done(void* call) {
  emscripten_proxy_finish(static_cast<MkwRenderJsCall*>(call)->ctx);
}

EMSCRIPTEN_KEEPALIVE int mkw_render_run_js(int index, int nargs, const void* args, void* result) {
  using namespace aurora::webgpu::render_thread;
  const pthread_t thread = g_thread.load(std::memory_order_acquire);
  if (thread == 0) {
    return 0;
  }
  MkwRenderJsCall call{nullptr, index, nargs, args, result};
  const double started = emscripten_get_now();
  const int ok = emscripten_proxy_sync_with_ctx(
      g_jsQueue, thread,
      [](em_proxying_ctx* ctx, void* arg) {
        auto* c = static_cast<MkwRenderJsCall*>(arg);
        c->ctx = ctx;
        mkw_render_receive_js(c->index, c->nargs, c->args, c->result, c);
      },
      &call);
  g_proxiedCalls.fetch_add(1, std::memory_order_relaxed);
  g_proxiedMicros.fetch_add(static_cast<uint64_t>((emscripten_get_now() - started) * 1000.0),
                            std::memory_order_relaxed);
  return ok;
}

bool aurora_render_thread_active(void) { return aurora::webgpu::render_thread::active(); }

void aurora_render_thread_stats(AuroraRenderThreadStats* out) {
  using namespace aurora::webgpu::render_thread;
  if (out == nullptr) {
    return;
  }
  out->cycles = g_cycles.load(std::memory_order_relaxed);
  out->cycleMicros = g_cycleMicros.load(std::memory_order_relaxed);
  out->proxiedCalls = g_proxiedCalls.load(std::memory_order_relaxed);
  out->proxiedMicros = g_proxiedMicros.load(std::memory_order_relaxed);
  out->sealWaitMicros = g_sealWaitMicros.load(std::memory_order_relaxed);
  out->doneWaitMicros = g_doneWaitMicros.load(std::memory_order_relaxed);
  out->servicedWaits = g_servicedWaits.load(std::memory_order_relaxed);
}
} // extern "C"

#else // no pthreads: there is never a render thread

namespace aurora::webgpu::render_thread {
bool start() noexcept { return false; }
bool active() noexcept { return false; }
bool on_render_thread() noexcept { return false; }
std::thread::id thread_id() noexcept { return {}; }
bool post(void (*)(void*), void*) noexcept { return false; }
void note_cycle(uint64_t, bool) noexcept {}
void note_seal_wait(uint64_t) noexcept {}
void note_done_wait(uint64_t) noexcept {}
void lock_serviced(std::recursive_mutex& mutex) noexcept { mutex.lock(); }
} // namespace aurora::webgpu::render_thread

extern "C" {
bool aurora_render_thread_active(void) { return false; }
void aurora_render_thread_stats(AuroraRenderThreadStats* out) {
  if (out != nullptr) {
    *out = AuroraRenderThreadStats{};
  }
}
}

#endif
