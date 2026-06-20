// Plugin.cpp: IServerPluginCallbacks implementation -- entry point for
// the Portal2-RL plugin. Loaded by Portal 2 via `plugin_load
// portal2_rl_plugin` (or `+plugin_load portal2_rl_plugin` on the launch
// command line, see python/portal2_gym/launcher.py).
//
// Most of our actual per-tick logic lives in RLBridge.cpp, driven from
// our own hook on IServerGameDLL::GameFrame (installed here in Load(),
// the same vtable-slot-replacement mechanism SourceAutoRecord uses for
// the same interface/offset -- see Server::Init() in the vendored
// reference source for the pattern this mirrors).
//
// IServerPluginCallbacks itself (Load/Unload/LevelInit/GameFrame/etc.)
// is a *separate* mechanism from our GameFrame hook: the engine calls
// our IServerPluginCallbacks methods directly (no hooking needed, this
// is the plugin's actual vtable, registered via CreateInterface), while
// GameFrame is *additionally* hooked on the interface so we can run our
// tick-exchange logic at the exact point the original CServerGameDLL::
// GameFrame would have executed, before/after the engine's own per-tick
// work.
//
// In practice we don't strictly need both: IServerPluginCallbacks::
// GameFrame(bool simulating) already fires once per tick and is simpler
// to use than hooking IServerGameDLL::GameFrame's vtable slot directly.
// We use the IServerPluginCallbacks::GameFrame callback as our primary
// per-tick entry point (cleaner, no hook-disable/enable dance needed),
// and skip installing the IServerGameDLL::GameFrame vtable hook
// entirely -- RLEngine::g_ServerGameDLL is still resolved (we need the
// interface for Offsets::Think's address derivation in Engine::Init(),
// see Engine.cpp), but its GameFrame slot is left untouched.

#include "Utils/SDK.hpp"
#include "Engine.hpp"
#include "RLBridge.hpp"
#include "RLInput.hpp"
#include "Modules/Console.hpp"
#include "Offsets.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

uint16_t g_port = 27182;
bool g_engine_initialized = false;
bool g_loaded = false;
bool g_has_sar = false;

// Returns the process's full command line as a single space-separated
// string (NULs replaced with spaces on Linux, where /proc/self/cmdline
// is NUL-separated). Used by the small substring-search parsers below
// for launch-time options we don't want to spend a ConVar/ICvar
// registration on.
const char *GetCommandLine_() {
#ifdef _WIN32
    return GetCommandLineA();
#else
    static char cmdline_buf[4096];
    static bool loaded = false;
    if (!loaded) {
        FILE *f = std::fopen("/proc/self/cmdline", "rb");
        if (!f) {
            cmdline_buf[0] = '\0';
            return cmdline_buf;
        }
        size_t n = std::fread(cmdline_buf, 1, sizeof(cmdline_buf) - 1, f);
        std::fclose(f);
        cmdline_buf[n] = '\0';
        for (size_t i = 0; i < n; ++i) {
            if (cmdline_buf[i] == '\0') cmdline_buf[i] = ' ';
        }
        loaded = true;
    }
    return cmdline_buf;
#endif
}

// Parses "+portal2_rl_port <N>" from the process's command line. We
// can't rely on a ConVar/ConCommand here (registering one would require
// resolving ICvar, more interfaces than we need) -- reading the raw
// command line is sufficient since this is only ever set at launch.
uint16_t ParsePortFromCommandLine() {
    const char *cmdline = GetCommandLine_();

    const char *needle = "portal2_rl_port";
    const char *pos = std::strstr(cmdline, needle);
    if (!pos) return g_port;

    pos += std::strlen(needle);
    while (*pos == ' ') ++pos;
    int value = std::atoi(pos);
    return (value > 0 && value < 65536) ? static_cast<uint16_t>(value) : g_port;
}

// Parses "+portal2_rl_has_sar <0|1>" from the command line (see
// GameLauncher._common_options in launcher.py). When set, we issue
// "plugin_load sar" + "sar_disable_no_focus_sleep 1" ourselves via
// ClientCommand once Load() succeeds, since +plugin_load sar placed
// directly on the launch command line does not reliably take effect
// (confirmed: our own plugin loads fine the same way SAR does not).
bool ParseHasSarFromCommandLine() {
    const char *cmdline = GetCommandLine_();

    const char *needle = "portal2_rl_has_sar";
    const char *pos = std::strstr(cmdline, needle);
    if (!pos) return false;

    pos += std::strlen(needle);
    while (*pos == ' ') ++pos;
    return std::atoi(pos) != 0;
}

}  // namespace

