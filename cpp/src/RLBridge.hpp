// RLBridge.hpp: orchestrates the protocol exchange described in
// docs/protocol.md, called from the plugin's IServerPluginCallbacks
// implementation. Owns the control socket + shared memory frame buffer,
// and drives the per-tick TICK/ACTION (and RESET/RESET_ACK) exchange
// from within the GameFrame hook.

#pragma once

#include <cstdint>
#include <string>

namespace RLBridge {

// Starts listening on 127.0.0.1:port. Does not block; the handshake
// (waiting for Python to connect) happens lazily on the first
// OnGameFrame() call so engine startup isn't blocked.
bool Start(uint16_t port);

// Called once per server tick from our IServerGameDLL::GameFrame hook
// (see RLGameFrameHook.hpp), with `simulating` mirroring the engine's own
// parameter (false during non-simulating frames, e.g. the main menu or a
// paused loading screen -- we skip the exchange in that case so we never
// send TICK messages for frames with no meaningful game state).
void OnGameFrame(bool simulating);

// Called from IServerPluginCallbacks::LevelInit. Used to detect map
// transitions: if the new map name differs from what we last requested
// via a RESET message, this was an engine-autonomous transition (e.g.
// point_changelevel firing), and we mark the *previous* tick's state as
// map_finished so Python sees it as a termination signal (see
// docs/protocol.md "Level transitions"). Also resets the per-episode
// tick counter.
void OnLevelInit(const char *map_name);

// Called from IServerPluginCallbacks::ServerActivate, once entities have
// finished spawning after a level load -- safe to start reading
// player/portal state and to (re)install the PlayerRunCommand hook.
void OnServerActivate();

// Closes the control socket and shared memory. Called from
// IServerPluginCallbacks::Unload.
void Shutdown();

}  // namespace RLBridge
