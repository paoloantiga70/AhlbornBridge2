#pragma once

#include <windows.h>

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_SETTINGS = 1001;
constexpr UINT ID_TRAY_EXIT = 1002;
constexpr UINT ID_TRAY_UPDATE = 1003;


bool CreateTrayIcon(HINSTANCE hInstance, HWND hWnd);
void RemoveTrayIcon(HWND hWnd);
void ShowTrayMenu(HWND hWnd, POINT pt);
LRESULT CALLBACK TrayIconWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowSettingsWindow(HINSTANCE hInstance, HWND hOwner);
void PopulateMidiInputs(HWND hCombo);
void PopulateMidiOutputs(HWND hCombo);
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
bool UpdateTrayIconFromFile(const wchar_t* iconPath);
bool UpdateTrayIconTooltip(const wchar_t* tooltip);
void CloseSettingsWindowIfAutoClose();
void NotifyOrganInfoTitleChanged();