class Portal2RLPlugin : public IServerPluginCallbacks {
public:
    bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) override {
        (void)interfaceFactory;
        (void)gameServerFactory;

        // Guard against being loaded twice in the same process. Observed
        // in practice when combined with a second +plugin_load on the
        // launch command line (e.g. has_sar=True adding "+plugin_load
        // sar" later on the same line) -- the engine's "+command arg"
        // command-line parsing isn't guaranteed to cleanly bound each
        // command's argument list, and Load() ran twice with byte-
        // identical resolved addresses both times, the second call
        // failing only when it tried to re-bind our already-listening
        // control socket. Without this guard the second call would
        // otherwise re-run Offsets::LoadOffsets()/RLEngine::Init()
        // harmlessly but then fail loudly at RLBridge::Start(); with it,
        // the second call is a clean no-op success.
        if (g_loaded) {
            console->Print("Portal2RL: Load() called again, ignoring "
                             "(already loaded -- check for duplicate "
                             "+plugin_load on the launch command line)\n");
            return true;
        }

        // We deliberately ignore the interfaceFactory/gameServerFactory
        // callbacks passed here and instead resolve everything ourselves
        // via Interface::Create(moduleName, version) (see Engine.cpp),
        // matching SourceAutoRecord's approach. This is more robust than
        // relying on the factories' search scope, and lets us resolve
        // interfaces from any loaded module (e.g. the engine binary),
        // not just the ones the two factories expose.
        Offsets::LoadOffsets();
        g_engine_initialized = RLEngine::Init();
        if (!g_engine_initialized) {
            console->Warning("Portal2RL: failed to initialize engine interfaces; "
                              "plugin will not function. Check that the Portal 2 "
                              "build matches the offsets table in use "
                              "(vendor/sar/Offsets/Portal 2 9568.hpp).\n");
            return false;
        }

        g_port = ParsePortFromCommandLine();
        if (!RLBridge::Start(g_port)) {
            console->Warning("Portal2RL: failed to start control socket on port %d\n", g_port);
            return false;
        }

        g_has_sar = ParseHasSarFromCommandLine();
        if (g_has_sar) {
            console->Print("Portal2RL: has_sar requested -- will issue "
                             "'plugin_load sar' once a level is ready\n");
        }

        console->Print("Portal2RL: listening on 127.0.0.1:%d, waiting for Python client...\n", g_port);
        g_loaded = true;
        return true;
    }

    void Unload() override {
        RLBridge::Shutdown();
        g_loaded = false;
    }

    void Pause() override {}
    void UnPause() override {}

    const char *GetPluginDescription() override {
        return "Portal2-RL: Gymnasium environment bridge plugin";
    }

    void LevelInit(char const *pMapName) override {
        RLBridge::OnLevelInit(pMapName);
    }

    void ServerActivate(void * /*pEdictList*/, int /*edictCount*/, int /*clientMax*/) override {
        RLBridge::OnServerActivate();
    }

    void GameFrame(bool simulating) override {
        if (!g_engine_initialized) return;

        if (g_has_sar && simulating) {
            // Fire once, on the first simulating tick we see -- by this
            // point a level has loaded and entities/ClientCommand's
            // target edict exist, unlike at Load() time. Issuing this
            // from Load() itself was tried and didn't reliably take
            // effect; deferring to here uses the same mechanism (and
            // same target edict) that QueueConsoleCommand("map ...")
            // already proven to work correctly with.
            g_has_sar = false;
            RLEngine::QueueConsoleCommand("plugin_load sar\nsar_disable_no_focus_sleep 1\n");
            console->Print("Portal2RL: issued 'plugin_load sar' + "
                             "'sar_disable_no_focus_sleep 1'\n");
        }

        RLBridge::OnGameFrame(simulating);
    }

    void LevelShutdown() override {}

    void ClientFullyConnect(void * /*pEdict*/) override {}

    void ClientActive(void *pEntity) override {
        // This fires once the local player is fully in the server --
        // UTIL_PlayerByIndex(1) is valid from here onward, making this
        // the right place to install the PlayerRunCommand hook (earlier
        // callbacks like ServerActivate fire before the player entity
        // exists, so InstallHookIfNeeded would get a null player there).
        (void)pEntity;
        void *player = RLEngine::GetLocalPlayer();
        if (player) RLInput::InstallHookIfNeeded(player);
    }
    void ClientDisconnect(void * /*pEntity*/) override {}
    void ClientPutInServer(void * /*pEntity*/, char const * /*playername*/) override {}
    void SetCommandClient(int /*index*/) override {}
    void ClientSettingsChanged(void * /*pEdict*/) override {}

    int ClientConnect(bool *bAllowConnect, void * /*pEntity*/, const char * /*pszName*/,
                        const char * /*pszAddress*/, char * /*reject*/, int /*maxrejectlen*/) override {
        if (bAllowConnect) *bAllowConnect = true;
        return 0;  // PLUGIN_CONTINUE
    }

    int ClientCommand(void * /*pEntity*/, const void *& /*args*/) override {
        return 0;  // PLUGIN_CONTINUE
    }

    int NetworkIDValidated(const char * /*pszUserName*/, const char * /*pszNetworkID*/) override {
        return 0;  // PLUGIN_CONTINUE
    }

    void OnQueryCvarValueFinished(int /*iCookie*/, void * /*pPlayerEntity*/, int /*eStatus*/,
                                    const char * /*pCvarName*/, const char * /*pCvarValue*/) override {}

    void OnEdictAllocated(void * /*edict*/) override {}
    void OnEdictFreed(const void * /*edict*/) override {}
};

EXPOSE_SINGLE_INTERFACE(Portal2RLPlugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS);
