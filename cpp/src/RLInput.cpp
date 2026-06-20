// RLInput.cpp: applies an Action via IVEngineServer::ClientCommand,
// issuing "+key"/"-key" style console commands exactly as a human typing
// into the console (or a real bound key) would -- e.g. "+forward",
// "-forward", "+jump", "-jump". This replaced two earlier approaches
// that didn't work correctly:
//
//   1. Hooking CPortal_Player::PlayerRunCommand (server-side) and
//      writing cmd->buttons/viewangles directly -- buttons and angles
//      were silently overwritten by the client input pipeline after our
//      hook ran (movement axes apparently survived, buttons/angles did
//      not).
//   2. Hooking CInput::SteamControllerMove (client-side) and writing
//      cmd->buttons/viewangles there instead -- confirmed via logging
//      that our writes DID land in the CUserCmd correctly (buttons_after
//      showed IN_JUMP set exactly when expected), and view angle control
//      via IVEngineClient::SetViewAngles worked correctly, and
//      forward/side movement worked correctly -- but jump still never
//      triggered. The likely explanation: Source's jump-button edge
//      detection (and bind-state tracking in general) lives in the
//      *console command / key-bind* subsystem (in_jump.state, polled by
//      CInput::CreateMove via the normal key-bind path), which our
//      direct CUserCmd write bypasses entirely -- the cmd we wrote
//      reached the server, but some other internal state the movement
//      code also consults (or a later overwrite specifically for
//      button-derived fields we didn't find) never agreed with it.
//
// Console commands avoid all of this by going through the exact same
// path real input takes: "+jump" calls the engine's registered "+jump"
// ConCommand, which sets the same internal key-bind state a physical
// keypress would, which CInput::CreateMove then reads when building the
// CUserCmd -- guaranteed consistent with whatever the rest of the engine
// expects, since it's indistinguishable from real input at every layer
// downstream of the command dispatch.
//
// Button semantics (matching real Source console/bind behavior exactly,
// per user-confirmed observation):
//   - "+attack"/"+attack2" (portal gun) are HELD: they fire continuously
//     until the matching "-attack"/"-attack2" is sent. We track held
//     state across ticks and only emit a transition when the desired
//     state changes, like a human holding/releasing a mouse button.
//   - "+jump"/"+use" fire their effect once, immediately, when run --
//     holding "+jump" across tick boundaries risks an unwanted second
//     jump later (e.g. immediately on landing, or on unpause), so we
//     always immediately follow a "+jump"/"+use" with "-jump"/"-use" on
//     the very next tick, regardless of what the agent's action requests
//     for that next tick. This means a single ACTION message with
//     jump=true reliably produces exactly one jump, and the agent must
//     send jump=false for at least one tick before sending jump=true
//     again to trigger a second jump (matching real key-press semantics:
//     you can't "jump again" without releasing the key first).
//   - Movement axes (forward/side) and view angles continue to use
//     direct, immediate mechanisms (ClientCommand for movement,
//     IVEngineClient::SetViewAngles for angles) since those worked
//     correctly with the previous approach and don't have edge-detection
//     semantics to worry about.

#include "RLInput.hpp"
#include "Engine.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/SDK.hpp"
#include "Modules/Console.hpp"

#include <cstdio>

namespace RLInput {

namespace {

// ------------------------------------------------------------------
// IVEngineClient::SetViewAngles/GetViewAngles (vtable slots 19/18 on
// "VEngineClient015"), unchanged from the previous approach -- this
// part worked correctly.
// ------------------------------------------------------------------
using _SetViewAngles = void(__rescall *)(void *thisptr, const QAngle &va);
using _GetViewAngles = void(__rescall *)(void *thisptr, QAngle &va);

Interface *g_engineClient = nullptr;
_SetViewAngles g_SetViewAngles = nullptr;
_GetViewAngles g_GetViewAngles = nullptr;

bool g_initialized = false;

// Tracks held-button state across ticks so we only emit "+x"/"-x" on
// actual transitions for held-style buttons (attack/attack2), and so we
// know when a pulse button (jump/use) needs its "-x" follow-up sent on
// the *next* tick regardless of the new action's request.
struct ButtonState {
    bool attack1_held = false;
    bool attack2_held = false;
    bool duck_held = false;

