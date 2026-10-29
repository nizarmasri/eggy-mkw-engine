#pragma once

// [eggy] The browser render thread (pthreads web builds only; lib/webgpu/web_render_thread.cpp).
//
// A pthread that owns the page's canvas (transferred as an OffscreenCanvas) and every WebGPU
// object. aurora's frame worker runs there: a frame the guest finished recording is sealed,
// encoded, submitted and presented on it while the guest thread goes on with the next frame's
// game logic. Everywhere else these report an inactive thread and zero counters.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AuroraRenderThreadStats {
  // Frames the render thread sealed, encoded, submitted and presented, and the time it was busy
  // with them (seal, encode, submit, present and preparing the next frame).
  uint64_t cycles;
  uint64_t cycleMicros;
  // WebGPU calls other threads made (texture, bind group and pipeline cache misses while
  // decoding GX), each run on the render thread while the caller waited; and that wait.
  uint64_t proxiedCalls;
  uint64_t proxiedMicros;
  // The guest thread waiting for the render thread: before its first GX decode of a frame until
  // the previous frame was sealed and the next one prepared, and before sealing a frame while the
  // render thread was still on the previous one (a whole frame behind).
  uint64_t sealWaitMicros;
  uint64_t doneWaitMicros;
  // Times the render thread found the renderer lock taken at the start of a frame and served
  // proxied WebGPU calls until it was free (the frame protocol should keep this at zero).
  uint64_t servicedWaits;
} AuroraRenderThreadStats;

// True once the render thread owns the canvas and WebGPU.
bool aurora_render_thread_active(void);

// Waiting on the browser main thread without spinning (pthreads builds; EGGY.md, "The render
// thread"). The main thread cannot block in Atomics.wait, so a C++ wait there spins a core at
// full speed; under JSPI it can suspend instead. aurora_web_wait_changed suspends the calling
// guest until *word no longer holds `seen` (aurora_web_notify) or `timeoutMicros` pass, and
// returns true. It returns false at once, and the caller waits its usual way, off the main
// thread, outside a pthreads build, without Atomics.waitAsync, or when the page turned it off
// (Module.mkwSuspendWaits = false, ?suspendWaits=0). Only call it where the guest may suspend
// (its own code, under the JSPI exports) and with no lock held that the page's callbacks take.
bool aurora_web_wait_changed(int32_t* word, int32_t seen, uint32_t timeoutMicros);
// Adds one to *word and wakes every waiter (any thread).
void aurora_web_notify(int32_t* word);
// Monotonic counters since start; zero when inactive.
void aurora_render_thread_stats(AuroraRenderThreadStats* out);

#ifdef __cplusplus
}
#endif
