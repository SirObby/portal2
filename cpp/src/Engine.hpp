// Engine.hpp: trimmed-down interface bootstrap, purpose-built for the
// Portal2-RL bridge plugin. Inspired directly by (and copying the
// concrete resolution logic of) SourceAutoRecord's Modules/Engine.cpp
// and Modules/Server.cpp, but stripped to only what our bridge needs:
//
//   - g_ServerGameDLL: IServerGameDLL interface, used to hook GameFrame
//     (the per-tick lockstep point) via vtable-slot replacement.
//   - gpGlobals: CGlobalVars*, giving us tickcount, mapname,
//     interval_per_tick, and the edict array directly.
//   - UTIL_PlayerByIndex: resolves the local (SP) player entity pointer
//     each tick, used for player state + as the PlayerRunCommand hook
//     target.
//   - m_EntPtrArray / gEntList: the engine's entity list, used to iterate
//     all entities each tick to find prop_portal instances.
//   - videomode: IVideoMode-equivalent singleton, used to call
//     ReadScreenPixels() for frame capture.
//
// All offsets referenced here come from vendor/sar/Offsets/Portal 2
// 9568.hpp (confirmed by SAR's own source comment to also cover exe
// build 8928, "The Cleaning Game", which matches our target build).
//
// See docs/plugin_architecture.md for the full reasoning trail.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "Utils/SDK.hpp"

class Interface;

namespace RLEngine {

// Populated by Init(). All valid once Init() returns true.
extern Interface *g_ServerGameDLL;  // IServerGameDLL ("ServerGameDLL005")
extern CGlobalVars *gpGlobals;

// IVideoMode-equivalent singleton, stored as a pointer to the engine's
// own global variable slot (void**), matching SourceAutoRecord's
// Renderer.cpp exactly: the relative-call-read chain resolves the
// *address of* the global pointer variable, not the object pointer
// itself, so callers must dereference once more (*g_videomode) to reach
// the actual IVideoMode instance before doing anything else with it
// (e.g. Memory::VMT calls). Storing the extra indirection level here
// (rather than collapsing it) mirrors upstream exactly and avoids the
// off-by-one-dereference bug that caused earlier crashes/bogus reads.
extern void **g_videomode;

using _ClientCommand = int (*)(void *thisptr, void *pEdict, const char *szFmt, ...);
extern Interface *g_VEngineServer;  // IVEngineServer ("VEngineServer022")
extern _ClientCommand ClientCommandFn;

using _UTIL_PlayerByIndex = void *(*)(int);
extern _UTIL_PlayerByIndex UTIL_PlayerByIndex;

// CEntInfo* array (see Utils/SDK/EntityEdict.hpp); index 0 is reserved
// (the world entity lives at index 0 in some builds, player typically at
// index 1 in singleplayer).
extern CEntInfo *m_EntPtrArray;

// Resolves all of the above. Must be called once, early (the engine and
// server modules must already be loaded into the process, which is
// guaranteed by the time our plugin's Load() callback fires). Returns
// false if any required interface/offset could not be resolved -- in
// which case the plugin should log an error and refuse to operate
// (rather than silently behaving incorrectly).
bool Init();

// Returns the current local (singleplayer) player entity pointer, or
// nullptr if not in a level / player not spawned yet. Entity index 1 is
// the convention for the listen-server host in Portal 2 SP.
void *GetLocalPlayer();

// Reads the screen into `out` (resized as needed) as tightly-packed
// RGB24 (width*height*3 bytes). Returns the (width, height) actually
// captured.
bool ReadScreenPixels(std::vector<uint8_t> *out, uint32_t *width, uint32_t *height);

// Issues a console command as if typed by the local listen-server
// player, via IVEngineServer::ClientCommand(edict_t*, fmt, ...) -- the
// same mechanism the in-game console uses for client-issued commands
// like `map <name>`, which is not cheat-protected and works this way
// for the SP listen-server host. Include a trailing '\n' in `command`.
void QueueConsoleCommand(const std::string &command);

}  // namespace RLEngine
