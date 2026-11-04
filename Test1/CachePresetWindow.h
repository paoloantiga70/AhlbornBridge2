#pragma once
#include <windows.h>

// Opens the Hauptwerk cache preset manager window (or brings it to front if
// already open). Lets the user snapshot the active organ cache into numbered
// presets and restore them, using fast renames (never copies) because the
// cache files are several GB.
void ShowCachePresetWindow(HINSTANCE hInstance, HWND hOwner);
