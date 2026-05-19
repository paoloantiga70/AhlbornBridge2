#pragma once

#include <windows.h>

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_SETTINGS = 1001;
constexpr UINT ID_TRAY_EXIT = 1002;
constexpr UINT ID_TRAY_UPDATE = 1003;
constexpr UINT ID_TRAY_TOGGLE_CONSOLE  = 1004;
constexpr UINT ID_TRAY_MIDI_ASSIGN     = 1005;


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
bool UpdateTrayIconFromFile(const wchar_t* iconPath);
bool UpdateTrayIconTooltip(const wchar_t* tooltip);
void CloseSettingsWindowIfAutoClose();
void NotifyOrganInfoTitleChanged();
