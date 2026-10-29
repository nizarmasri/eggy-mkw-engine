// The render thread's machinery without the game (EGGY.md, "The render thread"): a JSPI module
// built with pthreads, aurora and its libraries compiled with -pthread, the canvas transferred to
// the render thread (aurora-main/lib/webgpu/web_render_thread.cpp) and every emdawnwebgpu import
// proxied there (web/js/render/library_mkw_render.js).
//
// main() runs on the browser main thread under JSPI, as the game's guest does. It records GX
// draws each frame like the game: a solid clear, a vertex-coloured quad on the left, and a
// textured quad on the right whose 4x4 texture is uploaded from this thread (texture, view and
// bind group creation are proxied to the render thread); and each frame reads a depth back with
// GXPeekZ. aurora_end_frame hands the frame to the render thread, which seals, encodes, submits
// and presents it while this thread waits for the next animation frame.
//
// Prints "RENDER-THREAD-TEST PASS" with the measured costs once kFrames frames were presented on
// the render thread (or, with ?renderThread=0, on this thread), or "RENDER-THREAD-TEST FAIL: why",
// then keeps drawing so the page can be looked at (run_render_thread_test.mjs checks the pixels).
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/web_render_thread.h>
#include <dolphin/gx.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

EM_ASYNC_JS(void, wait_for_animation_frame, (), {
  await new Promise((resolve) => requestAnimationFrame(resolve));
});

EM_JS(int, render_thread_expected, (), {
  return /[?&]renderThread=0(&|$)/.test(location.search) ? 0 : 1;
});

namespace {

constexpr int kFrames = 120;
constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 480;
// What the page looks for (run_render_thread_test.mjs): clear, left quad, right (textured) quad.
constexpr GXColor kClear{30, 60, 200, 255};
constexpr GXColor kLeft{220, 40, 40, 255};
constexpr GXColor kTexel{40, 200, 90, 255};

alignas(32) unsigned char g_fifo[256 * 1024];
alignas(32) unsigned char g_texture[64]; // 4x4 RGBA8: one tile, AR plane then GB plane
GXTexObj g_texObj;

[[noreturn]] void Fail(const std::string& why) {
  std::printf("RENDER-THREAD-TEST FAIL: %s\n", why.c_str());
  std::fflush(stdout);
  std::abort();
}

void LogCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  std::FILE* out = level >= LOG_WARNING ? stderr : stdout;
  std::fprintf(out, "[aurora] [%d] [%s] %.*s\n", static_cast<int>(level), module ? module : "",
               static_cast<int>(len), message ? message : "");
  if (level == LOG_FATAL) {
    std::fflush(out);
    std::abort();
  }
}

