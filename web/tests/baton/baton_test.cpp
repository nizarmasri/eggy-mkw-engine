// The baton build's machinery without the game (EGGY.md, "iOS 26: the baton build"): the
// pthread HostContext backend (runtime/src/host_context.cpp), aurora split between a worker and
// the browser main thread (runtime/src/platform/web/baton_main_thread.cpp), and the JS proxying
// (web/js/baton/library_mkw_baton.js), in a page that has no JSPI at all.
//
// main() runs on a worker (-sPROXY_TO_PTHREAD) and is the scheduler context, as in the runtime.
// It initializes aurora there, then hands the baton to a "renderer" context that presents frames
// in a solid colour and ping-pongs with a "worker" context that checks the guest-visible
// invariants. The page (run_baton_test.mjs) checks the colour on screen and the log.
//
// Prints "BATON-TEST PASS" once everything held for kFrames frames, or "BATON-TEST FAIL: why",
// then keeps presenting so the page can be looked at.
#include "host_context.h"
#include "platform/web_host.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

// web/tests/baton/library_baton_test.js: an `__async` import proxied 'sync', i.e. a promise the
// worker blocks on (the mechanism behind every disc read of the baton build), and the same
// through an `async` function.
extern "C" int baton_test_async_on_main(int ms);
extern "C" int baton_test_async_function(int x);
// Eggy.wake() on the main thread, as the page's load()/go()/abort() do.
extern "C" void baton_test_wake_eggy(void);

// The product's own imports (web/js/*.js, linked here as in the product), each proxied to the
// main thread by web/js/baton/library_mkw_baton.js. The test calls the ones that are safe without
// a disc or an Eggy page and keeps the rest linked, so their proxies are generated too.
extern "C" {
char* mkw_dvd_load_manifest(int mount);
int32_t mkw_dvd_read(int mount, const char* relPath, double offset, uint32_t length, uint8_t* dest);
char* mkw_dvd_mod_mount_name(void);
int mkw_storage_flush(void);
int mkw_storage_is_persistent(void);
int mkw_storage_is_readonly(void);
void mkw_input_attach(void* state, uint32_t size, void* touch);
void mkw_input_describe(const char* json);
void mkw_host_report_fatal(const char* message);
int eggy_enabled(void);
void eggy_attach(int32_t* block, int bytes);
void eggy_wait(int ms);
int eggy_pads(int frame, uint8_t* dest);
void eggy_frame_done(int frame);
void eggy_state(int state);
void eggy_report_ready(void);
}

namespace {

constexpr int kFrames = 150;
constexpr int kHandOffsPerFrame = 10;
// web/tests/baton/run_baton_test.mjs expects exactly this many "[baton-test] console N" lines.
constexpr int kConsoleLines = 300;
// What GXSetCopyClear paints; the page looks for it on screen.
constexpr GXColor kClear{200, 40, 160, 255};

HostContext::Handle g_scheduler = nullptr;
HostContext::Handle g_renderer = nullptr;
HostContext::Handle g_worker = nullptr;
pthread_t g_rendererThread{};

// Natively and under JSPI every guest context is a fiber on one thread, so the runtime's
// thread_locals are shared by all guest threads; the baton build keeps that (shared TLS block).
thread_local int t_turn = 0;
thread_local std::string t_text;

int g_rounds = 0;
// Never set: keeps the imports the test does not call in the link.
volatile bool g_never = false;

[[noreturn]] void Fail(const std::string& why)
{
    std::printf("BATON-TEST FAIL: %s\n", why.c_str());
    std::fflush(stdout);
    std::abort();
}

int Recurse(int depth, volatile char* sink)
{
    char frame[256];
    frame[depth & 255] = static_cast<char>(depth);
    sink[0] = frame[depth & 255];
    return depth == 0 ? 0 : 1 + Recurse(depth - 1, sink);
}

void LogCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
    std::FILE* out = level >= LOG_WARNING ? stderr : stdout;
    std::fprintf(out, "[aurora] [%d] [%s] %.*s\n", static_cast<int>(level), module ? module : "",
                 static_cast<int>(len), message ? message : "");
}

