// [Eggy] The baton browser build (MKW_WEB_THREADS=baton, no JSPI): what of aurora runs on the
// browser main thread. Empty in every other build. See EGGY.md ("iOS 26: the baton build").
//
// The guest, and main() with it (-sPROXY_TO_PTHREAD), runs on workers that block: in a futex
// while another guest thread holds the baton, or in a sleep. A blocked worker never returns to
// its event loop, so it can neither present a canvas frame nor have WebGPU work flushed or
// promises settled. The canvas, its WebGPU device and SDL's window and event handling therefore
// live on the browser main thread, and the guest reaches them like this:
//
//   * aurora's frame entry points (aurora_begin_frame, _end_frame, _update, _shutdown,
//     _set_display_mode) run there whole, synchronously: about three round trips a frame, and the
//     frame is encoded and submitted where the JSPI build did it too.
//   * aurora_initialize stays on the guest's worker, so it can block while the adapter and the
//     device are requested; only its SDL window steps run on the main thread (SDL registers its
//     DOM listeners for the calling thread, and a worker would never receive them).
//   * the few WebGPU calls a guest thread makes while it decodes the GX FIFO (texture, bind
//     group and pipeline cache misses) are proxied one by one by web/js/baton/library_mkw_baton.js.
//
// None of this edits aurora: runtime/cmake/WebBaton.cmake links with --wrap for every symbol
// below, so a reference from aurora's objects reaches __wrap_X here and __real_X is the original.
// A name that stops matching fails the link (an undefined __real_X), it cannot go silently
// unproxied.
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)

#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>

#include <aurora/aurora.h>

#include "platform/web_host.h"

namespace {

std::atomic<uint32_t>& Counter(WebHost::BatonCounter counter)
{
    return WebHost::BatonCounters()[counter];
}

// Runs fn() on the browser main thread and returns its result; a plain call when already there.
// `kind` names it in the per-frame breakdown (WebHost::BatonMainCallStats).
template <typename F>
auto OnMainThread(WebHost::BatonMainCall kind, F&& fn) -> std::invoke_result_t<F&>
{
    using Result = std::invoke_result_t<F&>;
    if (emscripten_is_main_browser_thread()) {
        return fn();
    }
    struct Call {
        std::remove_reference_t<F>* fn;
        std::conditional_t<std::is_void_v<Result>, char, Result> result{};
        double runMs = 0.0;
    } call{&fn};
    const double start = emscripten_get_now();
    const int ok = emscripten_proxy_sync(
        emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
        [](void* argument) {
            auto* c = static_cast<Call*>(argument);
            const double runStart = emscripten_get_now();
            if constexpr (std::is_void_v<Result>) {
                (*c->fn)();
            } else {
                c->result = (*c->fn)();
            }
            c->runMs = emscripten_get_now() - runStart;
        },
        &call);
    if (!ok) {
        std::fprintf(stderr, "[ios26] baton: could not run a call on the browser main thread\n");
    }
    const auto waitMicros = static_cast<uint32_t>((emscripten_get_now() - start) * 1000.0);
    Counter(WebHost::kBatonMainCalls).fetch_add(1, std::memory_order_relaxed);
    Counter(WebHost::kBatonMainCallMicros).fetch_add(waitMicros, std::memory_order_relaxed);
    WebHost::BatonCallStats& stats = WebHost::BatonMainCallStats()[kind];
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    stats.waitMicros.fetch_add(waitMicros, std::memory_order_relaxed);
    stats.runMicros.fetch_add(static_cast<uint32_t>(call.runMs * 1000.0), std::memory_order_relaxed);
    if constexpr (!std::is_void_v<Result>) {
        return call.result;
    }
}

// For the one-time steps: say where they ran, so a smoke log shows the split.
template <typename F>
auto OnMainThreadNoted(const char* what, F&& fn) -> std::invoke_result_t<F&>
{
    std::printf("[ios26] aurora: %s on the browser main thread\n", what);
    return OnMainThread(WebHost::kMainCallOther, std::forward<F>(fn));
}

constexpr uint32_t kWaitStatusTimedOut = 2; // WGPUWaitStatus_TimedOut (webgpu/webgpu.h)

} // namespace

