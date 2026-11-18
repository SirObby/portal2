#pragma once
namespace RLChallenge {
bool Requested();
bool Init();
void LevelInit();
void InstallNodeHooks();
void Detach();
void Shutdown();
int Enabled();
int Nodes();
bool Finished();
float Timer();
bool HideHud();
bool HudHidden();
bool BlockSarSubmission();
}
