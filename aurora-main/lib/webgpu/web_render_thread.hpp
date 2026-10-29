#pragma once

// [eggy] The browser render thread (EGGY.md, "The render thread"). Pthreads web builds only;
// elsewhere every function reports no render thread.

#include <cstdint>
#include <mutex>
#include <thread>

namespace aurora::webgpu::render_thread {

// Starts the render thread on the browser main thread before any WebGPU call: the canvas is
// transferred to it, and from then on every emdawnwebgpu import runs there
// (web/js/render/library_mkw_render.js). False (and everything stays on the calling thread, as
// before) when the build has no threads, the page opted out or the browser cannot transfer a
// canvas.
bool start() noexcept;
bool active() noexcept;
bool on_render_thread() noexcept;
std::thread::id thread_id() noexcept;

// Runs fn(arg) on the render thread without waiting, in order with every WebGPU call proxied to
// it. The render thread never blocks between jobs: it presents the canvas and settles WebGPU
// promises in its own event loop.
bool post(void (*fn)(void*), void* arg) noexcept;

// Render thread busy time; `frame` counts a new frame (the seal/encode/present job).
void note_cycle(uint64_t micros, bool frame) noexcept;
void note_seal_wait(uint64_t micros) noexcept;
void note_done_wait(uint64_t micros) noexcept;

// Takes `mutex` on the render thread, running the WebGPU calls other threads proxy here while it
// waits (a plain lock elsewhere).
void lock_serviced(std::recursive_mutex& mutex) noexcept;

} // namespace aurora::webgpu::render_thread
