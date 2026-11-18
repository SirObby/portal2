#pragma once
#include "RLInput.hpp"
#include "Utils/SDK/Math.hpp"
namespace InputProbe {
bool ClientMode();
const char *BackendName();
bool Ready();
int LastConsumed();
int InFlight();
void Install(void *player);
void Request(const RLInput::Action &action, const QAngle &target);
void Reset();
void Detach();
void Shutdown();
}
