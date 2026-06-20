#include "Engine.hpp"

#include "Interface.hpp"
#include "Utils/Memory.hpp"
#include "Utils/Platform.hpp"
#include "Modules/Console.hpp"
#include "Offsets.hpp"

namespace RLEngine {

Interface *g_ServerGameDLL = nullptr;
CGlobalVars *gpGlobals = nullptr;
void **g_videomode = nullptr;
_UTIL_PlayerByIndex UTIL_PlayerByIndex = nullptr;
CEntInfo *m_EntPtrArray = nullptr;
Interface *g_VEngineServer = nullptr;
_ClientCommand ClientCommandFn = nullptr;

namespace {

// "server" / "engine" here become "server.so" / "engine.so" on Linux,
// "server.dll" / "engine.dll" on Windows, via MODULE() in Utils.hpp-style
// usage -- replicated directly rather than pulling in SAR's Utils.hpp to
// avoid its other macros.
const char *ServerModuleName() {
    return "server" MODULE_EXTENSION;
}
const char *EngineModuleName() {
    return "engine" MODULE_EXTENSION;
}

}  // namespace

bool Init() {
    // --- IServerGameDLL ("ServerGameDLL005") ---
    g_ServerGameDLL = Interface::Create(ServerModuleName(), "ServerGameDLL005");
    if (!g_ServerGameDLL) {
        console->Warning("Portal2RL: failed to resolve IServerGameDLL (ServerGameDLL005)\n");
        return false;
    }
    console->Print("Portal2RL: [ok] IServerGameDLL = %p\n", g_ServerGameDLL->ThisPtr());

    // --- Think (CServerGameDLL::Think), used as a base address to derive
    //     UTIL_PlayerByIndex and (from that) gpGlobals, exactly as
    //     SourceAutoRecord's Server::Init() does. ---
    uintptr_t think = g_ServerGameDLL->Original(Offsets::Think);
    if (!think) {
        console->Warning("Portal2RL: failed to resolve CServerGameDLL::Think\n");
        return false;
    }
    console->Print("Portal2RL: [ok] Think = 0x%lx\n", (unsigned long)think);

    Memory::Read<_UTIL_PlayerByIndex>(think + Offsets::UTIL_PlayerByIndex, &UTIL_PlayerByIndex);
    if (!UTIL_PlayerByIndex) {
        console->Warning("Portal2RL: failed to resolve UTIL_PlayerByIndex\n");
        return false;
    }
    console->Print("Portal2RL: [ok] UTIL_PlayerByIndex = %p\n", (void *)UTIL_PlayerByIndex);

    Memory::DerefDeref<CGlobalVars *>(
        (uintptr_t)UTIL_PlayerByIndex + Offsets::gpGlobals, &gpGlobals);
    if (!gpGlobals) {
        console->Warning("Portal2RL: failed to resolve gpGlobals\n");
        return false;
    }
    console->Print("Portal2RL: [ok] gpGlobals = %p (mapname=%s, maxEntities=%d)\n",
                     (void *)gpGlobals, gpGlobals->mapname ? gpGlobals->mapname : "(null)",
                     gpGlobals->maxEntities);

    // --- m_EntPtrArray, via IServerTools::GetIServerEntity, exactly as
    //     SourceAutoRecord's Server::Init() does (including the Linux
    //     PIC +4 adjustment, which always applies for Portal 2 on
    //     Linux). We don't need the resulting gEntList/OnRemoveEntity
    //     hook SAR installs here -- we only iterate entities, we don't
    //     need removal notifications. ---
    if (Interface *serverTools = Interface::Create(ServerModuleName(), "VSERVERTOOLS001")) {
        uintptr_t getIServerEntity = serverTools->Original(Offsets::GetIServerEntity);
        Memory::Deref(getIServerEntity + Offsets::m_EntPtrArray, &m_EntPtrArray);
#ifndef _WIN32
        // SourceGame_EIPRelPIC always applies to Portal 2 on Linux (see
        // SAR's Game.hpp: SourceGame_EIPRelPIC == SourceGame_Portal2).
        m_EntPtrArray = (CEntInfo *)((uintptr_t)m_EntPtrArray + 4);
#endif
        Interface::Delete(serverTools);
    }
    if (!m_EntPtrArray) {
        console->Warning("Portal2RL: failed to resolve m_EntPtrArray\n");
        return false;
    }
    console->Print("Portal2RL: [ok] m_EntPtrArray = %p\n", (void *)m_EntPtrArray);

    // --- videomode, via VENGINE_LAUNCHER_API_VERSION004::Init -> a
    //     relative-call read to VideoMode_Create's address -> a fixed
    //     byte offset to the global pointer assignment, exactly as
    //     SourceAutoRecord's Engine::Init() resolves it for
    //     Renderer::Init()/Stitcher::Init(). ---
    Memory::ModuleInfo engineInfo;
    bool haveEngineInfo = Memory::TryGetModule(EngineModuleName(), &engineInfo);
    if (haveEngineInfo) {
        console->Print("Portal2RL: [info] engine.so range = [0x%lx, 0x%lx)\n",
                         (unsigned long)engineInfo.base,
                         (unsigned long)engineInfo.base + (unsigned long)engineInfo.size);
    } else {
        console->Warning("Portal2RL: could not resolve engine.so module info for bounds checking\n");
    }

    auto InEngineRange = [&](uintptr_t addr) -> bool {
        if (!haveEngineInfo) return true;  // can't validate, don't block
        uintptr_t base = (uintptr_t)engineInfo.base;
        return addr >= base && addr < base + engineInfo.size;
    };

    Interface *engineApi =
        Interface::Create(EngineModuleName(), "VENGINE_LAUNCHER_API_VERSION004", /*copyVtable=*/false);
    if (!engineApi) {
        console->Warning("Portal2RL: failed to resolve VENGINE_LAUNCHER_API_VERSION004\n");
        return false;
    }
    uintptr_t initAddr = engineApi->Original(Offsets::Init);
    console->Print("Portal2RL: [ok] EngineAPI::Init = 0x%lx (in engine.so range: %s)\n",
                     (unsigned long)initAddr, InEngineRange(initAddr) ? "yes" : "NO");

    uintptr_t videoModeCreate = Memory::Read(initAddr + Offsets::VideoMode_Create);
    Interface::Delete(engineApi);

    if (!videoModeCreate) {
        console->Warning("Portal2RL: failed to resolve VideoMode_Create\n");
        return false;
    }
    console->Print("Portal2RL: [ok] VideoMode_Create = 0x%lx (in engine.so range: %s)\n",
                     (unsigned long)videoModeCreate, InEngineRange(videoModeCreate) ? "yes" : "NO");
    if (!InEngineRange(videoModeCreate)) {
        console->Warning("Portal2RL: VideoMode_Create resolved outside engine.so -- "
                          "Offsets::Init or Offsets::VideoMode_Create is wrong for this build\n");
        return false;
    }

    Memory::Deref<void **>(videoModeCreate + Offsets::videomode, &g_videomode);
    if (!g_videomode) {
        console->Warning("Portal2RL: failed to resolve videomode\n");
        return false;
    }
    console->Print("Portal2RL: [ok] videomode (slot addr) = %p, *videomode (object) = %p\n",
                     (void *)g_videomode, g_videomode ? *g_videomode : nullptr);

    if (!*g_videomode) {
        console->Warning("Portal2RL: videomode slot dereferenced to null -- "
                          "engine hasn't initialized video mode yet, or offset is wrong\n");
        return false;
    }

    // Sanity check: read the actual IVideoMode object's vtable pointer
    // (one more dereference past g_videomode itself, see Engine.hpp for
    // why g_videomode is void** rather than void*) and the function
    // pointer at the GetModeWidth slot before ever calling through it,
    // so a bad resolution here logs clearly instead of jumping to
    // garbage.
    void *videomode_obj = *g_videomode;
    void *vtable_check = *reinterpret_cast<void **>(videomode_obj);
    console->Print("Portal2RL: [check] videomode object vtable ptr = %p\n", vtable_check);
    // Note: the vtable lives in engine.so's .data.rel.ro which maps at a
    // higher VA than .text, so an "in engine.so range" check against the
    // .text range is unreliable here and omitted intentionally.

    if (!vtable_check || reinterpret_cast<uintptr_t>(vtable_check) < 0x1000) {
        console->Warning("Portal2RL: videomode looks bogus (vtable ptr %p) -- "
                          "VideoMode_Create/Init offsets likely wrong for this build\n",
                          vtable_check);
        g_videomode = nullptr;
        return false;
    }
    void *width_fn = (*reinterpret_cast<void ***>(videomode_obj))[Offsets::GetModeWidth];
    console->Print("Portal2RL: [check] GetModeWidth fn ptr = %p\n", width_fn);
    if (!width_fn || reinterpret_cast<uintptr_t>(width_fn) < 0x1000) {
        console->Warning("Portal2RL: GetModeWidth vtable slot looks bogus (%p) -- "
                          "refusing to call through it\n", width_fn);
        g_videomode = nullptr;
        return false;
    }

    // --- IVEngineServer ("VEngineServer022"), used only for
    //     ClientCommand (issuing console commands like `map <name>` as
    //     if typed by the local listen-server player). ---
    g_VEngineServer = Interface::Create(EngineModuleName(), "VEngineServer022");
    if (g_VEngineServer) {
        ClientCommandFn = g_VEngineServer->Original<_ClientCommand>(Offsets::ClientCommand);
    }
    if (!ClientCommandFn) {
        console->Warning("Portal2RL: failed to resolve IVEngineServer::ClientCommand\n");
        return false;
    }
    console->Print("Portal2RL: [ok] IVEngineServer::ClientCommand = %p\n", (void *)ClientCommandFn);

    console->Print("Portal2RL: all interfaces resolved successfully\n");
    return true;
}

void *GetLocalPlayer() {
    if (!UTIL_PlayerByIndex) return nullptr;
    return UTIL_PlayerByIndex(1);
}

void QueueConsoleCommand(const std::string &command) {
    if (!ClientCommandFn || !gpGlobals || !gpGlobals->pEdicts) return;

    // Entity index 1's edict_t -- the SP listen-server host player, same
    // convention used throughout (GetLocalPlayer(), PlayerRunCommand
    // hook target, etc).
    edict_t *player_edict = gpGlobals->pEdicts + 1;
    ClientCommandFn(g_VEngineServer->ThisPtr(), player_edict, "%s", command.c_str());
}

bool ReadScreenPixels(std::vector<uint8_t> *out, uint32_t *width, uint32_t *height) {
    if (!g_videomode || !*g_videomode) return false;
    void *videomode_obj = *g_videomode;  // see Engine.hpp: g_videomode is void**

    int w = Memory::VMT<int(__rescall *)(void *)>(videomode_obj, Offsets::GetModeWidth)(videomode_obj);
    int h = Memory::VMT<int(__rescall *)(void *)>(videomode_obj, Offsets::GetModeHeight)(videomode_obj);
    if (w <= 0 || h <= 0) return false;

    *width = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);

    size_t needed = static_cast<size_t>(w) * h * 3;  // IMAGE_FORMAT_RGB888
    if (out->size() != needed) out->resize(needed);

    using _ReadScreenPixels = void(__rescall *)(void *, int, int, int, int, void *, ImageFormat);
    auto readScreenPixels =
        Memory::VMT<_ReadScreenPixels>(videomode_obj, Offsets::ReadScreenPixels);
    readScreenPixels(videomode_obj, 0, 0, w, h, out->data(), IMAGE_FORMAT_RGB888);

    return true;
}

}  // namespace RLEngine
