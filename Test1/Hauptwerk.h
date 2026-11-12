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

// Enable/disable the OSC monitor: when enabled, every parsed incoming OSC
// message is logged to the debug console as a "[OSC MON]" entry.
void SetOscMonitorEnabled(bool enabled);
bool IsOscMonitorEnabled();

// A single received OSC message captured for the Settings OSC Monitor window.
struct OscMonitorEntry
{
    SYSTEMTIME   timestamp{};
    unsigned short port = 0;   // source UDP port (5000 DSP, 5001 levels)
    std::wstring address;      // e.g. "/DSP_FRONT/RvrbTime"
    std::wstring args;         // e.g. "f=0.45" or "s=\"Church\""
};

// Drains and returns the queued incoming OSC messages (thread-safe). Incoming
// messages are always queued regardless of the console-echo toggle so the
// Settings OSC Monitor window can display live traffic.
std::vector<OscMonitorEntry> PopOscMonitorEvents();
void ClearOscMonitorEvents();
