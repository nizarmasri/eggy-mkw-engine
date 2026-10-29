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
| `runtime/src/hle/gx/gx_egg.cpp` | The `AsyncDisplay::endRender` HLE returns at once when `Eggy::SkipEndRender()`. | Race frames Eggy does not show (`renderSkip`, `drawEvery`) skip the EFB copy with the draw, so the screen keeps the last shown frame. |
| `runtime/src/hle/vi.cpp`, `runtime/src/hle/audio/audio.cpp` | `Eggy::Perf::Scope` timers (marked `// [eggy]`) around aurora's frame end/begin, the browser yield, the wait for the retrace boundary, the idle sleeps and the audio mixer. | Race API 2's `perf()` breakdown. They count only in Eggy mode during a race or its results; otherwise one branch. |
| `web/build.sh` | `MKW_CMAKE_ARGS` is added to the configure, and `MKW_PRECOMPRESS_ARGS` to `precompress_dist.py`; both are unset by default. | Eggy's CI caches compiles that use the precompiled headers through ccache (it passes a project include with `-Xclang -fno-pch-timestamp`), and skips the brotli copy of the wasm, which its pack never reads. |
| `aurora-main/lib/aurora.cpp` | Under `AURORA_WEB_RENDER_THREAD` (a pthreads browser build): the frame worker runs on the render thread as two event-loop jobs (`web_cycle_job`, `web_prepare_job`) instead of a blocking `std::thread`; `end_frame` waits for DONE only when the render thread is a whole frame behind, and counts the guest's waits. | The render thread (below). Native builds are unchanged. |
| `aurora-main/lib/webgpu/gpu.cpp`, `aurora-main/cmake/aurora_core.cmake`, `aurora-main/CMakeLists.txt` | `webgpu::initialize` starts the render thread before the first WebGPU call; `web_render_thread.cpp` is compiled in (a no-op without pthreads); a comment. | The render thread. |
| `runtime/src/hle/audio/ax_mix.cpp` | `EnsureMixWorker` mixes on its own thread in a pthreads browser build, as natively. | Takes the AX mix off the guest's thread. |
| `runtime/cmake/PublicProducts.cmake`, `web/build.sh` | `MKW_WEB_RENDER_THREAD` (on by default in `build.sh`): every object compiled with `-pthread`, linked with `-sPTHREAD_POOL_SIZE=3 -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sOFFSCREENCANVAS_SUPPORT` and `web/js/render/library_mkw_render.js`, in `<build>/wasm-baton` (the baton build's directory: the same objects, so a compiler cache serves both), without the prebuilt package. `MKW_WEB_RENDER_THREAD=0` builds the single-threaded module. | The render thread. |

## Added

- `.github/workflows/eggy.yml`: the web disc test on every push (Node, JSPI, no game data).
- `aurora-main/include/aurora/web_render_thread.h`, `aurora-main/lib/webgpu/web_render_thread.{hpp,cpp}`,
  `web/js/render/library_mkw_render.js`, `web/tests/render_thread/`: the render thread (below).
- `runtime/src/eggy/eggy.cpp`, `runtime/include/eggy/eggy.h`, `web/js/library_eggy.js`,
  `web/site/eggy.html`, `web/site/eggy/eggy-engine.js`, `web/site/eggy/eggy-test.js`: Eggy's
  host API (below). Hooks on 0x80009984, 0x80009988, 0x805302C4 and 0x8051AF84, and on the
  five `Pages::RaceHUD` functions 0x80856278, 0x80856CEC, 0x808574B8, 0x80858A50 and 0x80858AE4,
  are `REGISTER_NATIVE_FUNCTION_AS` natives that call the translated bodies; without
  `Module.eggy` they fall straight through.

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

### Race API 2 (2026-10-28): real pace, picture rate, breakdown, results

Everything of race API 1 is unchanged; a page can tell a race API 2 build by
`Module.eggy.raceApi === 2` (the shared block's version word is 2).

- **Pace.** A race frame is due every 16.667 ms of wall time divided by the
  lockstep's pace (on from go; `pacing(false)` turns it off). The calc hook
  (`Eggy_SceneCalc_80009984`) runs up to three more calcs before the next draw
  whenever the race is a whole frame behind; each is a full race frame (its own
  `eggy_pads`, snapshots, hash, `onFrame`). Past six frames behind the debt is
  dropped (`droppedFrames`), and a lockstep stall over 100 ms is a park (no
  frames owed). Before this a phone that took 30 ms a frame ran the race at
  0.55x: the game runs exactly one calc per pass of its frame loop.
- **`drawEvery(n)`**, n 1-4: during a race and its results only one pass in n
  draws (the others skip `SceneManager::draw` and the EFB copy, and give the
  browser a turn instead of presenting). `renderSkip` still works on top
  during a race (the page's per-frame override).
- **Cheaper frames.** `eggy_pads` answers without suspending when the page's
  pad source has the pads (a solo race always); the race hash is taken only on
  the lockstep's checkpoints ((frame + 1) % 60 == 0), every 120th frame and the
  report frame; `kart()`/`hud()` snapshot every 4th race frame (and the report
  frame).
- **`perf()`**: the last window (about 1 s) of a race or its results, per game
  frame in ms: `calcMs` (the game's logic), `drawMs` (its GX drawing),
  `submitMs` (aurora's frame end, submit and next begin), `presentMs` (waiting
  for the 60 Hz boundary when early), `audioMs` (the mixer), `padsWaitMs` (the
  lockstep's wait), `yieldMs` (the browser's turns), `idleMs` (guest idle
  sleeps), `otherMs` (the rest of the window); and `windowMs`, `frames`,
  `passes`, `drawn`, `gameFramesPerSec` (60 is real speed), `late20`/`late34`
  (passes longer than 20/34 ms), `drawEvery`, `pacing`, `extraCalcs`,
  `droppedFrames`, `behindMax`, `seq`. Each part is exclusive: a part timed
  inside another is taken out of it (`Eggy::Perf::Scope`). Shared block words
  480-511 (diag is now 32 words, 448-479). Mailboxes: 32 drawEvery, 33 pacing
  off, 35 slowDraw (test only: microseconds added to every drawn race frame).
- **Results.** Config word 17 is the seconds of the game's own finish camera
  and results screens to show after the report (0, the default, keeps race API
  1's behaviour: idle on the next frame). The report and `onReport` come at the
  finish exactly as before; then the state is `results` (8) while frames run
  with neutral pads and nothing goes to the page, then `idle` parks on the last
  picture (`[eggy] results shown for N ms (F frames)`). `load` and `abort`
  leave it at once. After a `finishAll` the racers still on the track are
  handed to the computer so their karts drive home behind the results. With
  2-4 humans the results and leaderboard pages read the real local player count
  (only the race HUD has the one-slot patch); not yet checked on screen.
- **Test page** (`eggy.html`): `drawEvery=`, `slowDraw=`, `pacing=0`,
  `results=`, `hashParts=1` (the engine logs every hash's parts: `[eggy] hash
  parts frame N: ...`, mailbox 34 bit 0); it logs the hash at frame 0 and each
  checkpoint, `perf()` every 2 s and `race wall ... game frames/s` from go to
  the report.

**Smokes so far (engine-aa953d7, engine-affbb34-baton).** The CI runner is
slower than a phone (calc 14-29 ms and a software-WebGPU submit of 100-140 ms
a frame), so every run was always behind and ran exactly four calcs a pass:
6-29 game frames/s, never 60. In that state the race is the same whatever is
drawn: report hash 2500981042 at frame 1500 with drawEvery 1, 2 and 4, with
slowDraw=30000, and on the baton build. Results ran 10.5 s; two humans reach
the report.

**The hash and what a phone draws (2026-10-29).** Race API 1 gave 2205138225
for the same race, which looked like catch-up changing the race. It was not:
those smokes drew every pass (`skip=0`) and API 1's drew one frame in 20 (the
test page's default render skip). With `hashParts=1`, `pacing=0` and `pacing=1`
agree on every part at every checkpoint, and drawing every pass against one in
20 differs in `rng2` (RaceManager +0x8) alone, at some checkpoints and not
others: the same sequence, reached sooner by the run that draws more. Drawing
takes numbers from the second generator, not inside `SceneManager::draw`
itself (giving each draw its own copy of the generators changed nothing), and
nothing the race plays reads it: scrambled on every race frame for 1500 frames
(six karts, items on, items picked up), no other part moved, while scrambling
`rng1` the same way parted the race within 240 frames. So the hash takes `rng1`
only, and two phones that draw differently hash the same race the same.
(`field=7` and up on the test page crashed loading karts until its computer
list stopped putting King Boo, Funky Kong and Bowser Jr. on another weight
class's vehicle; Eggy's own picks are weight-checked.)

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
- Camera: `RaceCameraMgr` 0x809C19B8: +0x14 cameras, +0x18 sortedCameras, +0x1C cameraCount
  (= screen count 0x808B4BF0, from race scenario +5), +0x25 live view (camera mode 6), +0x27
  focused player (live view only; the ctor sets 0). Outside live view the ctor builds camera i for
  `Racedata::GetPlayerIdOfLocalPlayer(i)` 0x80531F70, which is `hudPlayerIds[i]`; RaceCamera +0x9C
  is its player. In live view it builds one camera per player, indexed by player id.
  `RaceCameraMgr::ChangeFocusedPlayer` 0x805A9B04 assumes that layout (`cameras[playerIdx]`), and
  its only caller is `RaceHUD::BeforeControlUpdate`, in the live-view sections 0x6A-0x6B and
  0x6E-0x6F. Calling it with one screen and player 1 reads `cameras[1]` past the array: this was
  b9770e3's crash (guest store to 0x55440340 = garbage + 0x33E, `isFocused`).
- One screen, many local players (b9770e3's `PatchOneScreen`). Each scenario field has its own
  readers:
  - **Screen count (+5, +7):** sizes the cameras, the audio listeners (`Audio::RaceMgr` ctor
    0x80710688: listener count = +5, all of them `hudPlayerIds[k]`), the Lakitus
    (`Lakitu::Manager::CreatePlayers` 0x8071E480) and per-screen effects and object LOD. All of
    them follow `hudPlayerIds[0]` with no help.
  - **localPlayerCount (+6):** must stay the number of REAL_LOCAL players.
    - `AI::Manager::GetLocalPlayerCount` 0x80739530 bounds the real-player controllers the CPUs
      consider; `AddKartAIController` 0x8073922C fills that list by type, in slot order.
    - Course objects size per-local-kart buffers by +6. 0x80753B34 has capacity = +6 and writes
      one entry per `Kart::Link::IsLocal` kart, so +6 = 1 with two REAL_LOCAL karts overruns it.
    - 0x806DF680 measures distances to karts in slots 0..+6-1.
  - **`Pages::RaceHUD` uses +6, not the screen count.** It sizes its slots (`OnInit` →
    `GetCtrlRaceBaseCount` / `InitCtrlRaceBase`, layout names `CtrlRaceRankNum_<count>_<slot>`)
    and loops over them every frame. The functions that read it:
    - `OnInit` 0x80856278 (and `InitActions`);
    - `BeforeControlUpdate` 0x80856CEC;
    - `AfterControlUpdate` 0x808574B8;
    - `OnItemObjTargeting` 0x80858A50, called from red and blue shells;
    - `UpdateRaceBalloons` 0x80858AE4, called from `RaceCameraMgr::Update`.

    The other readers of +6 are pause, results and leaderboard pages. Eggy shows the 1P HUD of
    slot 0 by making +6 read 1 only while those five run (`OneHudSlot` in eggy.cpp).
  - **Local numbers (player +5) and `hudPlayerIds`:** no reader changes race state. Kart local
    status (`Kart::Status` ctor sets bitfield4 bit 2 from type 0) goes by player type, and so do
    race end (`GMDataVS::CanRaceEnd` 0x80536054 counts unfinished type-0 players; it reads
    `hudPlayerIds[0]` only in competition mode), items (`DriverMgr::IsPlayerComputer` 0x807BD5BC
    is 1 for type 0 whatever the slot; `Item::Player` uses the slot only for local warning sounds)
    and the AI list. Everything else that calls `GetHudSlotId` 0x80531F18 (24 callers) or
    `GetPlayerIdOfLocalPlayer` (53) is camera, HUD, sound, effects, Lakitu or kart models (the
    models only with 2+ screens). Race RNG (`Raceinfo` +4/+8) is drawn by item code only.
  - **Sound:** kart and character actors (`Audio::KartActor::Link` 0x80707534) treat a kart with a
    local number as local, so the other humans' karts sound like local karts (lap jingles, sound
    limits), but the 3D listener is this phone's camera.
- Kart: KartObjectManager 0x809C18F8 +0x20 objects; object +0 -> pointers
  (+0 values (+0 isBike), +4 status (+4/+8/+0xC/+0x10/+0x14 bitfields,
  +0x9C start-boost charge, +0xA6 trickable timer), +8 body (+0x90 physics
  holder, +4 physics, +0x68 position), +0x28 move (+0x14 base speed, +0x20
  speed, +0xCC hop stick x, +0xFC drift state, +0xFE mt, +0x100 smt)).
  MT max 270 at 0x808B5CC2, SMT max 300 at 0x808B5CC6.
- Items: Item manager 0x809C3618 +0x14 players (0x248 each), inventory id
  +0x8C, count +0x90.

### Status

- **engine-9cd5ff9** (smoke run 39378559830), one human:
  - boots to idle, loads config v1, parks on frame 0 and goes;
  - the 240-frame countdown runs and the pads drive the kart;
  - the 1P HUD is full screen.
- **engine-b9770e3** (the one-screen patch), one human: reaches its report (run 40228689077).
  Its per-30-frame hashes equal 9cd5ff9's for the same config and seed, frame for frame
  (0-1500), so a race replays exactly from its config.
- **engine-b9770e3**, 2 humans with myPort 1: crashed at park (run 40213345378). `OnParked` read
  RaceCameraMgr +0x27 (always 0 outside live view) as "the followed player" and called
  `ChangeFocusedPlayer(1)`, which reads `cameras[1]` past the one-camera array.
- **The fix:**
  - The call is gone. Camera 0 already follows `hudPlayerIds[0]`; the park line now logs it as
    `screen: cameras N, camera 0 follows slot S`.
  - `OneHudSlot` gives the race HUD one full-screen slot.
- **engine-6321b26** (build run 40234603031), smokes on 2026-10-25. Each one reaches its report
  at frame 1500 with no runtime error:
  - 40237125193, 2 humans, myPort 1: camera 0 follows slot 1 (Peach) full screen; one 1P HUD
    (3rd, lap 1/3, her Mushroom in the item window).
  - 40237159881, 4 humans, myPort 3: Yoshi full screen; 1P HUD 1st; the minimap highlights him.
  - 40237168406, 1 human: unchanged. Hashes equal b9770e3's, frame for frame.
  - 40237178581, 2 humans, myPort 0: Bowser.
- **myPort 0 and myPort 1 run the same race.** The test page's hash at every 30th frame, 0-1500,
  is identical in all 51 samples between the two 2-human runs, Mushroom pickup included; both end
  on 0x61d64341.
- **Not covered by a smoke:**
  - sound (headless); the other humans' karts are local sound actors (above);
  - the shell-targeting hook (no shells were thrown);
  - team and battle sections with 2+ humans;
  - a cross-port hash comparison with 4 humans.

## The render thread (2026-10-29)

On a phone the browser main thread runs everything: the game's logic and drawing code, aurora's
GX decoding, then sealing, encoding, submitting and presenting the frame through WebGPU's JS
bindings, and the AX mixer. The phone holds that one core near 2 GHz with three or four others
idle. The build (`MKW_WEB_RENDER_THREAD=1`, `web/build.sh`'s default) is now a pthreads module,
and the drawing work that does not need the guest moves to a second core.

**What moves.** aurora starts a render thread (`aurora-main/lib/webgpu/web_render_thread.cpp`)
before its first WebGPU call. The canvas is transferred to it as an OffscreenCanvas
(`emscripten_pthread_attr_settransferredcanvases`, `-sOFFSCREENCANVAS_SUPPORT`), and every
WebGPU object is created there. aurora's frame worker, which natively is a thread that seals,
encodes, submits and presents frame N while the producer records N+1 (the SEALED and DONE
phases in `aurora.cpp`), runs on it as two jobs posted to its event loop: `web_cycle_job`
(posted by `aurora_end_frame`: seal, encode, submit, present) and `web_prepare_job` (the next
frame's surface, staging buffers and pass 0). A browser thread that blocks never presents its
canvas or settles a promise, so the render thread never waits between jobs; it returns to its
event loop after each. The AX mixer runs on its own worker too (`ax_mix.cpp`, as natively).
Emscripten gives a pthread 64 KiB of stack and a wasm stack overflow corrupts memory silently,
so the render thread asks for 4 MiB and every other thread gets 1 MiB.

**What stays.** The guest's thread still runs the game's code and decodes the GX FIFO into
aurora's command lists (most of a picture's cost on the phone: about three quarters of the
draw hook is the game's own code and aurora's GX decoding). WebGPU calls it makes while
decoding (texture, bind group and pipeline cache misses) and during startup run on the render
thread while it waits: `web/js/render/library_mkw_render.js` decorates every emdawnwebgpu
import so that, called anywhere but the render thread, it serializes its arguments and
`mkw_render_run_js` proxies it there. A promise-returning one (`emwgpuWaitAny`: the adapter, the
device, a synchronous readback) must not block the browser main thread: Chrome sets up a
worker's WebGPU connection with a task on the main thread, so a main thread blocked on the render
thread's `requestAdapter` waits forever (engine-5cc24f7's first smoke: "Failed to create WebGPU
adapter: TimedOut" after 5 s). On the main thread it goes through `mkw_render_run_js_async`
instead: the guest's wasm suspends under JSPI, as it did when WebGPU was on the main thread, and
the render thread's answer resumes it from the main thread's event loop. The guest waits for the render thread in two places only: before
the first GX decode of a frame until the previous frame is sealed and the next prepared
(`sealWaitMs`), and before sealing a frame while the render thread is still a whole frame
behind (`doneWaitMs`). At most one frame is in flight. Game state never depends on the render
thread: it only reads the sealed command lists, and the race hash covers none of it.

**Deadlocks.** A worker blocked on a futex runs nothing proxied to it, so a thread holding a
lock the render thread wants must not be waiting for a WebGPU call. The frame protocol keeps
the guest out of the renderer lock while a job runs (decoding waits for SEALED, staging splits
and synchronous readbacks wait for DONE). The render thread takes the renderer lock at the start
of a cycle serving the proxied calls while it waits (`servicedWaits` counts it; it should stay
0). Proxied calls use their own queue, so serving them never runs a nested job.

**Turning it off.** `?renderThread=0` or `Module.mkwRenderThread = false` before the module
starts draws on the guest's thread as before (same build); so does a browser without
`transferControlToOffscreen`. `MKW_WEB_RENDER_THREAD=0 web/build.sh` builds the single-threaded
module of before. The pthreads module needs the page cross-origin isolated (SharedArrayBuffer),
which Eggy always is; it compiles aurora and its libraries from source with `-pthread`.

**perf() fields** (`web/js/library_eggy.js`, shared words 500-507): `picturesPerSec` (pictures
the guest drew per wall second), `guestDrawMs` (its drawing code and GX decoding per picture),
`renderThread` (true when the render thread draws), `renderThreadMs` (that thread busy per
picture: seal, encode, submit, present, prepare), `renderPicturesPerSec`, `proxiedCalls` and
`proxiedMs` (WebGPU calls the guest sent it per picture, and its wait for them), `sealWaitMs`
and `doneWaitMs` (per game frame), `servicedWaits`. `submitMs` and `presentMs` keep their
meaning: with the render thread `submitMs` is only what `aurora_end_frame`/`begin_frame` still
cost the guest.

**Testing.** `web/tests/render_thread/run.sh` builds aurora with `-pthread` and a GX program
(`render_thread_test.cpp`: a clear, a vertex-coloured quad, a textured quad uploaded from the
guest's thread, a depth peek each frame) and checks in headless Chromium, cross-origin
isolated: 120 frames presented on the render thread, WebGPU calls proxied, the three colours on
screen, and the same with `?renderThread=0`; no game data. It has no workflow yet (adding one
needs a token with the `workflow` scope).

**Status (2026-10-29, CI: headless Chromium, 2-core runner, SwiftShader WebGPU).**
- engine-5cc24f7 (built as `variant=baton` for the ccache; a JSPI engine) never got an adapter:
  the main thread blocked on the render thread's `requestAdapter`, which needs a main-thread task
  (above). engine-440bff3 waits asynchronously and starts.
- engine-440bff3, `humans=1&finishAll=1500&drawEvery=2&results=10` (smoke 40906778825): the render
  thread takes the canvas, report hash 2276745402 at frame 1500 (the same as engine-4906146 at
  `skip=0` and the baton build), results 10.1 s, 57-60 game frames/s once warm, `servicedWaits` 0.
- `?renderThread=0` on engine-5cc24f7, 2 humans (smoke 40899498752): report at frame 1500, AX on
  its worker.
- smoke-ab 40906790387, engine-4906146 against engine-440bff3, `skip=0&drawEvery=1`, two rounds
  each, same hash every run. Per game frame, the averages of both rounds:

  | | 4906146 | 440bff3 |
  |---|---|---|
  | race wall, game frames/s | 6.5-6.7 | 4.3-4.4 |
  | calcMs | 11.8-12.0 | 11.6-11.9 |
  | drawMs (guest) | 3.3-3.5 | 3.6-4.8 |
  | submitMs (guest) | 115-121 | 0.15 |
  | audioMs | 2.6-2.7 | 20.8-21.3 |
  | render thread per picture | (on the main thread, ~400 in submit) | 838 |
  | sealWaitMs | - | 198 |

  `-pthread` costs the game's logic nothing measurable. On this runner the render thread is the
  limit: SwiftShader's work takes about twice as long there (the GPU process, the render thread
  and a main thread that spins while it waits share two cores), and the guest waits for each
  picture before decoding the next. The AX mix worker loses on two cores for the same reason (the
  main thread spins on the join while the worker waits for a core). A phone has real GPU work and
  idle cores, so these numbers say little about it; the phone is the test.
- Next, if the phone wants more: the main thread's waits for the render thread (SEALED, DONE) and
  for the mix should suspend under JSPI instead of spinning (a free core for the other threads,
  and less heat on a phone); SEALED can be published right after seal and prepare, so the guest
  decodes the next frame while the render thread encodes (aurora does this natively while it
  interpolates); the mix worker can be turned off on the web if the phone's `audioMs` does not
  drop.
