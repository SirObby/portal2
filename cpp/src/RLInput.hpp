// Legacy console delivery and optional queued CUserCmd delivery. See
// RLInput.cpp and InputProbe.cpp for timing and held-button semantics.

#pragma once

namespace RLInput {

struct Action {
    float move_forward = 0.f;  // [-1, 1]
    float move_side = 0.f;     // [-1, 1]
    float pitch_delta = 0.f;   // degrees this tick
    float yaw_delta = 0.f;     // degrees this tick

    bool jump = false;
    bool duck = false;
    bool use = false;
    bool fire_portal1 = false;  // IN_ATTACK  (blue portal)
    bool fire_portal2 = false;  // IN_ATTACK2 (orange portal)
};

// Applies `action` for this tick: issues the appropriate +/- console
// commands for movement and buttons, and updates view angles via
// IVEngineClient::SetViewAngles. Must be called exactly once per tick
// (RLBridge does this whenever a TICK/RESET_ACK reply contains an
// ACTION), since pulse-button release tracking depends on a 1:1
// call-per-tick cadence.
void SetAction(const Action &action);

// Resolves IVEngineClient (for SetViewAngles/GetViewAngles). Idempotent
// -- safe to call every tick, only resolves once per process. The player
// parameter also installs the optional queued CUserCmd hooks.
void InstallHookIfNeeded(void *player);

// Releases all held buttons and clears pending pulse-release state, so a
// fresh map doesn't inherit stale held input.
void ResetAction();
bool IsPaused();

}  // namespace RLInput
