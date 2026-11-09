#pragma once
#include "Midi.h"

bool LaunchHauptwerkAndDismissWelcome();
void CloseProcessByName(const wchar_t* processName);
bool IsProcessRunningByName(const wchar_t* processName);
bool LaunchBidule();
bool SendBiduleOscOpenProfile(const std::wstring& profilePath, unsigned short port = 3210);
bool SendBiduleOscFileSaveAs(const std::wstring& profilePath, unsigned short port = 3210);
bool SendBiduleOscPlay(bool play, unsigned short port = 3210);
bool SendBiduleOscStringCommand(const std::wstring& address, const std::wstring& value, unsigned short port = 3210);
bool SendBiduleOscFloatCommand(const std::wstring& address, float value, unsigned short port = 3210);
void CloseBiduleProcess();
std::wstring DetectBiduleExePath();
bool IsBiduleWindowVisible();
bool EnsureBiduleWindowReady(unsigned int timeoutMs = 10000);
bool ShowBiduleWindow();
bool HideBiduleWindow();
bool ToggleBiduleWindowVisibility();
// Reads current priority settings and applies them to Hauptwerk immediately.
void ApplyHauptwerkPriorityNow();

// Incoming OSC listeners: DSP parameters on UDP 5000, audio levels on UDP 5001.
// Each runs in its own thread; safe to call Start/Stop once.
void StartOscListeners();
void StopOscListeners();

// Arms a one-shot trigger so that the next OSC datagram received on the DSP
// port (5000) causes a single "/play true" to be sent to Bidule. This is used
// after loading an organ/profile: the first incoming parameter burst from the
// freshly loaded Bidule profile proves the profile is ready, which is the exact
// moment to start the audio engine. The trigger disarms itself after firing.
void ArmBidulePlayOnNextDspPacket();
