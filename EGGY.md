# Eggy's changes to WiiCompiled for the web

This fork is the engine behind Mario Kart Wii in Eggy (a private party-game app
for a group of friends). It is `dynamiteswag81/webcompiled`'s `web-port` (the
browser port of `patchzyy/Wiicompiled`), with the changes below. Eggy's own
code goes in new files where it can; every change to an upstream file is
listed here so an upstream merge knows what to keep.

The repository never holds game data. Eggy builds the game in its private data
repository from a pinned commit of this one.

## Changes to upstream files

| File | Change | Why |
|---|---|---|
| `web/js/library_mkw_dvd.js` | `Module.mkwDiscSource`: when the page supplies `{manifest(mount), read(mount, relPath, offset, length)}`, the disc manifest and every read come from it instead of HTTP. | Eggy serves the disc as content-addressed 1 MiB chunks cached on the phone chunk by chunk, so a dropped connection or a closed page resumes where it stopped. |
| `web/tests/web_disc_node_host.mjs`, `web/tests/run_web_disc_test.mjs` | A third Node run of the disc test through `mkwDiscSource`. | Proves the supplied source behaves exactly like the HTTP path (missing files, truncation, odd names). |
| `runtime/src/hle/input/pad.cpp` | `PAD__Read_HLE` calls `Eggy::OverridePads` after the bindings; in Eggy mode the touch overlay is not applied. | Eggy mode takes every port's pad from the page's race frames (neutral outside a race); a no-op on the stock page. |
| `runtime/src/hle/vi.cpp` | `IntervalForFormat` divides by a pace factor (1 unless Eggy sets it); `VI_HLE_EggyNoteHostPark` and `VI_HLE_EggySetPace` at the end of the file. | Eggy parks the whole guest while idle or waiting for go (the retrace grid moves on like the hidden-page park), and its lockstep runs the clock 0.9-1.1x. |
| `runtime/src/hle/gx/gx_egg.cpp` | The `AsyncDisplay::endRender` HLE returns at once when `Eggy::SkipEndRender()`. | Race frames Eggy does not show (`renderSkip`) skip the EFB copy with the draw, so the screen keeps the last shown frame. |
| `runtime/src/host_context.cpp` | A third web backend under `__EMSCRIPTEN_PTHREADS__` (the baton: every context a pthread, one runs at a time, shared TLS), plus `WebHost`'s baton functions and the `mkw_baton_counters` diagnostics. The JSPI backend is untouched. | iOS 26 has no JSPI (below). |
| `runtime/include/platform/web_host.h` | A baton branch (`__EMSCRIPTEN_PTHREADS__`): `Yield` is a no-op, sleeps block, page visibility is a futex word; `BatonCounter`. The JSPI branch is unchanged. | Same. |
| `runtime/cmake/PublicProducts.cmake` | `MKW_WEB_THREADS` (`jspi` default, or `baton`): the baton branch links `<Target>Baton.js/.wasm` without `-sJSPI` (cmake/WebBaton.cmake); the post-build copy uses `$<TARGET_FILE_BASE_NAME>` (the same names for JSPI). | Same. |
| `web/build.sh` | `MKW_WEB_THREADS=baton`: no prebuilt, `-pthread` everywhere and `SDL_PTHREADS=ON`, its own CMake directory `wasm-baton`, `WiiCompiledBaton.js` and `"threads":"baton"` in products.json. Default output unchanged. | Same. |
| `web/site/main.js` | `?nojspi` deletes `WebAssembly.Suspending`/`promising` before anything loads; without JSPI a cross-origin isolated page runs a `"threads":"baton"` product (JSPI preferred when a product has both). | Same, and to prove a module needs no JSPI. |

## Added

- `.github/workflows/eggy.yml`: the web disc test on every push (Node, JSPI, no game data).
- `runtime/src/eggy/eggy.cpp`, `runtime/include/eggy/eggy.h`, `web/js/library_eggy.js`,
  `web/site/eggy.html`, `web/site/eggy/eggy-engine.js`, `web/site/eggy/eggy-test.js`: Eggy's
  host API (below). Hooks on 0x80009984, 0x80009988, 0x805302C4 and 0x8051AF84 are
  `REGISTER_NATIVE_FUNCTION_AS` natives that call the translated bodies; without `Module.eggy`
  they fall straight through.
- The baton build (below): `runtime/cmake/WebBaton.cmake`,
  `runtime/src/platform/web/baton_main_thread.cpp`, `web/js/baton/library_mkw_baton.js`, the
  test `web/tests/baton/` and `.github/workflows/ios26.yml` (runs it on every push to `ios26`).

## Eggy host API (2026-10-20)

