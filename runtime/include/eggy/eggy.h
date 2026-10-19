#pragma once

// Eggy's host API inside the runtime (runtime/src/eggy/eggy.cpp, web/js/library_eggy.js).
//
// Eggy (a private party-game app) runs Mario Kart Wii races in a phone browser. With Module.eggy
// set by the page, the engine boots to an idle state, loads a race from a config the page
// writes, parks on the race's first frame until it is told to go, takes the four pads of every
// race frame from the page, keeps a snapshot of kart state and the HUD, and reports the result.
// Without Module.eggy (the stock page) none of it does anything: every hook falls straight
// through to the game's own code.
//
// The only call sites in upstream files are listed in EGGY.md.

#include <chrono>
#include <cstdint>

struct PADStatus;

namespace Eggy {

// True when the page asked for Eggy mode (Module.eggy is set). Always false outside the browser
// build. The first call attaches the shared block to the page.
bool Enabled() noexcept;

// Attaches the shared block to the page (idempotent; Enabled() calls it on first use).
void Initialize() noexcept;

// PADRead (hle/input/pad.cpp): in Eggy mode every port's status comes from Eggy (the race
// frame's pads while racing, neutral otherwise), never from the keyboard or a gamepad, and no
// port ever reports itself disconnected. Returns false (statuses untouched) outside Eggy mode.
bool OverridePads(PADStatus* statuses, uint32_t count) noexcept;

// AsyncDisplay::endRender (hle/gx/gx_egg.cpp): true on a race frame whose draw Eggy skipped
// (the renderSkip mailbox), so its EFB copy is skipped too and the screen keeps the last frame.
bool SkipEndRender() noexcept;

} // namespace Eggy

// Implemented in hle/vi.cpp for Eggy:
// The guest was parked (idle, or a loaded race waiting for go) for `parked`: move the VI retrace
// grid forward by that much so no retraces are "caught up" on resume (the page-hidden park does
// the same).
void VI_HLE_EggyNoteHostPark(std::chrono::nanoseconds parked);
// Runs the retrace clock `factor` times as fast (Eggy's lockstep time sync, about 0.97-1.03).
void VI_HLE_EggySetPace(double factor);
