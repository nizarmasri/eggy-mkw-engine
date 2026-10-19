#pragma once

// Waiting in the browser build. The game never returns from main(), so a plain
// sleep (libc nanosleep busy-waits on the main thread) would freeze the page:
// no rendering, no input, no fetch completions. Instead the waiting wasm stack
// suspends through JSPI (web/js/library_mkw_host.js) and the browser runs.
// Callers must be on a JSPI-suspendable stack: main() or a guest host context,
// never a JS callback.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
// [Eggy] Baton build (MKW_WEB_THREADS=baton, no JSPI): the guest runs on workers, which may
// block. See runtime/src/host_context.cpp and web/js/baton/library_mkw_baton.js.
#include <thread>
#elif defined(__EMSCRIPTEN__)
extern "C" void mkw_host_yield(int ms);
extern "C" int mkw_host_page_hidden(void);
extern "C" void mkw_host_wait_visible(void);
extern "C" void mkw_host_note_frame(void);
#else
#include <thread>
#endif

namespace WebHost {

// Total host time the guest spent parked while the page was hidden. Guest-visible clocks subtract
// it so a background tab looks like a paused console, not a time jump. Always 0 natively.
inline std::atomic<int64_t>& ParkedNanosCounter() {
    static std::atomic<int64_t> parked{0};
    return parked;
}
inline int64_t ParkedNanos() { return ParkedNanosCounter().load(std::memory_order_relaxed); }

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
// Guest threads are workers here: the browser main thread renders and takes input on its own, so
// there is nothing to yield to, and a sleep is a real blocking wait (Atomics.wait, sub-ms).
inline void Yield() {}

template <typename Clock, typename Duration>
inline void SleepUntil(std::chrono::time_point<Clock, Duration> deadline) {
    std::this_thread::sleep_until(deadline);
}

// Kept in shared memory by the page's visibilitychange listener (host_context.cpp).
bool PageHidden();
// Blocks the calling guest thread (and so the whole console, which only ever runs on the baton
// holder) until the page is visible again; returns how long that took.
std::chrono::steady_clock::duration WaitWhilePageHidden();
// Counts the frame for the page and, every few seconds, logs what the baton costs per frame.
void NoteFramePresented();

// Diagnostics in shared memory (mkw_baton_counters() in host_context.cpp), bumped by the
// runtime and by web/js/baton/library_mkw_baton.js (slots 0 and 1). Microsecond sums wrap;
// only differences are ever read.
enum BatonCounter : uint32_t {
    kBatonJsCalls = 0,       // JS library calls a worker proxied to the browser main thread
    kBatonJsCallMicros = 1,  // ... and the time the worker spent blocked on them
    kBatonHandOffs = 2,      // HostContext::Switch calls
    kBatonHandOffMicros = 3, // hand-off to the moment the destination runs (futex wake latency)
    kBatonFrames = 4,        // frames presented
    kBatonMainCalls = 5,     // aurora entry points run on the browser main thread
    kBatonMainCallMicros = 6,
    kBatonCounterCount = 8,
};
std::atomic<uint32_t>* BatonCounters();
#elif defined(__EMSCRIPTEN__)
// A single macrotask hop: the browser renders, delivers input and settles
// promises, then this stack resumes (no setTimeout clamp).
inline void Yield() { mkw_host_yield(0); }

// Waits until `deadline`. Whole milliseconds sleep on a browser timer (unclamped, see
// mkw_host_yield; it fires at or a little after the requested time); only the sub-millisecond
// remainder is covered by macrotask hops. Hopping through whole waits instead spun the idle
// scheduler at ~150k hops/s, a full core whenever the game had nothing to do.
template <typename Clock, typename Duration>
inline void SleepUntil(std::chrono::time_point<Clock, Duration> deadline) {
    using namespace std::chrono;
    for (auto now = Clock::now(); now < deadline; now = Clock::now()) {
        const auto wholeMs = duration_cast<milliseconds>(deadline - now).count();
        mkw_host_yield(wholeMs >= 1 ? static_cast<int>(wholeMs) : 0);
    }
}
// True while document.visibilityState is "hidden" (background tab, minimized window).
inline bool PageHidden() { return mkw_host_page_hidden() != 0; }

// If the page is hidden, suspends this stack (JSPI) until it is visible again and returns how long
// that took; returns zero at once otherwise. Nothing else in the module runs meanwhile: every guest
// thread is a context on this one stack, so the whole console is paused. The caller must restart
// whatever it paces against the host clock (VI retraces, audio output).
inline std::chrono::steady_clock::duration WaitWhilePageHidden() {
    if (!PageHidden()) {
        return {};
    }
    const auto start = std::chrono::steady_clock::now();
    mkw_host_wait_visible();
    const auto parked = std::chrono::steady_clock::now() - start;
    ParkedNanosCounter().fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(parked).count(), std::memory_order_relaxed);
    return parked;
}

// Counts a presented frame for the page (Module.mkwFramesPresented: diagnostics and tests).
inline void NoteFramePresented() { mkw_host_note_frame(); }
#else
inline void Yield() {}
inline bool PageHidden() { return false; }
inline std::chrono::steady_clock::duration WaitWhilePageHidden() { return {}; }
inline void NoteFramePresented() {}

template <typename Clock, typename Duration>
inline void SleepUntil(std::chrono::time_point<Clock, Duration> deadline) {
    std::this_thread::sleep_until(deadline);
}
#endif

template <typename Rep, typename Period>
inline void SleepFor(std::chrono::duration<Rep, Period> duration) {
    SleepUntil(std::chrono::steady_clock::now() + duration);
}

} // namespace WebHost
