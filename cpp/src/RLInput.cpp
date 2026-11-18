// Action delivery supports a legacy console backend and opt-in CUserCmd
// backends (PORTAL2_RL_DIRECT_INPUT=1 for server, =client for CreateMove).
// Console jump/use pulses can be missed before client command creation.
// The direct backends preserve held bits and analog movement magnitudes;
// InputProbe queues each requested action for one generated/consumed command.
// GameFrame observations can precede consumption; protocol metadata exposes
// the last consumed request and actions still in flight.

#include "RLInput.hpp"
#include "InputProbe.hpp"
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
bool g_desiredValid=false;
QAngle g_desired{};

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
    if ((InputProbe::ClientMode() || action.pitch_delta != 0.0f || action.yaw_delta != 0.0f) &&
        g_SetViewAngles && g_GetViewAngles && g_engineClient) {
        QAngle ang;
        g_GetViewAngles(g_engineClient->ThisPtr(), ang);
        if (InputProbe::ClientMode()) {
            if (!g_desiredValid) {g_desired=ang;g_desiredValid=true;}
            ang=g_desired;
        }
        ang.x += action.pitch_delta;
        ang.y += action.yaw_delta;
        if (ang.x > 89.0f) ang.x = 89.0f;
        if (ang.x < -89.0f) ang.x = -89.0f;
        while (ang.y > 180.0f) ang.y -= 360.0f;
        while (ang.y < -180.0f) ang.y += 360.0f;
        if (InputProbe::ClientMode()) g_desired=ang;
        g_SetViewAngles(g_engineClient->ThisPtr(), ang);
    }
    QAngle target{};
    if (g_GetViewAngles) g_GetViewAngles(g_engineClient->ThisPtr(),target);
    InputProbe::Request(action,target);
}

void ResetAction() {
    InputProbe::Reset();
    g_desiredValid=false;
    // Release everything so a fresh map doesn't inherit stale held
    // input. Mirrors a human releasing every key before a level change.
    RLEngine::QueueConsoleCommand(
        "-forward\n-back\n-moveleft\n-moveright\n"
        "-attack\n-attack2\n-duck\n-jump\n-use\n");
    g_buttons = ButtonState{};
}

bool IsPaused() {
    if (!g_engineClient) return false;
    auto paused=g_engineClient->Original<bool(__rescall *)(void *)>(Offsets::IsPaused);
    return paused(g_engineClient->ThisPtr());
}

void InstallHookIfNeeded(void *player) {
    InputProbe::Install(player);
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
