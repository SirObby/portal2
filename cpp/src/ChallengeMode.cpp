// Optional Challenge Mode matching leaderboard demonstrations, without SAR.
// ApplyGameSettings resets sv_bonus_challenge; restore it after that call,
// matching SAR's CM TAS initialization. A real finish requires the native
// challenge node's real StartTouch event for the local player (SAR predicate).
#include "ChallengeMode.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/SDK.hpp"
#include "Modules/Console.hpp"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <link.h>
#endif
namespace RLChallenge {
namespace {
Interface *cvar=nullptr;
ConVar *bonus=nullptr;
bool finished=false;
using Apply=int(__rescall *)(void *,void *);
Apply originalApply=nullptr;
using Touch=int(__rescall *)(void *,void *);
std::vector<void *> nodes;
Touch originalTouch=nullptr;
Memory::Patch touchPatch;
Interface *hud=nullptr;
std::vector<uintptr_t> hudTable;
ConCommand *leaderboard=nullptr;
_CommandCallback leaderboardOriginal=nullptr;
bool *sarCheated=nullptr;
bool hudViaSar=false;
void noLeaderboard(const CCommand &) {}
#ifdef _WIN32
const char *__fastcall hiddenName(void *,void *) {return "";}
bool __fastcall hiddenDraw(void *,void *) {return false;}
#else
const char *__rescall hiddenName(void *) {return "";}
bool __rescall hiddenDraw(void *) {return false;}
#endif
void Enable() {
    if (!bonus) return;
    auto set=Memory::VMT<_InternalSetIntValue>(bonus,Offsets::InternalSetIntValue);
    set(bonus,1);
}
#ifdef _WIN32
int __fastcall apply(void *self,void *,void *settings) {
#else
int __rescall apply(void *self,void *settings) {
#endif
    int result=originalApply(self,settings);Enable();return result;
}
bool completed(void *node,int slot) {
#ifdef _WIN32
    constexpr int offset=0x4B0;
#else
    constexpr int offset=0x4C8;
#endif
    return *(reinterpret_cast<unsigned char *>(node)+offset+slot)!=0;
}
#ifdef _WIN32
int __fastcall touch(void *self,void *,void *entity) {
#else
int __rescall touch(void *self,void *entity) {
#endif
    BlockSarSubmission();
    bool candidate=entity==RLEngine::GetLocalPlayer() && !completed(self,2) && !completed(self,0);
    if(candidate && Enabled()==1) {
        finished=true;
        console->Print("Portal2RL: native Challenge Mode finish confirmed\n");
    }
    // The engine can dispatch this through a stored member-function pointer,
    // so a vtable replacement alone misses it. Match SAR's function hook.
    touchPatch.Restore();
    int result=originalTouch(self,entity);
    touchPatch.Execute();
    return result;
}
}
bool Requested() {const char *p=std::getenv("PORTAL2_RL_CHALLENGE_MODE");return p && std::strcmp(p,"1")==0;}
bool Init() {
    if(!Requested())return true;
#ifdef _WIN32
    const char *module="vstdlib.dll";
#else
    const char *module="libvstdlib.so";
#endif
    cvar=Interface::Create(module,"VEngineCvar007",false);
    if(!cvar)return false;
    auto find=cvar->Original<_FindCommandBase>(Offsets::FindCommandBase);
    auto base=static_cast<ConCommandBase *>(find(cvar->ThisPtr(),"sv_bonus_challenge"));
    if(!base || base->IsCommand())return false;
    bonus=static_cast<ConVar *>(base);
    bool ok=RLEngine::g_ServerGameDLL->Hook(apply,originalApply,Offsets::ApplyGameSettings);
    Enable();return ok;
}
void LevelInit() {if(!Requested())return;finished=false;Enable();}
int Enabled() {return bonus ? bonus->m_pParent->m_nValue : 0;}
int Nodes() {return nodes.size();}
bool Finished() {return finished;}
float Timer() {void *player=RLEngine::GetLocalPlayer();return Requested() && player ? SE(player)->field<float>("fNumSecondsTaken") : 0.f;}
bool HudHidden() {return hud!=nullptr || hudViaSar;}
bool HideHud() {
    if(!Requested())return true;
    if(HudHidden())return true;
    if(!cvar)return false;
    auto find=cvar->Original<_FindCommandBase>(Offsets::FindCommandBase);
    auto base=static_cast<ConCommandBase *>(find(cvar->ThisPtr(),"+leaderboard"));
    if(!base || !base->IsCommand())return false;
    auto command=static_cast<ConCommand *>(base);
    // SAR already wraps +leaderboard. Never decode that wrapper using the
    // native callback's machine-code offsets; verify its own hide settings.
    if(BlockSarSubmission()) {
        auto stats=static_cast<ConCommandBase *>(find(cvar->ThisPtr(),"sar_disable_challenge_stats_hud"));
        auto portal=static_cast<ConCommandBase *>(find(cvar->ThisPtr(),"sar_portalgun_hud"));
        if(!stats || !portal || stats->IsCommand() || portal->IsCommand())return false;
        if(static_cast<ConVar *>(stats)->m_pParent->m_nValue!=-1 || static_cast<ConVar *>(portal)->m_pParent->m_nValue!=0)return false;
        hudViaSar=true;
        leaderboard=command;leaderboardOriginal=command->m_fnCommandCallback;
        command->m_fnCommandCallback=noLeaderboard;
        RLEngine::QueueConsoleCommand("-leaderboard\n");
        console->Print("Portal2RL: verified SAR CM HUD suppression; leaderboard blocked\n");
        return true;
    }
    auto callback=reinterpret_cast<uintptr_t>(command->m_fnCommandCallback);
    using GetHud=void *(__cdecl *)(int);
    using FindElement=void *(__rescall *)(void *,const char *);
    auto getHud=Memory::Read<GetHud>(callback+Offsets::GetHud);
    auto findElement=Memory::Read<FindElement>(callback+Offsets::FindElement);
    if(!getHud || !findElement)return false;
    void *element=findElement(getHud(-1),"CHUDChallengeStats");
    if(!element)return false;
    hud=Interface::Create(element,false);
    if(Offsets::GetName>=hud->vtableSize || Offsets::ShouldDraw>=hud->vtableSize){Interface::Delete(hud);hud=nullptr;return false;}
#ifdef _WIN32
    constexpr int prefix=1;
#else
    constexpr int prefix=2;
#endif
    hudTable.resize(hud->vtableSize+prefix);
    std::memcpy(hudTable.data(),hud->vtable-prefix,hudTable.size()*sizeof(uintptr_t));
    hudTable[Offsets::GetName+prefix]=reinterpret_cast<uintptr_t>(hiddenName);
    hudTable[Offsets::ShouldDraw+prefix]=reinterpret_cast<uintptr_t>(hiddenDraw);
    *hud->baseclass=hudTable.data()+prefix;
    leaderboard=command;leaderboardOriginal=command->m_fnCommandCallback;
    command->m_fnCommandCallback=noLeaderboard;
    RLEngine::QueueConsoleCommand("-leaderboard\n");
    console->Print("Portal2RL: Challenge Mode stats HUD and leaderboard suppressed\n");
    return true;
}
bool BlockSarSubmission() {
    if(!sarCheated) {
        // The vendored module inventory is cached before SAR is loaded.
        // Query the loader's current inventory, without loading a new DLL.
        void *module=nullptr;
#ifdef _WIN32
        module=GetModuleHandleA("sar.dll");
#else
        dl_iterate_phdr([](dl_phdr_info *info,size_t,void *data) {
            const char *leaf=std::strrchr(info->dlpi_name,'/');
            leaf=leaf ? leaf+1 : info->dlpi_name;
            if(std::strcmp(leaf,"sar.so"))return 0;
            *static_cast<void **>(data)=dlopen(info->dlpi_name,RTLD_NOLOAD|RTLD_NOW);
            return 1;
        },&module);
#endif
        if(!module)return false;
        sarCheated=Memory::GetSymbolAddress<bool *>(module,"_ZN10AutoSubmit9g_cheatedE");
        Memory::CloseModuleHandle(module);
        if(sarCheated)console->Print("Portal2RL: SAR autosubmission blocked using its cheated-run guard\n");
    }
    if(sarCheated)*sarCheated=true;
    return sarCheated && *sarCheated;
}
void InstallNodeHooks() {
    if(!Requested() || !RLEngine::gpGlobals || !RLEngine::m_EntPtrArray)return;
    for(int i=0;i<RLEngine::gpGlobals->maxEntities;++i) {
        void *ent=RLEngine::m_EntPtrArray[i].m_pEntity;if(!ent)continue;
        char *name=SE(ent)->classname();if(!name || std::strcmp(name,"challenge_mode_end_node"))continue;
        bool found=false;for(void *node:nodes)if(node==ent)found=true;
        if(found)continue;
        if(!originalTouch) {
            originalTouch=Memory::VMT<Touch>(ent,Offsets::StartTouch);
            unsigned char jump[5]={0xE9,0,0,0,0};
            int32_t relative=static_cast<int32_t>(reinterpret_cast<uintptr_t>(touch)-reinterpret_cast<uintptr_t>(originalTouch)-5);
            std::memcpy(jump+1,&relative,sizeof(relative));
            if(!touchPatch.Execute(reinterpret_cast<uintptr_t>(originalTouch),jump,sizeof(jump))) {originalTouch=nullptr;continue;}
        }
        auto p=SE(ent)->abs_origin();console->Print("Portal2RL: challenge finish node %.3f %.3f %.3f\n",p.x,p.y,p.z);
        nodes.push_back(ent);
    }
}
void Detach() {touchPatch.Restore();originalTouch=nullptr;nodes.clear();}
void Shutdown() {Detach();if(hud){*hud->baseclass=hud->vtable;Interface::Delete(hud);hud=nullptr;hudTable.clear();}if(leaderboard && leaderboard->m_fnCommandCallback==noLeaderboard)leaderboard->m_fnCommandCallback=leaderboardOriginal;leaderboard=nullptr;leaderboardOriginal=nullptr;hudViaSar=false;if(originalApply)RLEngine::g_ServerGameDLL->Unhook(Offsets::ApplyGameSettings);originalApply=nullptr;Interface::Delete(cvar);cvar=nullptr;bonus=nullptr;sarCheated=nullptr;}
}
