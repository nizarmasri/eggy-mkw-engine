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

## Added

- `.github/workflows/eggy.yml`: the web disc test on every push (Node, JSPI, no game data).
- `web/js/library_eggy.js`, `runtime/include/eggy/eggy.h`, `web/site/eggy.html`,
  `web/site/eggy/eggy-engine.js`, `web/site/eggy/eggy-test.js`: Eggy's host API (work in
  progress, see below). The library is inert until C++ references its imports.

## Eggy host API: work in progress (2026-10-20)

Goal: `Module.eggy` boots to idle, loads a race from a config v1 (Eggy's
`games/mkw/kit/config.ts`), parks on the race's first frame until `go`, takes
the four pads of each race frame from JS, snapshots kart state/HUD, reports the
result. JS never calls guest code: mailboxes and snapshots live in one shared
block (layout in `web/js/library_eggy.js` `Eggy.W`; the C++ side must match).

Written so far (not yet built): the JS library (mailboxes, suspending
`eggy_wait` / `eggy_pads`, callbacks), the `EngineLike` wrapper
(`eggy-engine.js`), the test page (`eggy.html?course=..&humans=..&myPort=..`,
no clicks, holds A, render-skips all but every 20th frame so the 1 fps smoke
gets through the 240-frame countdown). Missing: `runtime/src/eggy/eggy.cpp`
and the one call in `hle/input/pad.cpp` (`Eggy::OverridePads`).

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

### Next step

Write `runtime/src/eggy/eggy.cpp`: hooks at 0x80009984 (pre-calc: lazy
`eggy_enabled`/`eggy_attach`, state machine idle -> load -> parked -> racing,
`eggy_pads` before calc; post-calc: snapshots, hash, `eggy_frame_done`,
report on stage >= 3 or `finishAll`), 0x805302C4 (bind holders and write
menuScenario before, hud = 1 / hudPlayerIds[0] = myPort's slot / other
humans' localPlayerNum = -1 / seeds after), 0x8051AF84, 0x80009988 and
0x8020FF9C (render skip); `Eggy::OverridePads` in `pad.cpp`; a small
`VI_HLE_NoteHostPark` / pace setter in `vi.cpp` so long parks shift the
retrace grid. Then build + smoke `/eggy.html` (step 1-3), then
`/eggy.html?humans=2&myPort=1` (step 4).
