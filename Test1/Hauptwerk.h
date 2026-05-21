#pragma once
#include "Midi.h"

bool LaunchHauptwerkAndDismissWelcome();
void CloseProcessByName(const wchar_t* processName);
bool IsProcessRunningByName(const wchar_t* processName);
bool LaunchBidule();
bool SendBiduleOscOpenProfile(const std::wstring& profilePath, unsigned short port = 3210);
void CloseBiduleProcess();
std::wstring DetectBiduleExePath();
bool IsBiduleWindowVisible();
bool ShowBiduleWindow();
bool HideBiduleWindow();
bool ToggleBiduleWindowVisibility();
