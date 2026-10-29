// [Eggy] runtime/include/eggy/logic_switch.h.
#include "eggy/logic_switch.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace {

uint32_t ReadOff() {
#if defined(__EMSCRIPTEN__)
    // Global constructors run on the page's thread, where location is the page's URL.
    return static_cast<uint32_t>(EM_ASM_INT({
        try {
            var v = typeof location != 'undefined' ? new URLSearchParams(location.search).get('logic') : null;
            if (v) console.log('[logic] switches off: ' + v);
            return v ? (Number(v) | 0) : 0;
        } catch (e) {
            return 0;
        }
    }));
#else
    return 0;
#endif
}

} // namespace

uint32_t LogicSwitch::g_off = ReadOff();