    // Pulse buttons: set true the tick after we send "+jump"/"+use", so
    // ApplyAction knows to send the matching "-jump"/"-use" this tick
    // before considering the new action's jump/use request.
    bool jump_pending_release = false;
    bool use_pending_release = false;
};

ButtonState g_buttons;

const char *PlayerModuleName() {
    // IVEngineServer::ClientCommand runs the command on the local
    // listen-server client, which is implemented in engine.so (not
    // server.so) -- ClientCommandFn itself already resolved against
    // "engine.so" in Engine.cpp, this name isn't otherwise needed here,
    // kept only for clarity at call sites below.
    return "engine" MODULE_EXTENSION;
}

}  // namespace

void SetAction(const Action &action) {
    // --- Pulse buttons: release first if pending from last tick ---
    if (g_buttons.jump_pending_release) {
        RLEngine::QueueConsoleCommand("-jump\n");
        g_buttons.jump_pending_release = false;
    }
    if (g_buttons.use_pending_release) {
        RLEngine::QueueConsoleCommand("-use\n");
        g_buttons.use_pending_release = false;
    }

    // --- Movement: forward/back, strafe left/right ---
    // Sent as instantaneous +/- pairs each tick based on sign, mirroring
    // held-key semantics (a real held key would keep sending the same
    // "+forward" state; re-sending "+forward" every tick it's already
    // held is harmless -- the engine just keeps the bind active).
    constexpr float kDeadzone = 0.1f;

    if (action.move_forward > kDeadzone) {
        RLEngine::QueueConsoleCommand("-back\n+forward\n");
    } else if (action.move_forward < -kDeadzone) {
        RLEngine::QueueConsoleCommand("-forward\n+back\n");
    } else {
        RLEngine::QueueConsoleCommand("-forward\n-back\n");
    }

    if (action.move_side > kDeadzone) {
        RLEngine::QueueConsoleCommand("-moveleft\n+moveright\n");
    } else if (action.move_side < -kDeadzone) {
        RLEngine::QueueConsoleCommand("-moveright\n+moveleft\n");
    } else {
        RLEngine::QueueConsoleCommand("-moveleft\n-moveright\n");
    }

    // --- Held buttons: attack / attack2 (portal gun), duck ---
    if (action.fire_portal1 != g_buttons.attack1_held) {
        RLEngine::QueueConsoleCommand(action.fire_portal1 ? "+attack\n" : "-attack\n");
        g_buttons.attack1_held = action.fire_portal1;
    }
    if (action.fire_portal2 != g_buttons.attack2_held) {
        RLEngine::QueueConsoleCommand(action.fire_portal2 ? "+attack2\n" : "-attack2\n");
        g_buttons.attack2_held = action.fire_portal2;
    }
    if (action.duck != g_buttons.duck_held) {
        RLEngine::QueueConsoleCommand(action.duck ? "+duck\n" : "-duck\n");
        g_buttons.duck_held = action.duck;
    }

    // --- Pulse buttons: jump / use ---
    // Fire "+x" this tick if requested; schedule "-x" for the start of
    // the *next* SetAction call regardless of what that next action
    // requests, so the press is always a clean single-tick pulse and
    // never carries across a tick boundary (avoids unwanted repeat
    // jumps, e.g. on landing or on unpause, exactly the failure mode
    // flagged during review of this design).
    if (action.jump) {
        RLEngine::QueueConsoleCommand("+jump\n");
        g_buttons.jump_pending_release = true;
    }
    if (action.use) {
        RLEngine::QueueConsoleCommand("+use\n");
        g_buttons.use_pending_release = true;
    }

    // --- View angles ---
    if ((action.pitch_delta != 0.0f || action.yaw_delta != 0.0f) &&
        g_SetViewAngles && g_GetViewAngles && g_engineClient) {
        QAngle ang;
        g_GetViewAngles(g_engineClient->ThisPtr(), ang);
        ang.x += action.pitch_delta;
        ang.y += action.yaw_delta;
        if (ang.x > 89.0f) ang.x = 89.0f;
        if (ang.x < -89.0f) ang.x = -89.0f;
        while (ang.y > 180.0f) ang.y -= 360.0f;
        while (ang.y < -180.0f) ang.y += 360.0f;
        g_SetViewAngles(g_engineClient->ThisPtr(), ang);
    }
}

void ResetAction() {
    // Release everything so a fresh map doesn't inherit stale held
    // input. Mirrors a human releasing every key before a level change.
    RLEngine::QueueConsoleCommand(
        "-forward\n-back\n-moveleft\n-moveright\n"
        "-attack\n-attack2\n-duck\n-jump\n-use\n");
    g_buttons = ButtonState{};
}

void InstallHookIfNeeded(void * /*player*/) {
    if (g_initialized) return;

    if (!g_engineClient) {
        g_engineClient = Interface::Create("engine" MODULE_EXTENSION, "VEngineClient015");
        if (!g_engineClient) {
            console->Warning("Portal2RL: failed to resolve VEngineClient015\n");
            return;
        }
    }
    g_GetViewAngles = g_engineClient->Original<_GetViewAngles>(Offsets::GetViewAngles);
    g_SetViewAngles = g_engineClient->Original<_SetViewAngles>(Offsets::SetViewAngles);
    console->Print("Portal2RL: [ok] GetViewAngles=%p SetViewAngles=%p\n",
                     (void *)g_GetViewAngles, (void *)g_SetViewAngles);

    if (!g_GetViewAngles || !g_SetViewAngles) {
        console->Warning("Portal2RL: failed to resolve SetViewAngles/GetViewAngles\n");
        return;
    }

    g_initialized = true;
    console->Print("Portal2RL: input system ready (console-command based)\n");
}

}  // namespace RLInput