// The other side of every hand-off: checks what a guest thread must see after a switch.
void WorkerEntry(void*)
{
    const pthread_t self = pthread_self();
    if (emscripten_is_main_browser_thread()) Fail("a context runs on the browser main thread");
    for (int round = 0;; ++round) {
        if (pthread_equal(self, g_rendererThread)) Fail("two contexts share one worker");
        if (t_turn != 2 * round + 1) Fail("thread_local not shared between contexts (turn " + std::to_string(t_turn) + ")");
        if (t_text != "renderer " + std::to_string(round)) Fail("thread_local string not shared");
        ++t_turn;
        t_text = "worker " + std::to_string(round);
        volatile char sink[1];
        if (Recurse(500, sink) != 500) Fail("recursion");
        try {
            throw std::runtime_error("inside a context");
        } catch (const std::exception& e) {
            if (std::string(e.what()) != "inside a context") Fail("exception");
        }
        HostContext::Switch(g_renderer);
    }
}

void HandOff()
{
    t_turn = 2 * g_rounds + 1;
    t_text = "renderer " + std::to_string(g_rounds);
    HostContext::Switch(g_worker);
    if (t_turn != 2 * g_rounds + 2 || t_text != "worker " + std::to_string(g_rounds)) {
        Fail("the worker's thread_local writes are not visible after the hand-off");
    }
    ++g_rounds;
}

bool PresentFrame()
{
    for (const AuroraEvent* event = aurora_update(); event != nullptr && event->type != AURORA_NONE; ++event) {
    }
    if (!aurora_begin_frame()) return false;
    GXSetCopyClear(kClear, GX_MAX_Z24);
    aurora_end_frame();
    WebHost::NoteFramePresented();
    return true;
}

void RendererEntry(void*)
{
    g_rendererThread = pthread_self();

    // An __async import proxied to the main thread blocks this worker until its promise settles.
    const double asyncStart = emscripten_get_now();
    const int answer = baton_test_async_on_main(40);
    const double asyncMs = emscripten_get_now() - asyncStart;
    if (answer != 7) Fail("the proxied promise returned " + std::to_string(answer) + " instead of 7 (not run on the main thread?)");
    if (asyncMs < 35.0) Fail("the proxied promise did not block the worker (" + std::to_string(asyncMs) + " ms)");
    std::printf("[baton-test] a promise on the main thread blocked this worker for %.1f ms (asked 40)\n", asyncMs);
    const int plus = baton_test_async_function(7);
    if (plus != 8) Fail("the proxied async function returned " + std::to_string(plus) + " instead of 8");

    // The product's imports, on the page's side.
    if (eggy_enabled() != 0) Fail("eggy_enabled() is not 0 on a page without Module.eggy");
    const double waitStart = emscripten_get_now();
    eggy_wait(30); // resolves through Eggy.wake() on the main thread when the 30 ms pass
    const double waitMs = emscripten_get_now() - waitStart;
    if (waitMs < 25.0) Fail("eggy_wait(30) returned after " + std::to_string(waitMs) + " ms");
    // A wake that reaches the page before the engine's wait does is not lost (library_mkw_baton.js):
    // under the baton the page runs while the engine does, so Eggy's load() can land between the
    // engine reading its mailboxes and parking. The wait must return at once, not after 2 s.
    baton_test_wake_eggy();
    const double lostStart = emscripten_get_now();
    eggy_wait(2000);
    const double lostMs = emscripten_get_now() - lostStart;
    if (lostMs > 1000.0) Fail("a wake before eggy_wait was lost (the wait ran " + std::to_string(lostMs) + " ms)");
    // ...and once it is used up, the next wait parks again until its timeout.
    const double parkStart = emscripten_get_now();
    eggy_wait(30);
    const double parkMs = emscripten_get_now() - parkStart;
    if (parkMs < 25.0) Fail("eggy_wait(30) after a kept wake returned after " + std::to_string(parkMs) + " ms");
    std::printf("[baton-test] a wake before eggy_wait is kept: the wait returned in %.1f ms, the next one "
                "parked %.1f ms\n", lostMs, parkMs);
    const int persistent = mkw_storage_is_persistent();
    const int readOnly = mkw_storage_is_readonly();
    char* modName = mkw_dvd_mod_mount_name();
    std::printf("[baton-test] product imports on the main thread: eggy_enabled 0, eggy_wait(30) %.1f ms, "
                "storage persistent %d read-only %d, mod mount '%s'\n",
                waitMs, persistent, readOnly, modName ? modName : "(none)");
    std::free(modName);

    // A worker's console output is handed to the main thread without waiting for it (the
    // fd_write import is wrapped in baton_main_thread.cpp): these lines cost far less than as many
    // round trips (1-4 ms each on CI), and the page checks that they all arrived, in order.
    const double consoleStart = emscripten_get_now();
    for (int i = 0; i < kConsoleLines; ++i) {
        std::printf("[baton-test] console %d\n", i);
    }
    const double consoleMs = emscripten_get_now() - consoleStart;
    std::printf("[baton-test] %d console lines from a worker took %.1f ms\n", kConsoleLines, consoleMs);
    if (consoleMs > 150.0) {
        Fail("a worker's console output waited for the main thread (" + std::to_string(consoleMs) + " ms for " +
             std::to_string(kConsoleLines) + " lines)");
    }
    if (g_never) {
        uint8_t bytes[16] = {};
        int32_t block[4] = {};
        std::free(mkw_dvd_load_manifest(0));
        mkw_dvd_read(0, "sys/main.dol", 0.0, 16, bytes);
        mkw_storage_flush();
        mkw_input_attach(bytes, sizeof bytes, bytes);
        mkw_input_describe("{}");
        mkw_host_report_fatal("never");
        eggy_attach(block, sizeof block);
        eggy_pads(0, bytes);
        eggy_frame_done(0);
        eggy_state(0);
        eggy_report_ready();
    }

    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    int skipped = 0;
    while (frames < kFrames) {
        for (int i = 0; i < kHandOffsPerFrame; ++i) HandOff();
        if (PresentFrame()) {
            ++frames;
        } else if (++skipped > 600) {
            Fail("aurora_begin_frame kept failing");
        }
        WebHost::SleepFor(std::chrono::milliseconds(16));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::atomic<uint32_t>* counters = WebHost::BatonCounters();
    const uint32_t handOffs = counters[WebHost::kBatonHandOffs].load();
    const uint32_t handOffMicros = counters[WebHost::kBatonHandOffMicros].load();
    const uint32_t mainCalls = counters[WebHost::kBatonMainCalls].load();
    const uint32_t mainMicros = counters[WebHost::kBatonMainCallMicros].load();
    const uint32_t jsCalls = counters[WebHost::kBatonJsCalls].load();
    const uint32_t jsMicros = counters[WebHost::kBatonJsCallMicros].load();
    std::printf("[baton-test] %d frames in %.1f s (%d not begun), %d rounds; %u hand-offs, %.1f us each to wake; "
                "%u aurora calls on the main thread, %.2f ms each; %u JS calls proxied, %.0f us each\n",
                frames, seconds, skipped, g_rounds, handOffs, handOffs ? double(handOffMicros) / handOffs : 0.0,
                mainCalls, mainCalls ? double(mainMicros) / mainCalls / 1000.0 : 0.0, jsCalls,
                jsCalls ? double(jsMicros) / jsCalls : 0.0);
    std::printf("BATON-TEST PASS\n");
    std::fflush(stdout);
    for (;;) {
        HandOff();
        PresentFrame();
        WebHost::SleepFor(std::chrono::milliseconds(16));
    }
}

} // namespace