void SetupCommon() {
  f32 proj[4][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};
  f32 modelView[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  GXLoadPosMtxImm(modelView, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetViewport(0.f, 0.f, static_cast<f32>(kEfbWidth), static_cast<f32>(kEfbHeight), 0.f, 1.f);
  GXSetScissor(0, 0, kEfbWidth, kEfbHeight);
  GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  GXSetCullMode(GX_CULL_NONE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetColorUpdate(GX_TRUE);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTevStages(1);
}

// Left half: a vertex-coloured quad (no texture).
void DrawColouredQuad() {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumTexGens(0);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  const float z = -0.5f;
  GXPosition3f32(-0.9f, 0.8f, z);
  GXColor4u8(kLeft.r, kLeft.g, kLeft.b, kLeft.a);
  GXPosition3f32(-0.1f, 0.8f, z);
  GXColor4u8(kLeft.r, kLeft.g, kLeft.b, kLeft.a);
  GXPosition3f32(-0.1f, -0.8f, z);
  GXColor4u8(kLeft.r, kLeft.g, kLeft.b, kLeft.a);
  GXPosition3f32(-0.9f, -0.8f, z);
  GXColor4u8(kLeft.r, kLeft.g, kLeft.b, kLeft.a);
  GXEnd();
}

// Right half: a quad sampling the 4x4 texture (uploaded by aurora from this thread's memory).
void DrawTexturedQuad() {
  GXLoadTexObj(&g_texObj, GX_TEXMAP0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  GXBegin(GX_QUADS, GX_VTXFMT1, 4);
  const float z = -0.5f;
  GXPosition3f32(0.1f, 0.8f, z);
  GXTexCoord2f32(0.f, 0.f);
  GXPosition3f32(0.9f, 0.8f, z);
  GXTexCoord2f32(1.f, 0.f);
  GXPosition3f32(0.9f, -0.8f, z);
  GXTexCoord2f32(1.f, 1.f);
  GXPosition3f32(0.1f, -0.8f, z);
  GXTexCoord2f32(0.f, 1.f);
  GXEnd();
}

} // namespace

int main(int argc, char* argv[]) {
  const bool expectThread = render_thread_expected() != 0;
  std::printf("[render-test] main() on %s, render thread %s\n",
              emscripten_is_main_browser_thread() ? "the browser main thread" : "a worker (wrong)",
              expectThread ? "expected" : "turned off (?renderThread=0)");
  if (!emscripten_is_main_browser_thread()) Fail("main() must run on the browser main thread (JSPI)");

  for (int i = 0; i < 16; ++i) {
    g_texture[2 * i] = kTexel.a;
    g_texture[2 * i + 1] = kTexel.r;
    g_texture[32 + 2 * i] = kTexel.g;
    g_texture[32 + 2 * i + 1] = kTexel.b;
  }

  AuroraConfig config{};
  config.appName = "render-thread-test";
  config.logCallback = &LogCallback;
  config.logLevel = LOG_INFO;
  config.desiredBackend = BACKEND_AUTO;
  config.windowWidth = kEfbWidth;
  config.windowHeight = kEfbHeight;
  const AuroraInfo info = aurora_initialize(argc, argv, &config);
  if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS) {
    Fail(std::string("aurora_initialize: ") + (info.initializationError ? info.initializationError : "?"));
  }
  const bool active = aurora_render_thread_active();
  std::printf("[render-test] aurora initialized: backend %d, framebuffer %ux%u, render thread %s\n",
              static_cast<int>(info.backend), info.windowSize.fb_width, info.windowSize.fb_height,
              active ? "active" : "off");
  if (active != expectThread) Fail(std::string("the render thread is ") + (active ? "on" : "off"));
  GXInit(g_fifo, sizeof g_fifo);
  GXInitTexObj(&g_texObj, g_texture, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&g_texObj, GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);

  int frames = 0;
  int notBegun = 0;
  double recordMs = 0.0;
  double endMs = 0.0;
  double beginMs = 0.0;
  bool reported = false;
  const double started = emscripten_get_now();
  for (;;) {
    for (const AuroraEvent* event = aurora_update(); event != nullptr && event->type != AURORA_NONE; ++event) {
    }
    const double t0 = emscripten_get_now();
    if (!aurora_begin_frame()) {
      if (++notBegun > 600) Fail("aurora_begin_frame kept failing");
      wait_for_animation_frame();
      continue;
    }
    const double t1 = emscripten_get_now();
    GXSetCopyClear(kClear, GX_MAX_Z24);
    SetupCommon();
    DrawColouredQuad();
    DrawTexturedQuad();
    u32 depth = 0;
    GXPeekZ(kEfbWidth / 4, kEfbHeight / 2, &depth);
    const double t2 = emscripten_get_now();
    aurora_end_frame();
    const double t3 = emscripten_get_now();
    ++frames;
    if (frames > 10) {
      beginMs += t1 - t0;
      recordMs += t2 - t1;
      endMs += t3 - t2;
    }
    if (!reported && frames >= kFrames) {
      reported = true;
      AuroraRenderThreadStats stats{};
      aurora_render_thread_stats(&stats);
      const double seconds = (emscripten_get_now() - started) / 1000.0;
      const int counted = frames - 10;
      std::printf("[render-test] %d frames in %.1f s (%d not begun); this thread per frame: begin %.3f ms, "
                  "record %.3f ms, end %.3f ms\n",
                  frames, seconds, notBegun, beginMs / counted, recordMs / counted, endMs / counted);
      std::printf("[render-test] render thread: %llu frames, %.3f ms busy each; %llu WebGPU calls proxied "
                  "(%.1f us each); waited for it: %.3f ms before decoding, %.3f ms before sealing\n",
                  static_cast<unsigned long long>(stats.cycles),
                  stats.cycles ? stats.cycleMicros / 1000.0 / static_cast<double>(stats.cycles) : 0.0,
                  static_cast<unsigned long long>(stats.proxiedCalls),
                  stats.proxiedCalls ? static_cast<double>(stats.proxiedMicros) / static_cast<double>(stats.proxiedCalls) : 0.0,
                  stats.sealWaitMicros / 1000.0, stats.doneWaitMicros / 1000.0);
      std::printf("[render-test] last depth peek %u\n", static_cast<unsigned>(depth));
      if (expectThread && stats.cycles + 2 < static_cast<uint64_t>(frames)) {
        Fail("the render thread presented " + std::to_string(stats.cycles) + " of " + std::to_string(frames) + " frames");
      }
      if (expectThread && stats.proxiedCalls == 0) Fail("no WebGPU call was proxied to the render thread");
      std::printf("RENDER-THREAD-TEST PASS\n");
      std::fflush(stdout);
    }
    wait_for_animation_frame();
  }
}
