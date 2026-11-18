#pragma once

// SAR's CInputSystem::SleepUntilInput fix, used without loading SAR itself.
// Only installed in live RL mode, not in demonstration capture mode.
namespace NoFocusSleep {
bool Init();
void Shutdown();
}