int main()
{
    std::printf("[baton-test] main() on %s\n",
                emscripten_is_main_browser_thread() ? "the browser main thread (wrong)" : "a worker");
    if (emscripten_is_main_browser_thread()) Fail("main() must run on a worker (-sPROXY_TO_PTHREAD)");
    if (!HostContext::InitializeScheduler(&g_scheduler)) Fail("InitializeScheduler");

    AuroraConfig config{};
    config.appName = "baton-test";
    config.logCallback = &LogCallback;
    config.logLevel = LOG_INFO;
    config.desiredBackend = BACKEND_AUTO;
    config.windowWidth = 640;
    config.windowHeight = 480;
    const AuroraInfo info = aurora_initialize(0, nullptr, &config);
    if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS) {
        Fail(std::string("aurora_initialize: ") + (info.initializationError ? info.initializationError : "?"));
    }
    std::printf("[baton-test] aurora initialized on a worker: backend %d, framebuffer %ux%u\n",
                static_cast<int>(info.backend), info.windowSize.fb_width, info.windowSize.fb_height);

    // A context destroyed before it ever ran gives its worker back.
    HostContext::Destroy(HostContext::Create(256 * 1024, [](void*) { Fail("a destroyed context ran"); }, nullptr));

    g_worker = HostContext::Create(1024 * 1024, WorkerEntry, nullptr);
    g_renderer = HostContext::Create(1024 * 1024, RendererEntry, nullptr);
    if (!g_worker || !g_renderer) Fail("HostContext::Create");
    HostContext::Switch(g_renderer);
    Fail("the scheduler was resumed");
}
