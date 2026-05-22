#pragma once
#include "Midi.h"

bool LaunchHauptwerkAndDismissWelcome();
void CloseProcessByName(const wchar_t* processName);
bool IsProcessRunningByName(const wchar_t* processName);
bool LaunchBidule();
bool SendBiduleOscOpenProfile(const std::wstring& profilePath, unsigned short port = 3210);
bool SendBiduleOscFileSaveAs(const std::wstring& profilePath, unsigned short port = 3210);
bool SendBiduleOscStringCommand(const std::wstring& address, const std::wstring& value, unsigned short port = 3210);
void CloseBiduleProcess();
std::wstring DetectBiduleExePath();
bool IsBiduleWindowVisible();
bool EnsureBiduleWindowReady(unsigned int timeoutMs = 10000);
bool ShowBiduleWindow();
bool HideBiduleWindow();
bool ToggleBiduleWindowVisibility();
