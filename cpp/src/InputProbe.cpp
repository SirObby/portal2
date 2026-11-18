// Opt-in observation of the exact CUserCmd entering server PlayerRunCommand.
#include "InputProbe.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/SDK.hpp"
#include "Modules/Console.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <deque>
#include <unordered_map>
using json = nlohmann::json;
namespace InputProbe {
namespace {
FILE *output = nullptr;
Interface *hook = nullptr;
std::vector<uintptr_t> table;
Interface *clientDll=nullptr,*clientMode=nullptr;
std::vector<uintptr_t> clientTable;
using CreateMove=int(__rescall *)(void *,float,CUserCmd *);
CreateMove createOriginal=nullptr;
std::unordered_map<int,int> issued;
using Run = int(__rescall *)(void *, CUserCmd *, void *);
Run original = nullptr;
int request = -1;
int lastConsumed=-1;
struct Pending {int id; RLInput::Action action; QAngle target;};
std::deque<Pending> pending;
bool direct() {const char *p=std::getenv("PORTAL2_RL_DIRECT_INPUT");return p && (*p=='1' || std::strcmp(p,"client")==0);}
void write(json row) {
    if (!output) return;
    row["engine_tick"] = RLEngine::gpGlobals->tickcount;
    row["latest_request"] = request;
    std::fprintf(output,"%s\n",row.dump().c_str());
    std::fflush(output);
}
json state(void *player) {
    auto p=SE(player)->abs_origin(); auto v=SE(player)->abs_velocity();
    return {{"pos",{p.x,p.y,p.z}},{"vel",{v.x,v.y,v.z}}, {"flags",SE(player)->flags()}};
}
int apply(CUserCmd *cmd) {
    int applied=-1;
        cmd->buttons=0;cmd->forwardmove=0;cmd->sidemove=0;cmd->upmove=0;
        if (!pending.empty()) {
            Pending p=pending.front();pending.pop_front();applied=p.id;
            const auto &a=p.action;
            cmd->forwardmove=175.f*a.move_forward;cmd->sidemove=175.f*a.move_side;
            cmd->viewangles=p.target;
            cmd->buttons=(a.jump?2:0)|(a.duck?4:0)|(a.use?32:0)|
                (a.fire_portal1?1:0)|(a.fire_portal2?2048:0)|
                (a.move_forward>0?8:a.move_forward<0?16:0)|
                (a.move_side>0?1024:a.move_side<0?512:0);
        }
    return applied;
}
#ifdef _WIN32
int __fastcall create(void *self,void *,float sample,CUserCmd *cmd) {
#else
int __rescall create(void *self,float sample,CUserCmd *cmd) {
#endif
    int result=createOriginal(self,sample,cmd);
    if (cmd && cmd->command_number>0) {
        int id=apply(cmd);issued[cmd->command_number]=id;
        write({{"type","created"},{"command",cmd->command_number},{"applied_request",id},
            {"buttons",cmd->buttons},{"angles",{cmd->viewangles.x,cmd->viewangles.y,cmd->viewangles.z}}});
    }
    return result;
}
#ifdef _WIN32
int __fastcall observe(void *self, void *, CUserCmd *cmd, void *helper) {
#else
int __rescall observe(void *self, CUserCmd *cmd, void *helper) {
#endif
    int clientButtons=cmd->buttons;
    int applied=-1;
    if (direct() && !ClientMode()) applied=apply(cmd);
    else if (ClientMode()) {
        auto found=issued.find(cmd->command_number);
        if(found!=issued.end()){applied=found->second;issued.erase(found);}
    }
    if (!output) {
        int result=original(self,cmd,helper);
        if(applied>=0)lastConsumed=applied;
        return result;
    }
    json before={{"type","consumed"},{"client_buttons",clientButtons},{"applied_request",applied},{"pending",pending.size()},{"command",cmd->command_number},{"command_tick",cmd->tick_count},
        {"angles",{cmd->viewangles.x,cmd->viewangles.y,cmd->viewangles.z}},
        {"move",{cmd->forwardmove,cmd->sidemove,cmd->upmove}},{"buttons",cmd->buttons},{"before",state(self)}};
    int result=original(self,cmd,helper);
    if(applied>=0)lastConsumed=applied;
    before["after"]=state(self);
    write(before);
    return result;
}
}
bool ClientMode() {const char *p=std::getenv("PORTAL2_RL_DIRECT_INPUT");return p && std::strcmp(p,"client")==0;}
bool Ready() { return !direct() || (hook && (!ClientMode() || clientMode)); }
const char *BackendName() { return ClientMode() ? "client" : (direct() ? "server" : "console"); }
int LastConsumed() {return lastConsumed;}
int InFlight() {int n=pending.size();for(auto &p:issued)if(p.second>=0)++n;return n;}
void Install(void *player) {
    const char *path=std::getenv("PORTAL2_RL_INPUT_TRACE");
    if (((!path || !*path) && !direct()) || !player || hook) return;
    if (!output && path && *path) output=std::fopen(path,"w");
    hook=Interface::Create(player,false);
    if (Offsets::PlayerRunCommand >= hook->vtableSize) {
        Interface::Delete(hook);hook=nullptr;
        console->Warning("Portal2RL: input probe PlayerRunCommand hook failed\n");
        return;
    }
    // Entity RTTI uses BOTH Itanium ABI words before the address point.
    // The generic interface helper preserves only typeinfo, which is enough
    // for engine interfaces but corrupts dynamic_cast on server entities.
#ifdef _WIN32
    constexpr int prefix=1;
#else
    constexpr int prefix=2;
#endif
    table.resize(hook->vtableSize+prefix);
    std::memcpy(table.data(),hook->vtable-prefix,table.size()*sizeof(uintptr_t));
    original=hook->Original<Run>(Offsets::PlayerRunCommand);
    table[Offsets::PlayerRunCommand+prefix]=reinterpret_cast<uintptr_t>(observe);
    *hook->baseclass=table.data()+prefix;
    if (ClientMode() && !clientMode) {
        clientDll=Interface::Create("client" MODULE_EXTENSION,"VClient016",false);
        if(!clientDll) {console->Warning("Portal2RL: VClient016 unavailable for usercmd input\n");return;}
        auto hud=clientDll->Original(Offsets::HudProcessInput);
        auto getMode=Memory::Read<uintptr_t>(hud+Offsets::GetClientMode);
        auto modeAddress=Memory::Deref<uintptr_t>(getMode+Offsets::g_pClientMode);
        clientMode=Interface::Create(Memory::Deref<void *>(modeAddress),false);
        if(!clientMode || Offsets::CreateMove>=clientMode->vtableSize) {console->Warning("Portal2RL: client CreateMove unavailable\n");return;}
        clientTable.resize(clientMode->vtableSize+prefix);
        std::memcpy(clientTable.data(),clientMode->vtable-prefix,clientTable.size()*sizeof(uintptr_t));
        createOriginal=clientMode->Original<CreateMove>(Offsets::CreateMove);
        clientTable[Offsets::CreateMove+prefix]=reinterpret_cast<uintptr_t>(create);
        *clientMode->baseclass=clientTable.data()+prefix;
    }
    write({{"type","installed"},{"slot",Offsets::PlayerRunCommand}});
}
void Request(const RLInput::Action &a, const QAngle &target) {
    ++request;
    if (direct()) pending.push_back({request,a,target});
    if(!output)return;
    write({{"type","requested"},{"movement",{a.move_forward,a.move_side}},
        {"turn",{a.pitch_delta,a.yaw_delta}},{"jump",a.jump},{"use",a.use},
        {"duck",a.duck},{"portal1",a.fire_portal1},{"portal2",a.fire_portal2}});
}
void Reset() { pending.clear();issued.clear();request=-1;lastConsumed=-1;write({{"type","reset"}}); }
void Detach() {
    if(clientMode)*clientMode->baseclass=clientMode->vtable;
    Interface::Delete(clientMode);clientMode=nullptr;clientTable.clear();
    Interface::Delete(clientDll);clientDll=nullptr;createOriginal=nullptr;
    if(hook)*hook->baseclass=hook->vtable; Interface::Delete(hook);hook=nullptr;original=nullptr;table.clear(); }
void Shutdown() { Detach(); if(output)std::fclose(output);output=nullptr; }
}