Goal: `Module.eggy` boots to idle, loads a race from a config v1 (Eggy's
`games/mkw/kit/config.ts`), parks on the race's first frame until `go`, takes
the four pads of each race frame from JS, snapshots kart state/HUD, reports the
result. JS never calls guest code: mailboxes and snapshots live in one shared
block (layout in `web/js/library_eggy.js` `Eggy.W`; the C++ side must match).

Pieces: the JS library (mailboxes, suspending `eggy_wait` / `eggy_pads`,
callbacks), `runtime/src/eggy/eggy.cpp` (hooks, state machine, snapshots,
hash, report), the `EngineLike` wrapper (`eggy-engine.js`), and the test page
(`eggy.html?course=..&humans=..&myPort=..`, no clicks, holds A, render-skips
all but every 20th frame so the slow CI smoke gets through the 240-frame
countdown). Note: 0x8020FF9C (`AsyncDisplay::endRender`) is already a
`PPC_NATIVE_OVERRIDE` in `hle/gx/gx_egg.cpp`, so render skip asks
`Eggy::SkipEndRender()` there instead of registering a second native.

### How to hook guest code without losing it

`REGISTER_NATIVE_FUNCTION_AS(0xADDR, MyHook, "name")` with
`void MyHook(CpuContext*)` registers a native that wins dispatch but does
**not** exclude the translated body, which stays callable as
`extern "C" void func_ADDR(CpuContext*)` (upper-case hex; precedent:
`hle/os/os_init.cpp`). Native winners get no static trait, so direct `bl`
callers also go through the registry, and leaf inlining is blocked for them
(`Translator.Cli` `BuildLeafInliningBlockedTargets`). A body that passes `ctx`
on is analysed as a full-context call. New files under `runtime/src/` are
scanned by the translator and globbed by CMake. Call other guest functions
with `InvokeDirectCpu<0xADDR>(&copyOfCtx)` (copy so r3..r12 of the hooked
call survive). Keep `runtime/CMakeLists.txt` untouched: it is part of the web
prebuilt fingerprint.

### Verified against the PAL DOL/REL (sha256 matches recomp.yml)

- Frame loop `RKSystem::run` 0x8000951C: display beginFrame, `onBeginFrame`
  0x80009820 (blr), sceneMgr draw `RKSceneManager::draw` 0x80009988
  (`b EGG::SceneManager::draw` 0x8023AEAC), endRender (`AsyncDisplay::endRender`
  0x8020FF9C: copyEFBtoXFB or clearEFB), sceneMgr calc `RKSceneManager::calc`
  0x80009984 (`b EGG::SceneManager::calc` 0x8023AE60), endFrame, `onEndFrame`
  0x80009824 (blr). All virtual: hook 0x80009984 as pre/post-frame,
  0x80009988 + 0x8020FF9C for render skip (skipping only the scene draw would
  present a cleared, black frame).
- PADRead 0x801AF44C per race frame only via `KPadDirector::calc` 0x805238F0
  from `GameScene::calc` 0x8051B3C8 (inside SceneManager::calc), so pads
  fetched in the pre-calc hook belong to that frame. (Also StrapScene input,
  InitScene::enter once; EGG::GCControllerMgr is unused.)
- Boot section: `SectionMgr::init` 0x80634E44 sets boot +4 (save proxy result,
  else `ArgumentParser::parse`, else 0x3F TitleFromBoot) and next +0xC;
  `InitScene::enter` 0x80543B84 creates the child scene from
  `GetBootSectionSceneId` 0x80634FA0 (+8 if != -1 else +4).
- Section change: `setNextSection(mgr, id, anim)` 0x80635A3C (only if higher
  priority than +0xC), `requestSceneChange(mgr, delay, color)` 0x80635AC8
  (+0x30 = 2); `SectionMgr::calc` 0x8063550C then calls
  `GameScene::ChangeScene` 0x8051AFE8. SectionMgr instance 0x809C1E38;
  +0 current Section (Section +0 = id), +0x30 0 when idle.
- `RaceConfig::initRace` 0x805302C4 is called from `RaceScene_initSLoader`
  0x80553C50 (+0x40) and `RaceScene_onReinit`, before `createSubsystems`
  (RaceManager etc.): the place to write menuScenario (RaceConfig 0x809BD728,
  menu +0xC10, race +0x20) and to fix the race scenario afterwards.
  Scenario: +4 playerCount +5 hudCount +6 localPlayerCount +7 hudCount2;
  player i at +8+i*0xF0 (+5 localPlayerNum, +6 playerInputIdx, +8 vehicle,
  +0xC character, +0x10 type, +0xCC team, +0xD0 controllerId, +0xD8 prevScore,
  +0xDA score); settings +0xB48 course, +0xB4C cc, +0xB50 mode, +0xB54 camera
  (0 = no intro; 5 = intro, which ends in a section reinit), +0xB58 battle,
  +0xB5C cpu, +0xB60 items, +0xB64 hudPlayerIds[4], +0xB6C race#, +0xB6D laps,
  +0xB70 flags (1 mirror, 2 teams, 4 competition), +0xB74/+0xB78 seeds.
