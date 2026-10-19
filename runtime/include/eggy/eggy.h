#pragma once

// Eggy's host API inside the runtime (runtime/src/eggy/eggy.cpp, web/js/library_eggy.js).
//
// Eggy (a private party-game app) runs Mario Kart Wii races in a phone browser. With Module.eggy
// set by the page, the engine boots to an idle state, loads a race from a config the page
// writes, parks on the race's first frame until it is told to go, takes the four pads of every
// race frame from the page, keeps a snapshot of kart state and the HUD, and reports the result.
// Without Module.eggy (the stock page) none of it does anything: every hook below falls straight
// through to the game's own code.
//
// The only call sites in upstream files are listed in EGGY.md.

#include <cstdint>

struct PADStatus;

namespace Eggy {

// True once main() found Module.eggy on the page (always false outside the browser build).
bool Enabled() noexcept;

// Called from main() before the game starts: attaches the shared block to the page.
void Initialize() noexcept;

// PADRead (hle/input/pad.cpp): in Eggy mode every port's status comes from Eggy (the race
// frame's pads while racing, neutral otherwise), never from the keyboard or a gamepad, and no
// port ever reports itself disconnected. Returns false (statuses untouched) outside Eggy mode.
bool OverridePads(PADStatus* statuses, uint32_t count) noexcept;

} // namespace Eggy
