// Adapted from SourceAutoRecord's Modules/InputSystem.cpp (MIT; see
// cpp/vendor/sar/LICENSE). SAR forwards SleepUntilInput with a zero maximum
// wait when sar_disable_no_focus_sleep is enabled. Preserve the engine call:
// returning early would skip input processing along with the sleep.
#include "NoFocusSleep.hpp"
#include "Utils/SDK.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Modules/Console.hpp"

namespace NoFocusSleep {
namespace {
Interface *g_input = nullptr;
using SleepUntilInput = int(__rescall *)(void *self, int max_sleep_ms);
SleepUntilInput g_original = nullptr;

#ifdef _WIN32
int __fastcall WithoutSleep(void *self, int /*edx*/, int /*max_sleep_ms*/) {
#else
int __rescall WithoutSleep(void *self, int /*max_sleep_ms*/) {
#endif
    return g_original(self, 0);
}
}  // namespace

bool Init() {
    if (g_input) return true;
    g_input = Interface::Create("inputsystem" MODULE_EXTENSION, "InputSystemVersion001");
    if (!g_input) {
        console->Warning("Portal2RL: no-focus sleep fix unavailable: InputSystemVersion001 not found\n");
        return false;
    }
    if (!g_input->Hook(WithoutSleep, g_original, Offsets::SleepUntilInput)) {
        Interface::Delete(g_input);
        g_input = nullptr;
        g_original = nullptr;
        console->Warning("Portal2RL: no-focus sleep fix unavailable: SleepUntilInput hook failed\n");
        return false;
    }
    console->Print("Portal2RL: no-focus sleep disabled (native InputSystem hook; SAR not required)\n");
    return true;
}

void Shutdown() {
    if (g_input) {
        // Interface's destructor restores the original vtable before freeing its copy.
        Interface::Delete(g_input);
        g_input = nullptr;
        g_original = nullptr;
    }
}
}  // namespace NoFocusSleep
