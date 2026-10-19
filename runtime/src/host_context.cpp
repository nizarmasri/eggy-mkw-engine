#include "host_context.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);
#elif defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
// [Eggy] "Baton" browser build (MKW_WEB_THREADS=baton): no JSPI, every context is a pthread.
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "platform/web_host.h"

// web/js/baton/library_mkw_baton.js. Runs on the browser main thread: keeps *hidden equal to
// (document.visibilityState == "hidden") and futex-wakes it on every change.
extern "C" void mkw_host_watch_visibility(int32_t* hidden);
// Counts a presented frame for the page (Module.mkwFramesPresented), asynchronously.
extern "C" void mkw_host_note_frame(void);

namespace {
// See WebHost::BatonCounter (platform/web_host.h).
std::atomic<uint32_t> g_batonCounters[WebHost::kBatonCounterCount];
std::atomic<uint32_t> g_liveContexts{0};
std::atomic<uint32_t> g_parkedForGood{0};
std::atomic<int32_t> g_pageHidden{0};

void AddBatonCounter(WebHost::BatonCounter counter, uint32_t value)
{
    g_batonCounters[counter].fetch_add(value, std::memory_order_relaxed);
}
} // namespace

// For the JS side (slots 0 and 1, and the main thread's heartbeat line).
extern "C" EMSCRIPTEN_KEEPALIVE std::atomic<uint32_t>* mkw_baton_counters()
{
    return g_batonCounters;
}
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/stack.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// JS side lives in web/js/library_mkw_host.js. mkw_host_context_switch is a
// JSPI-suspending import; mkw_host_context_entry is a JSPI (promising) export.
extern "C" void mkw_host_context_switch(void* source, void* destination);
extern "C" void _emscripten_stack_restore(uintptr_t stackPointer);
#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))
#include <libco.h>

#include <cstdlib>
#include <unordered_map>
#else
#error "HostContext needs a supported cooperative-context backend"
#endif

namespace HostContext {

#if defined(_WIN32)

namespace {
thread_local bool g_convertedScheduler = false;
}

bool InitializeScheduler(Handle* scheduler)
{
    void* context = ConvertThreadToFiber(nullptr);
    g_convertedScheduler = context != nullptr;
    if (!context) {
        context = GetCurrentFiber();
    }
    *scheduler = context;
    return context != nullptr;
}

void ShutdownScheduler(Handle scheduler)
{
    if (scheduler && g_convertedScheduler) {
        ConvertFiberToThread();
    }
    g_convertedScheduler = false;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    return CreateFiber(stackSize, entry, argument);
}

void Destroy(Handle context)
{
    if (context) {
        DeleteFiber(context);
    }
}

bool IsCurrent(Handle context)
{
    return context != nullptr && GetCurrentFiber() == context;
}

void Switch(Handle target)
{
    SwitchToFiber(target);
}

#elif defined(__APPLE__) && defined(__aarch64__)

namespace {
struct Context {
    void* savedStackPointer = nullptr;
    void* stack = nullptr;
    std::size_t stackSize = 0;
};

// Guest scheduling is confined to the initialized main host thread. Keeping
// this as ordinary process state also avoids relying on Darwin TLS internals
// while executing on a manually managed stack.
Context* g_current = nullptr;
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    const std::size_t guardSize = static_cast<std::size_t>(getpagesize());
    const std::size_t totalSize = stackSize + guardSize;
    context->stack = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (context->stack == MAP_FAILED) {
        delete context;
        return nullptr;
    }
    // Fault on stack overflow instead of corrupting the preceding mapping.
    if (mprotect(context->stack, guardSize, PROT_NONE) != 0) {
        munmap(context->stack, totalSize);
        delete context;
        return nullptr;
    }
    context->stackSize = totalSize;