extern "C" {

// ---- aurora's C API (aurora/aurora.h) --------------------------------------------------------
bool __real_aurora_begin_frame();
void __real_aurora_end_frame();
const AuroraEvent* __real_aurora_update();
void __real_aurora_shutdown();
void __real_aurora_set_display_mode(AuroraDisplayMode mode);

bool __wrap_aurora_begin_frame()
{
    return OnMainThread(WebHost::kMainCallBeginFrame, [] { return __real_aurora_begin_frame(); });
}

void __wrap_aurora_end_frame()
{
    OnMainThread(WebHost::kMainCallEndFrame, [] { __real_aurora_end_frame(); });
}

const AuroraEvent* __wrap_aurora_update()
{
    return OnMainThread(WebHost::kMainCallUpdate, [] { return __real_aurora_update(); });
}

void __wrap_aurora_shutdown()
{
    OnMainThreadNoted("aurora_shutdown", [] { __real_aurora_shutdown(); });
}

void __wrap_aurora_set_display_mode(AuroraDisplayMode mode)
{
    OnMainThread(WebHost::kMainCallOther, [mode] { __real_aurora_set_display_mode(mode); });
}

// ---- SDL window steps inside aurora::initialize (aurora-main/lib/window.hpp, imgui.hpp) -------
// Itanium-mangled C++ names: aurora::window::initialize(), create_window(AuroraBackend),
// destroy_window(), show_window(), create_renderer() and aurora::imgui::initialize().
bool __real__ZN6aurora6window10initializeEv();
bool __real__ZN6aurora6window13create_windowE13AuroraBackend(AuroraBackend backend);
void __real__ZN6aurora6window14destroy_windowEv();
void __real__ZN6aurora6window11show_windowEv();
bool __real__ZN6aurora6window15create_rendererEv();
void __real__ZN6aurora5imgui10initializeEv();

bool __wrap__ZN6aurora6window10initializeEv()
{
    return OnMainThreadNoted("window::initialize (SDL video and events)",
                             [] { return __real__ZN6aurora6window10initializeEv(); });
}

bool __wrap__ZN6aurora6window13create_windowE13AuroraBackend(AuroraBackend backend)
{
    return OnMainThreadNoted("window::create_window",
                             [backend] { return __real__ZN6aurora6window13create_windowE13AuroraBackend(backend); });
}

void __wrap__ZN6aurora6window14destroy_windowEv()
{
    OnMainThreadNoted("window::destroy_window", [] { __real__ZN6aurora6window14destroy_windowEv(); });
}

void __wrap__ZN6aurora6window11show_windowEv()
{
    OnMainThreadNoted("window::show_window", [] { __real__ZN6aurora6window11show_windowEv(); });
}

bool __wrap__ZN6aurora6window15create_rendererEv()
{
    return OnMainThreadNoted("window::create_renderer", [] { return __real__ZN6aurora6window15create_rendererEv(); });
}

void __wrap__ZN6aurora5imgui10initializeEv()
{
    OnMainThreadNoted("imgui::initialize", [] { __real__ZN6aurora5imgui10initializeEv(); });
}

// ---- WebGPU waits (emdawnwebgpu) -------------------------------------------------------------
// A timed WaitAny needs Asyncify or JSPI in emdawnwebgpu, and asking for the TimedWaitAny
// instance feature without them makes wgpuCreateInstance fail. aurora asks for it and waits up
// to 5 s for its adapter and device. Here the instance is created without the feature, and a
// timed wait polls instead: from a worker it sleeps between polls while the main thread settles
// the promise; on the main thread itself nothing can settle meanwhile, so it polls once.
void* __real_wgpuCreateInstance(const void* descriptor);
uint32_t __real_wgpuInstanceWaitAny(void* instance, size_t count, void* futures, uint64_t timeoutNS);

void* __wrap_wgpuCreateInstance(const void* descriptor)
{
    (void)descriptor; // aurora's web descriptor holds only the TimedWaitAny feature
    std::printf("[ios26] aurora: WebGPU instance without TimedWaitAny (no JSPI/Asyncify); timed "
                "waits poll from the worker\n");
    return __real_wgpuCreateInstance(nullptr);
}

uint32_t __wrap_wgpuInstanceWaitAny(void* instance, size_t count, void* futures, uint64_t timeoutNS)
{
    if (timeoutNS == 0 || emscripten_is_main_browser_thread()) {
        return __real_wgpuInstanceWaitAny(instance, count, futures, 0);
    }
    const double deadline = emscripten_get_now() + static_cast<double>(timeoutNS) / 1e6;
    for (;;) {
        const uint32_t status = __real_wgpuInstanceWaitAny(instance, count, futures, 0);
        if (status != kWaitStatusTimedOut || emscripten_get_now() >= deadline) {
            return status;
        }
        usleep(500);
    }
}

// ---- emscripten_sleep -------------------------------------------------------------------------
// Needs Asyncify or JSPI; aurora sleeps while it waits for the canvas to be laid out. A worker can
// simply block.
void __wrap_emscripten_sleep(unsigned int ms)
{
    if (!emscripten_is_main_browser_thread()) {
        usleep(static_cast<useconds_t>(ms) * 1000);
    }
}

} // extern "C"

static_assert(sizeof(AuroraBackend) == sizeof(int), "AuroraBackend is passed as an i32");

#endif
