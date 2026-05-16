#pragma once

#include <windows.h>
#include <string>

// Check GitHub for a newer release.
// On success, fills newVersion (e.g. "0.2.0") and downloadUrl (asset URL).
// Returns true when a version newer than APP_VERSION is available.
bool CheckForUpdate(std::wstring& newVersion, std::wstring& downloadUrl);

// Generic version: check any GitHub repo for a newer release.
bool CheckForUpdateGeneric(const wchar_t* repoOwner, const wchar_t* repoName,
                           const wchar_t* currentVersion,
                           std::wstring& newVersion, std::wstring& downloadUrl);

// Download the installer from downloadUrl to a temp file and launch it.
// Returns true if the installer was started successfully.
bool DownloadAndInstallUpdate(const std::wstring& downloadUrl,
                              const wchar_t* installerFilename = L"AhlbornBridge_Setup.exe");

// Interactive flow: check → prompt user → download & install → exit app.
// Shows message boxes to communicate progress/results.
// Pass silent=true to suppress the "already up to date" message (e.g. on startup).
void CheckForUpdateInteractive(HWND hParent, bool silent = false);

// Interactive flow for the Stream Deck plugin update.
void CheckForPluginUpdateInteractive(HWND hParent, bool silent = false);