    auto* stackTop = static_cast<char*>(context->stack) + totalSize;
    context->savedStackPointer = mkw_co_init(stackTop, entry, argument);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    if (nativeContext->stack) {
        munmap(nativeContext->stack, nativeContext->stackSize);
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    mkw_co_switch(&destination->savedStackPointer, &source->savedStackPointer);
    g_current = source;
}

#elif defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)

// [Eggy] The baton build, for browsers without JSPI (Safari before iOS 27).
//
// Every context is a real pthread (a Web Worker), and exactly one of them holds the baton at a
// time: Switch() hands it to the destination (store + futex wake on the destination's word) and
// waits on its own word until somebody hands it back. The scheduler context is the thread that
// runs main(), which -sPROXY_TO_PTHREAD puts on a worker too, so the browser main thread stays
// free for input, audio, disc fetches and WebGPU (aurora proxies its frame work there).
//
// Thread-local state. Natively and under JSPI every guest context is a fiber on one host
// thread, so the runtime's many `thread_local`s (the current CpuContext, the FP control mirror,
// the SEH jump target, the dispatch memos...) are shared by all guest threads. Real pthreads
// would each get their own copy and quietly change that, so a context adopts the scheduler's TLS
// block (__tls_base) the first time it receives the baton and keeps it. Only the baton holder
// runs, and the futex hand-off orders every access, exactly as a fiber switch would.
//
// A destroyed context is never resumed. One that never ran exits at once; one that is parked
// somewhere in guest code stays parked for good (a fiber is freed without unwinding, and
// unwinding a guest stack here would run cleanups against live runtime state). The number of
// such parked workers is logged; MKW creates its threads once.
namespace {
__asm__(".globaltype __tls_base, i32\n");

void SetTlsBase(void* base)
{
    __asm__ volatile("local.get %0\n"
                     "global.set __tls_base\n"
                     :
                     : "r"(base)
                     : "memory");
}

void* TlsBase()
{
    return __builtin_wasm_tls_base();
}

struct Context {
    // 1 while this context holds the baton (futex word).
    std::atomic<int32_t> run{0};
    // Set by Destroy() before waking a context that never ran.
    std::atomic<int32_t> dead{0};
    Entry entry = nullptr;
    void* argument = nullptr;
    pthread_t thread{};
    uint32_t id = 0;
    bool started = false;
    // emscripten_get_now() when the baton was handed to this context (diagnostics).
    double handedAt = 0.0;
};

// Written only by the baton holder.
Context* g_current = nullptr;
void* g_sharedTls = nullptr;
uint32_t g_nextId = 0;

void WaitForBaton(Context* context)
{
    while (context->run.load(std::memory_order_acquire) == 0) {
        emscripten_futex_wait(&context->run, 0, INFINITY);
    }
}

// How long the hand-off took to reach this context: the price of one baton pass.
void NoteResumed(const Context* context)
{
    const double waitedMs = emscripten_get_now() - context->handedAt;
    AddBatonCounter(WebHost::kBatonHandOffMicros, static_cast<uint32_t>(std::max(0.0, waitedMs) * 1000.0));
}

void* ContextThreadMain(void* argument)
{
    auto* context = static_cast<Context*>(argument);
    WaitForBaton(context);
    if (context->dead.load(std::memory_order_acquire)) {
        // Destroyed before it ever ran: nothing of the guest is on this stack.
        delete context;
        return nullptr;
    }
    NoteResumed(context);
    SetTlsBase(g_sharedTls);
    g_current = context;
    context->started = true;
    std::printf("[ios26] baton: context #%u is running on its own worker (shared TLS)\n", context->id);
    context->entry(context->argument);
    // A guest fiber always switches away for good instead of returning (the same contract as
    // Win32 fibers); returning would silently strand the context that should run next.
    std::fprintf(stderr, "HostContext: context #%u returned from its entry\n", context->id);
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->run.store(1, std::memory_order_relaxed);
    context->started = true;
    context->id = g_nextId++;
    g_sharedTls = TlsBase();
    g_current = context;
    *scheduler = context;
    g_liveContexts.fetch_add(1, std::memory_order_relaxed);
    mkw_host_watch_visibility(reinterpret_cast<int32_t*>(&g_pageHidden));
    std::printf("[ios26] baton: scheduler context #%u on the main() pthread (no JSPI; guest threads "
                "are pthreads handing one baton around)\n", context->id);
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->id = g_nextId++;

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, std::max<std::size_t>(stackSize, 64 * 1024));
    const int error = pthread_create(&context->thread, &attributes, ContextThreadMain, context);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        std::fprintf(stderr, "[ios26] baton: pthread_create failed for context #%u (error %d)\n",
                     context->id, error);
        delete context;
        return nullptr;
    }
    const uint32_t live = g_liveContexts.fetch_add(1, std::memory_order_relaxed) + 1;
    std::printf("[ios26] baton: created context #%u as a pthread (%u live)\n", context->id, live);
    return context;
}

void Destroy(Handle handle)
{
    auto* context = static_cast<Context*>(handle);
    if (!context) {
        return;
    }
    g_liveContexts.fetch_sub(1, std::memory_order_relaxed);
    if (!context->started) {
        // Wake it so its worker returns to the pool; it frees the context itself.
        context->dead.store(1, std::memory_order_release);
        context->run.store(1, std::memory_order_release);
        emscripten_futex_wake(&context->run, 1);
        return;
    }
    // Parked inside guest code: leave it parked (see the comment at the top of this backend).
    const uint32_t parked = g_parkedForGood.fetch_add(1, std::memory_order_relaxed) + 1;
    std::printf("[ios26] baton: context #%u destroyed; its worker stays parked (%u so far)\n",
                context->id, parked);
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    AddBatonCounter(WebHost::kBatonHandOffs, 1);
    g_current = destination;
    destination->handedAt = emscripten_get_now();
    // Drop our own word before handing over: the destination may hand the baton straight back,
    // and its store must not be overwritten by ours.
    source->run.store(0, std::memory_order_relaxed);
    destination->run.store(1, std::memory_order_release);
    emscripten_futex_wake(&destination->run, 1);
    WaitForBaton(source);
    NoteResumed(source);
    g_current = source;
}

#elif defined(__EMSCRIPTEN__)

// WebAssembly has no way to switch native stacks, and Emscripten's own fibers
// are built on Asyncify (which would instrument every translated function).
// Instead every context is its own JSPI-suspendable call stack:
//   * the scheduler context is the stack running main();
//   * a created context starts as a call into the promising export
//     mkw_host_context_entry, issued from a microtask once its creator has
//     suspended;
//   * Switch() suspends the caller on a promise that the JS side resolves when
//     some later Switch() targets it again.
// The wasm value stacks are owned by the engine, but the C shadow stack
// (__stack_pointer) is a single global, so each context owns a malloc'd shadow
// stack and Switch() reinstates the caller's pointer after every resume.
namespace {
struct Context {
    Entry entry = nullptr;
    void* argument = nullptr;
    uint8_t* stack = nullptr;       // nullptr for the scheduler (Emscripten's main stack)
    std::size_t stackSize = 0;
    uintptr_t stackBase = 0;        // highest address (stack grows down)
    uintptr_t stackEnd = 0;         // lowest address
    uintptr_t savedStackPointer = 0;
};

Context* g_current = nullptr;

void InstallStack(const Context* context, uintptr_t stackPointer)
{
    emscripten_stack_set_limits(reinterpret_cast<void*>(context->stackBase),
                                reinterpret_cast<void*>(context->stackEnd));
    _emscripten_stack_restore(stackPointer);
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void mkw_host_context_entry(void* handle)
{
    auto* context = static_cast<Context*>(handle);
    g_current = context;
    InstallStack(context, context->stackBase);
    context->entry(context->argument);
    // A guest fiber always switches away for good instead of returning (the
    // same contract as Win32 fibers); returning would silently strand the
    // context that should run next.
    std::fprintf(stderr, "HostContext: context %p returned from its entry\n", handle);
    std::abort();
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->stackBase = emscripten_stack_get_base();
    context->stackEnd = emscripten_stack_get_end();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->stackSize = (stackSize + 15u) & ~std::size_t{15};
    context->stack = static_cast<uint8_t*>(std::aligned_alloc(16, context->stackSize));
    if (!context->stack) {
        delete context;
        return nullptr;
    }
    context->stackEnd = reinterpret_cast<uintptr_t>(context->stack);
    context->stackBase = context->stackEnd + context->stackSize;
    return context;
}

void Destroy(Handle handle)
{
    auto* context = static_cast<Context*>(handle);
    if (!context) {
        return;
    }
    // A destroyed context is never resumed again; its suspended engine stack
    // is released with the JS resolver that referenced it.
    EM_ASM({ Module.mkwHostContextForget($0); }, context);
    std::free(context->stack);
    delete context;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    source->savedStackPointer = emscripten_stack_get_current();
    g_current = destination;
    mkw_host_context_switch(source, destination);
    // Resumed: whichever context ran last left its own shadow-stack pointer in
    // the global, so reinstate ours before this frame calls anything.
    g_current = source;
    InstallStack(source, source->savedStackPointer);
}

#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))

namespace {
struct Context {
    cothread_t native = nullptr;
    Entry entry = nullptr;
    void* argument = nullptr;
    bool ownsNative = false;
};

thread_local Context* g_current = nullptr;
thread_local std::unordered_map<cothread_t, Context*> g_contexts;

void ContextEntry()
{
    const auto found = g_contexts.find(co_active());
    if (found == g_contexts.end() || !found->second || !found->second->entry) {
        std::abort();
    }

    Context* context = found->second;
    g_current = context;
    context->entry(context->argument);

    // A guest fiber must return through FiberProc's scheduler handoff. There
    // is no valid native caller to return to from libco's entry trampoline.
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->native = co_active();
    if (!context->native) {
        delete context;
        return false;
    }

    g_current = context;
    g_contexts.emplace(context->native, context);
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (!context) {
        return;
    }

    g_contexts.erase(context->native);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->native = co_create(static_cast<unsigned int>(stackSize), ContextEntry);
    context->ownsNative = context->native != nullptr;
    if (!context->native) {
        delete context;
        return nullptr;
    }

    g_contexts.emplace(context->native, context);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }

    g_contexts.erase(nativeContext->native);
    if (nativeContext->ownsNative) {
        co_delete(nativeContext->native);
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    co_switch(destination->native);
    g_current = source;
}

#endif

} // namespace HostContext

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
// [Eggy] WebHost for the baton build (declared in platform/web_host.h).
namespace WebHost {

std::atomic<uint32_t>* BatonCounters()
{
    return g_batonCounters;
}

bool PageHidden()
{
    return g_pageHidden.load(std::memory_order_acquire) != 0;
}

std::chrono::steady_clock::duration WaitWhilePageHidden()
{
    if (!PageHidden()) {
        return {};
    }
    const auto start = std::chrono::steady_clock::now();
    while (g_pageHidden.load(std::memory_order_acquire) != 0) {
        emscripten_futex_wait(&g_pageHidden, 1, INFINITY);
    }
    const auto parked = std::chrono::steady_clock::now() - start;
    ParkedNanosCounter().fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(parked).count(), std::memory_order_relaxed);
    return parked;
}

// Called by the guest thread that presents, once per frame. Every 3 s it logs what the baton
// costs: hand-offs and their wake latency, and the round trips to the browser main thread.
void NoteFramePresented()
{
    AddBatonCounter(kBatonFrames, 1);
    mkw_host_note_frame();

    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_windowStart = Clock::now();
    static uint32_t s_last[kBatonCounterCount] = {};
    static uint32_t s_reports = 0;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - s_windowStart).count();
    if (seconds < 3.0) {
        return;
    }
    uint32_t delta[kBatonCounterCount];
    for (uint32_t i = 0; i < kBatonCounterCount; ++i) {
        const uint32_t value = g_batonCounters[i].load(std::memory_order_relaxed);
        delta[i] = value - s_last[i];
        s_last[i] = value;
    }
    const auto per = [](uint32_t total, uint32_t count) {
        return count != 0 ? static_cast<double>(total) / static_cast<double>(count) : 0.0;
    };
    if (s_reports < 40 || (s_reports % 20) == 0) {
        const double blockedShare = (static_cast<double>(delta[kBatonJsCallMicros]) +
                                     static_cast<double>(delta[kBatonMainCallMicros])) / (seconds * 1e6);
        std::printf("[ios26] baton: %.1f s: %u frames (%.1f/s) | %u hand-offs (%.1f/frame, %.0f us to "
                    "wake) | %u JS calls on the main thread (%.1f/frame, %.0f us each) | %u aurora "
                    "calls on the main thread (%.1f/frame, %.2f ms each) | guest blocked on the main "
                    "thread %.0f%% of the time | %u live contexts, %u parked for good\n",
                    seconds, delta[kBatonFrames], delta[kBatonFrames] / seconds, delta[kBatonHandOffs],
                    per(delta[kBatonHandOffs], delta[kBatonFrames]),
                    per(delta[kBatonHandOffMicros], delta[kBatonHandOffs]), delta[kBatonJsCalls],
                    per(delta[kBatonJsCalls], delta[kBatonFrames]),
                    per(delta[kBatonJsCallMicros], delta[kBatonJsCalls]), delta[kBatonMainCalls],
                    per(delta[kBatonMainCalls], delta[kBatonFrames]),
                    per(delta[kBatonMainCallMicros], delta[kBatonMainCalls]) / 1000.0,
                    blockedShare * 100.0, g_liveContexts.load(std::memory_order_relaxed),
                    g_parkedForGood.load(std::memory_order_relaxed));
    }
    ++s_reports;
    s_windowStart = now;
}

} // namespace WebHost
#endif