- `Scenario::initControllers` 0x8052F4E8: local player k gets
  playerInputIdx k and the type of `KPadDirector` (0x809BD70C) holder k's
  controller (+8+k*0xEC). `RaceManagerPlayer` ctor binds
  input = playerInputIdx != -1 ? holder (mgr+4+idx*0xEC) : AI holder
  (mgr+0x3B4+i*0x180); the HUD slot (localPlayerNum) is not involved.
  Bind port p to holder k with `RealControllerHolder::setController(holder,
  mgr+0x3BA0+p*0xB0, 0)` 0x80521554 (GCN controller p reads channel p) and
  `setDriftType(holder, auto)` 0x80520F2C. SectionPad (SectionMgr+0x34) status
  0 only records the holder's ID each frame; it does not rebind.
- Seeds: RaceManager ctor 0x805327A0 builds `Random(race seed1)`/`(seed2)`
  from RaceConfig+0xB94/+0xB98; the timer manager's Random and
  `Random()` 0x80555464 / reset 0x805554D0 use OSGetTick (next milestone).
- `RaceManager` 0x809BD730: +0xC players, +0x1E intro timer, +0x20 timer,
  +0x28 stage (0 intro, 1 countdown, 2 race, 3 finishing for one frame,
  4 finished), +0x2E canCountdownStart. With camera 0 the countdown starts
  when `CameraManager::isReady`, lasts exactly 240 frames (`0xF0 - timer`).
  Player: +0x20 place, +0x22 battle score, +0x24 lap (0 before the line),
  +0x38 flags (2 finished), +0x40 finish Timer* (+4 min u16, +6 s u8, +8 ms
  u16), +0x48 input holder. `EndRace` 0x805347F4 switches a local kart to the
  AI holder and sets kart Status bitfield4 |= 1 (cpu) via 0x8058F04C: the
  same two writes make `away` CPU-driven.
- Force60: `GameScene::setFramerate(this, is30)` 0x8051AF84 (called from
  `RaceScene_createSubsystems`); force r4 = 0.
- Camera: `ChangeFocusedPlayer(playerId)` 0x805A9B04 (CameraManager
  0x809C19B8, focused +0x27).
- Kart: KartObjectManager 0x809C18F8 +0x20 objects; object +0 -> pointers
  (+0 values (+0 isBike), +4 status (+4/+8/+0xC/+0x10/+0x14 bitfields,
  +0x9C start-boost charge, +0xA6 trickable timer), +8 body (+0x90 physics
  holder, +4 physics, +0x68 position), +0x28 move (+0x14 base speed, +0x20
  speed, +0xCC hop stick x, +0xFC drift state, +0xFE mt, +0x100 smt)).
  MT max 270 at 0x808B5CC2, SMT max 300 at 0x808B5CC6.
- Items: Item manager 0x809C3618 +0x14 players (0x248 each), inventory id
  +0x8C, count +0x90.

### Status

- 9cd5ff9 compiles: data-repo build run 39340035137, build job green (translator
  and emscripten clean). Its publish job was refused by GitHub billing, so there is
  no `engine-9cd5ff9` release yet; `gh run rerun 39340035137 --failed` publishes it
  without rebuilding once Actions runs again.
- Not yet smoke-tested. First smoke: `/eggy.html` with `click=''` (steps 1-3), then
  `/eggy.html?humans=2&myPort=1` (step 4). The console carries `[eggy]` lines for
  every state change, the load, the race scenario after initRace and at park,
  progress every 120 ticks while booting/loading, and every 120 race frames.
- Step 4's one-screen patch copies the shape the game gives its own VS intro
  (`Scenario::initRace` 0x8052FB90 with camera mode 5): screen count 1 while every
  local player keeps a local number and a hudPlayerIds entry.

## iOS 26: the baton build (2026-10-20)

