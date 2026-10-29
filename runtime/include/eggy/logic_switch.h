#pragma once

// [Eggy] Logic performance switches: each bit of the test page's `logic=` URL parameter turns
// one optimization off, so a single engine build can race with and without each of them on the
// same CI runner (engine-to-engine timings only compare on one runner). Read once at startup;
// with no parameter (players, the stock page, native builds) every optimization is on.

#include <cstdint>

namespace LogicSwitch {

extern uint32_t g_off;

enum : uint32_t {
    kIndirectFast = 1u << 0,  // abi_bridge.h TryDispatchRawCpuTarget: skip the guards' setup
    kLastPage = 1u << 1,      // memory.cpp: a backing store's last 1 MiB page takes the coarse tier
};

inline bool On(uint32_t bit) noexcept { return (g_off & bit) == 0; }

} // namespace LogicSwitch
