// Read-only demo client capture. Never connects to Python or applies input.
#include "DemoCapture.hpp"
#include "Entity.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "Modules/Console.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
using json = nlohmann::json;
namespace DemoCapture {
namespace {
FILE *output = nullptr;
Interface *clientDll = nullptr, *input = nullptr, *entities = nullptr, *engine = nullptr;
Interface *demoPlayer = nullptr;
using Decode = int(__rescall *)(void *, int, int, int);
Decode original = nullptr;
using GetEntity = void *(__rescall *)(void *, int);
GetEntity getEntity = nullptr;
int samples = 0;

json vec(const Vector &v) { return {v.x, v.y, v.z}; }
json angles(const QAngle &v) { return {v.x, v.y, v.z}; }
void write(const json &row) {
    if (!output) return;
    auto line = row.dump();
    if (std::fprintf(output, "%s\n", line.c_str()) < 0 || std::fflush(output) != 0) {
        console->Warning("Portal2RL: demo capture write failed\n");
        std::fclose(output); output = nullptr;
    }
}
template<typename T> bool field(void *entity, const char *name, T *value) {
    auto entry = EntField::getClientOffset(entity, name);
    if (entry.second == EntField::Type::NONE || !EntField::matchFieldType<T>(entry.second)) return false;
    *value = *reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(entity) + entry.first);
    return true;
}
json snapshot(int slot) {
    void *player = getEntity(entities->ThisPtr(), slot + 1);
    if (!player) return nullptr;
    Vector pos, vel; CBaseHandle ground;
    if (!field(player,"m_vecAbsOrigin",&pos) || !field(player,"m_vecAbsVelocity",&vel) ||
        !field(player,"m_hGroundEntity",&ground)) return nullptr;
    QAngle view;
    using GetAngles = void(__rescall *)(void *, QAngle &);
    engine->Original<GetAngles>(Offsets::GetViewAngles)(engine->ThisPtr(), view);
    json portals[2] = {json{{"placed",false},{"pos",{0,0,0}}}, json{{"placed",false},{"pos",{0,0,0}}}};
    bool portalsValid = true;
    for (int i=0; i<Offsets::NUM_ENT_ENTRIES; ++i) {
        void *ent=getEntity(entities->ThisPtr(), i);
        if (!ent) continue;
        auto cc=CE(ent)->GetClientClass();
        if (!cc || !cc->m_pNetworkName || std::strcmp(cc->m_pNetworkName,"CProp_Portal")) continue;
        bool active, orange;
        if (!field(ent,"m_bActivated",&active)) { portalsValid=false; continue; }
        if (!active) continue;
        Vector portalPos;
        if (!field(ent,"m_bIsPortal2",&orange) || !field(ent,"m_vecAbsOrigin",&portalPos)) { portalsValid=false; continue; }
        portals[orange ? 1 : 0] = {{"placed",true},{"pos",vec(portalPos)}};
    }
    return {{"pos",vec(pos)},{"vel",vec(vel)},{"ang",angles(view)},
            {"on_ground",static_cast<bool>(ground)}, {"portal1",portals[0]}, {"portal2",portals[1]},
            {"portals_valid",portalsValid},{"slot",slot}};
}
int __rescall decode(void *self, int slot, int buffer, int sequence) {
    // This is a snapshot before decoding this recorded input, not a claim that
    // every interpolated client entity is the original server's pre-tick state.
    json before = snapshot(slot);
    using Tick = int(__rescall *)(void *);
    using Flag = bool(__rescall *)(void *);
    int playbackTick = demoPlayer->Original<Tick>(Offsets::GetPlaybackTick)(demoPlayer->ThisPtr());
    bool skipping = demoPlayer->Original<Flag>(Offsets::IsSkipping)(demoPlayer->ThisPtr());
    bool paused = demoPlayer->Original<Flag>(Offsets::IsPlaybackPaused)(demoPlayer->ThisPtr());
    int result = original(self,slot,buffer,sequence);
    auto commands = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(self) +
        slot * Offsets::PerUserInput_tSize + Offsets::m_pCommands);
    auto cmd = reinterpret_cast<CUserCmd *>(commands + Offsets::CUserCmdSize * (sequence % Offsets::MULTIPLAYER_BACKUP));
    write({{"type","command"},{"index",samples++},{"sequence",sequence},{"slot",slot},
           {"command_number",cmd->command_number},{"command_tick",cmd->tick_count},
           {"view_angles",angles(cmd->viewangles)},{"buttons",cmd->buttons},
           {"movement",{cmd->forwardmove,cmd->sidemove,cmd->upmove}},
           {"playback_tick",playbackTick},{"skipping",skipping},{"paused",paused},
           {"demo_name",reinterpret_cast<char *>(demoPlayer->ThisPtr()) + Offsets::m_szFileName},
           {"state",before},{"stage","before_decode_usercmd"}});
    return result;
}
}
bool Requested() { const char *p=std::getenv("PORTAL2_RL_DEMO_CAPTURE"); return p && *p; }
bool Start() {
    output=std::fopen(std::getenv("PORTAL2_RL_DEMO_CAPTURE"),"w");
    if (!output) return false;
    clientDll=Interface::Create("client" MODULE_EXTENSION,"VClient016",false);
    entities=Interface::Create("client" MODULE_EXTENSION,"VClientEntityList003",false);
    engine=Interface::Create("engine" MODULE_EXTENSION,"VEngineClient015",false);
    if (!clientDll || !entities || !engine) return false;
    using ClientState = void *(*)();
    auto clientState = Memory::Read<ClientState>(engine->Original(Offsets::ClientCmd) + Offsets::GetClientStateFunction);
    void *state = clientState();
    auto disconnect = Memory::VMT<uintptr_t>(state, Offsets::Disconnect);
    Memory::ModuleInfo module;
    if (!Memory::TryGetModule("engine" MODULE_EXTENSION, &module) ||
        disconnect < reinterpret_cast<uintptr_t>(module.base) ||
        disconnect >= reinterpret_cast<uintptr_t>(module.base) + module.size) {
        console->Warning("Portal2RL: capture must be loaded before SAR (client-state vtable already hooked)\n");
        return false;
    }
    demoPlayer = Interface::Create(Memory::DerefDeref<void *>(disconnect + Offsets::demoplayer), false);
    if (!demoPlayer) return false;
    getEntity=entities->Original<GetEntity>(Offsets::GetClientEntity);
    auto activate=clientDll->Original(Offsets::IN_ActivateMouse);
    void *ptr=Memory::DerefDeref<void *>(activate + Offsets::g_Input);
    input=Interface::Create(ptr);
    if (!input || !input->Hook(decode,original,Offsets::DecodeUserCmdFromBuffer)) return false;
    write({{"type","header"},{"schema","portal2-client-demo-capture-v1"},
           {"demo_sha256",std::getenv("PORTAL2_RL_DEMO_SHA256") ? std::getenv("PORTAL2_RL_DEMO_SHA256") : ""},
           {"state_source","replayed_client_entities"},{"stage","before_decode_usercmd"},
           {"causal_simulation_alignment_verified",false}});
    console->Print("Portal2RL: read-only client demo capture active\n");
    return true;
}
void Shutdown() {
    Interface::Delete(input); input=nullptr;
    Interface::Delete(clientDll); clientDll=nullptr;
    Interface::Delete(entities); entities=nullptr;
    Interface::Delete(engine); engine=nullptr;
    Interface::Delete(demoPlayer); demoPlayer=nullptr;
    write({{"type","end"},{"complete",true},{"command_count",samples}});
    if (output) std::fclose(output);
    output=nullptr;
}
}