Safari 26 (the owner's iPhone 11) has WebGPU, workers and, on a cross-origin isolated page,
SharedArrayBuffer, but no JSPI, and the default build parks every guest thread as a JSPI stack
on the browser main thread. `MKW_WEB_THREADS=baton web/build.sh ...` builds
`WiiCompiledBaton.js/.wasm`, which needs no JSPI. The default JSPI build is unchanged: every
baton line is behind `__EMSCRIPTEN_PTHREADS__` or in files only the baton link uses.

**Guest threads.** Every HostContext is a pthread (a Web Worker) and exactly one holds the
baton: `Switch` stores 1 in the destination's futex word, wakes it and waits on its own.
`main()` runs on a worker too (`-sPROXY_TO_PTHREAD`) and is the scheduler, so the browser main
thread never blocks. Natively and under JSPI all contexts are fibers of one thread and share its
`thread_local`s (current CpuContext, FP mirror, SEH target, errno, the C++ exception globals);
here a context adopts the scheduler's TLS block (`__tls_base`) when it first runs, which keeps
those semantics exactly: only the baton holder runs and the futex hand-off orders memory like a
fiber switch. A context destroyed after it ran stays parked for good (unwinding a guest stack
would run cleanups against live state); the log counts them.

**Waits.** `WebHost::Yield` is a no-op (nothing to yield to), `SleepUntil` is a real sleep
(Atomics.wait), the hidden-page park blocks on a word the page's `visibilitychange` keeps.
Everything the JSPI build awaited blocks instead: an `__async` JS import marked
`__proxy: 'sync'` runs on the main thread and the worker waits for its promise (Emscripten's
PROXY_SYNC_ASYNC). That covers the disc (`mkw_dvd_*`, so `Module.mkwDiscSource` keeps working,
on the main thread where the page put it), the save store, and every `eggy_*` import
(`web/js/baton/library_mkw_baton.js` decorates them all, so library_eggy.js needs no change).

**Rendering: WebGPU stays on the browser main thread.** A worker blocked in Atomics.wait never
returns to its event loop, so it can never present a canvas frame, have Chrome flush its WebGPU
wire, or settle `createRenderPipelineAsync`/`mapAsync`: rendering from the guest's worker is
not possible, and a dedicated render worker would need OffscreenCanvas plumbing through SDL for
nothing the main thread cannot do. So aurora is split, without editing it (the link `--wrap`s
the symbols in `cmake/WebBaton.cmake`; a name that stops matching fails the link):
`aurora_begin_frame`, `_end_frame`, `_update`, `_shutdown`, `_set_display_mode` run on the main
thread whole (three synchronous round trips a frame; encode and submit happen where the JSPI
build did them); `aurora_initialize` stays on the worker so it can wait for the adapter and the
device, with only its SDL window steps on the main thread (SDL registers DOM listeners for the
calling thread). The WebGPU instance is created without TimedWaitAny (it needs Asyncify or JSPI)
and timed waits poll from the worker. The WebGPU calls a guest thread makes while it decodes the
GX FIFO (texture, bind group and pipeline cache misses, the movie's texture uploads) are proxied
one at a time: every emdawnwebgpu import is marked `__proxy: 'sync'`.

**What is slower.** Each hand-off is a futex wake of another worker (about 27 us on the iPhone,
measured by the coordinator; the log prints the measured wake latency) instead of a JSPI
suspend/resume on one thread; each aurora frame call and each proxied JS call is a round trip to
the main thread, and the guest waits while the main thread encodes the frame (as it did under
JSPI, where everything was one thread). The build prints, every 3 s:
`[ios26] baton: ... frames | hand-offs (n/frame, us to wake) | JS calls on the main thread
(n/frame, us each) | aurora calls on the main thread (n/frame, ms each) | guest blocked on the
main thread x% | live contexts`, and the main thread a heartbeat every 5 s.

**Known limits.**
- If `createRenderPipelineAsync` fails, aurora builds that pipeline synchronously in the
  callback, on the main thread, while the guest may be creating another one
  (`pipeline_cache.cpp`'s `g_webAsync*` statics); a failure is already an error case.
- The baton module compiles aurora and its libraries from source (`-pthread`), so it never uses
  `web/prebuilt/`.
- Workers: 8 are started before `main()`; more start on demand, one per guest thread.

**Testing.** `web/tests/baton/` builds aurora with `-pthread` and runs the pieces above in
headless Chromium with JSPI deleted (cross-origin isolated, SwiftShader WebGPU): contexts on
their own workers sharing TLS, a proxied promise blocking a worker, frames presented from a
guest context in the colour the guest cleared to. In the data repo, `build.yml` with
`variant=baton` builds the game and `smoke.yml` with `path=/?nojspi=1` boots it without JSPI.

**Status.** The test passes in CI (ios26 run 39378346856, Chromium on a 4-core runner,
SwiftShader WebGPU, JSPI deleted): aurora initializes on a worker after waiting for the 0x0
canvas, 60 frames/s presented from a guest context, the canvas shows the guest's clear colour.
Measured there: a hand-off reaches the other worker in 10-25 us; an aurora frame call on the main
thread takes 0.13-0.2 ms (3 a frame); a proxied JS call about 30 us; the guest waits on the main
thread 3-9% of the time. The game itself has not been built as baton yet (the data repo's CI is
blocked by billing).
