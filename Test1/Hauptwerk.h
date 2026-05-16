#pragma once
#include "Midi.h"

bool LaunchHauptwerkAndDismissWelcome();
void CloseProcessByName(const wchar_t* processName);
bool IsProcessRunningByName(const wchar_t* processName);
