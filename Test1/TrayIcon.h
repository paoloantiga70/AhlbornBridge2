#pragma once

#include <windows.h>

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_SETTINGS = 1001;
constexpr UINT ID_TRAY_EXIT = 1002;
constexpr UINT ID_TRAY_UPDATE = 1003;
constexpr UINT ID_TRAY_TOGGLE_CONSOLE  = 1004;
constexpr UINT ID_TRAY_MIDI_ASSIGN     = 1005;
constexpr UINT ID_TRAY_TOGGLE_ACTIVE_SENSING = 1006;
constexpr UINT ID_TRAY_TOGGLE_BIDULE = 1007;
constexpr UINT ID_TRAY_BIDULE_OSC_TEST = 1008;


bool CreateTrayIcon(HINSTANCE hInstance, HWND hWnd);
void RemoveTrayIcon(HWND hWnd);
void ShowTrayMenu(HWND hWnd, POINT pt);
LRESULT CALLBACK TrayIconWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowSettingsWindow(HINSTANCE hInstance, HWND hOwner);
void PopulateMidiInputs(HWND hCombo);
void PopulateMidiOutputs(HWND hCombo);
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
// Show a fade-out splash window with ahlborn_started.png for a few seconds.
void ShowAhlbornStartedSplash();
// Show a fade-out splash window with ahlborn_closed.png for a few seconds.
void ShowAhlbornClosedSplash();
// Show a short overlay splash for VST Link mode.
void ShowVstLinkModeSplash();
// Show a short text splash while Hauptwerk is restarting after an audio-device change.
void ShowHauptwerkRestartSplash(const wchar_t* deviceName);
// Update the restart splash progress (0..100) while Hauptwerk restart steps complete.
void UpdateHauptwerkRestartSplashProgress(int percent);
// Close the restart splash when Hauptwerk is ready again or the restart fails.
void CloseHauptwerkRestartSplash();
bool UpdateTrayIconFromFile(const wchar_t* iconPath);
bool UpdateTrayIconTooltip(const wchar_t* tooltip);
void CloseSettingsWindowIfAutoClose();
void NotifyOrganInfoTitleChanged();
bool EnsureBiduleVisibleViaTrayToggle();
