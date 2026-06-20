// RLInput.hpp: applies an Action via console commands ("+forward",
// "-jump", etc.) issued through IVEngineServer::ClientCommand, exactly
// matching real keyboard/console input semantics. See RLInput.cpp for
// the full history of why direct CUserCmd manipulation (both a
// server-side PlayerRunCommand hook and a client-side SteamControllerMove
// hook) didn't reliably work, and why console commands were chosen
// instead -- this path goes through the exact same dispatch real input
// takes, so the engine's internal key-bind/edge-detection state is
// always consistent with what we send, with no risk of being overwritten
// by some other stage in the pipeline we haven't found.
//
// Button semantics (matching real console/bind behavior):
//   - "+attack"/"+attack2"/"+duck" are HELD: sent as a "+"/"-" pair only
//     on state transitions, exactly like holding/releasing a real key.
//   - "+jump"/"+use" are PULSES: firing the action once, immediately.
//     SetAction() always follows a "+jump"/"+use" with the matching
//     "-jump"/"-use" at the start of the *next* call, regardless of what
//     that next action requests -- this guarantees the press never
//     carries across a tick boundary (which could otherwise cause an
//     unwanted second jump, e.g. immediately on landing or on unpause).
//     A consequence: to jump on two consecutive ACTION messages, the
//     agent doesn't need to do anything special -- each jump=true tick
//     produces its own clean +jump/-jump pulse since the release always
//     happens before the next press is considered.

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
// -- safe to call every tick, only resolves once per process. The
// `player` parameter is accepted for call-site compatibility with
// earlier hook-based designs but is unused.
void InstallHookIfNeeded(void *player);

// Releases all held buttons and clears pending pulse-release state, so a
// fresh map doesn't inherit stale held input.
void ResetAction();

}  // namespace RLInput
