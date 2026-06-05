#include "AhlbornBridge.h"
#include "Midi.h"
#include "TrayIcon.h"
#include "Hauptwerk.h"
#include "StreamDeck.h"
#include "Xml.h"
#include "AutoUpdate.h"
#include "StreamDeck_profiler.h"
#include "Midi2Endpoint.h"
#include "Version.h"
#include <tlhelp32.h>
#include <cwchar>
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")




int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
if (FAILED(hr))
{
    // COM initialization failed, exit application
    return 0;
}

	// Single-instance guard: create a named mutex. If it already exists,
	// another instance is running and we should exit.
	const wchar_t* kInstanceMutexName = L"Local\\AhlbornBridge_InstanceMutex";
	HANDLE hInstanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
	if (hInstanceMutex == nullptr)
	{
		// Failed to create mutex, continue (best-effort).
	}
	else if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		// Another instance is running. Clean up and exit.
		CloseHandle(hInstanceMutex);
		return 0;
	}

	// -----------------------------------------------------------------------
	// Verify Hauptwerk is installed BEFORE doing anything else (no Settings
	// writes, no console, no threads).  If not found, inform the user and
	// abort immediately — they must install Hauptwerk first, then re-run setup.
	// -----------------------------------------------------------------------
	if (!InitHauptwerkPaths())
	{
		MessageBoxW(nullptr,
			L"Hauptwerk Virtual Pipe Organ was not found on this system.\n\n"
			L"Please install Hauptwerk first, then run AhlbornBridge Setup again.",
			L"AhlbornBridge \u2014 Installation Cancelled",
			MB_OK | MB_ICONERROR | MB_TOPMOST);
		if (hInstanceMutex) CloseHandle(hInstanceMutex);
		CoUninitialize();
		return 0;
	}

	// Verify Windows MIDI Services is installed.
	if (!IsMidi2ServiceInstalled())
	{
		MessageBoxW(nullptr,
			L"Windows MIDI Services was not found on this system.\n\n"
			L"AhlbornBridge requires Windows MIDI Services to create virtual MIDI ports.\n\n"
			L"Please install it from:\nhttps://aka.ms/midisrv\n\nthen run AhlbornBridge Setup again.",
			L"AhlbornBridge \u2014 Installation Cancelled",
			MB_OK | MB_ICONERROR | MB_TOPMOST);
		if (hInstanceMutex) CloseHandle(hInstanceMutex);
		CoUninitialize();
		return 0;
	}

	CloseProcessByName(L"Hauptwerk.exe");
	CloseProcessByName(L"Bidule.exe");
	CloseProcessByName(L"bidule.exe");

	// Allocate a console for a GUI application
//#ifdef _DEBUG
	if (CONSOLE_ALLOCATION) consoleAllocation();

	// Apply saved console visibility setting.
	{
		bool showConsole = false;
		if (LoadShowDebugConsole(showConsole))
		{
			HWND hConsoleWnd = GetConsoleWindow();
			if (hConsoleWnd)
			{
				ShowWindow(hConsoleWnd, showConsole ? SW_SHOW : SW_HIDE);
			}
		}
	}
//#endif

	// Hauptwerk is present: start dependent subsystems.
	StartOrganFolderWatcher(); // Monitor OrganDefinitions for installs/uninstalls
	printf("[Startup] OrganFolderWatcher started.\n");

	// Load Active Sensing settings BEFORE initMidiState so that
	// RefreshSettingsFile() inside it does not overwrite the saved value.
	{
		bool activeSensingEnabled = false;
		LoadActiveSensingEnabled(activeSensingEnabled);
		g_activeSensingEnabled.store(activeSensingEnabled);
	}

	initMidiState(); // Initialize MIDI state (includes first-launch MIDI wizard if needed)
	printf("[Startup] MIDI state initialized.\n");

	// Apply and persist the Active Sensing output name (lock is ready after initMidiState).
	{
		std::wstring activeSensingOutputName;
		LoadActiveSensingOutputName(activeSensingOutputName);
		if (activeSensingOutputName.empty())
		{
			// Try to find a suitable loopback output on this system.
			// Prefer "Default App Loopback (A)", then any port containing "(A)" or "Loopback".
			UINT numDevs = midiOutGetNumDevs();
			std::wstring candidate;
			for (UINT i = 0; i < numDevs; ++i)
			{
				MIDIOUTCAPS caps = {};
				if (midiOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
					continue;
				std::wstring name = caps.szPname;
				if (name == L"Default App Loopback (A)")
				{
					candidate = name;
					break;
				}
				if (candidate.empty() &&
					(name.find(L"(A)") != std::wstring::npos ||
					 name.find(L"Loopback") != std::wstring::npos ||
					 name.find(L"loopback") != std::wstring::npos))
				{
					candidate = name;
				}
			}
			if (!candidate.empty())
				activeSensingOutputName = candidate;
			else
				activeSensingOutputName = L"Default App Loopback (A)"; // keep as placeholder
		}
		SetActiveSensingOutputName(activeSensingOutputName);
		SaveActiveSensingOutputName(activeSensingOutputName);
	}

	// Auto-detect and persist Bidule path at startup (instead of waiting for Settings UI).
	DetectBiduleExePath();

	// Start Stream Deck pipe server AFTER MIDI init and first-launch wizard are complete,
	// and only if Stream Deck is actually installed on this system and the user has enabled it.
	{
		bool pipeServerEnabled = true;
		LoadStreamDeckPipeServerEnabled(pipeServerEnabled);

		if (pipeServerEnabled)
		{
			wchar_t sdPluginPath[MAX_PATH] = {};
			if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, sdPluginPath)))
			{
				std::wstring sdPath = std::wstring(sdPluginPath) + L"\\Elgato\\StreamDeck";
				DWORD attr = GetFileAttributesW(sdPath.c_str());
				if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
				{
					StartStreamDeckPipeServer();
				}
				else
				{
					printf("[Startup] Stream Deck not detected — pipe server not started.\n");
				}
			}
		}
		else
		{
			printf("[Startup] Stream Deck pipe server disabled in settings.\n");
		}
	}

	startsWithFe();  // Start the MIDI processing thread
	printf("[Startup] MIDI processing thread started.\n");

	const wchar_t className[] = L"AhlbornBridgeTrayWindow";
	WNDCLASSW wc = {};
	wc.lpfnWndProc = TrayIconWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = className;

	if (!RegisterClassW(&wc))
		return 0;

	HWND hWnd = CreateWindowExW(
		0,
		className,
		L"AhlbornBridge",
		0,
		0, 0, 0, 0,
		nullptr,
		nullptr,
		hInstance,
		nullptr);

	if (!hWnd)
		return 0;

	if (!CreateTrayIcon(hInstance, hWnd))
		return 0;

	{
		std::wstring currentVersion =
			std::to_wstring(APP_VERSION_MAJOR) + L"." +
			std::to_wstring(APP_VERSION_MINOR) + L"." +
			std::to_wstring(APP_VERSION_PATCH);

		std::wstring lastSeenVersion;
		LoadLastSeenAppVersion(lastSeenVersion);
		if (lastSeenVersion != currentVersion)
		{
			std::wstring whatsNew = LoadChangelogForVersion(currentVersion);
				if (whatsNew.empty())
					whatsNew = L"See CHANGELOG.md in the installation folder for details.";
				std::wstring title = L"What's new in " + currentVersion;
				MessageBoxW(hWnd, whatsNew.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
			SaveLastSeenAppVersion(currentVersion);
		}

		// First launch only: if Settings.xml does not exist yet, open Settings
		// so the user can pick the correct MIDI ports.
		std::wstring settingsPath = GetSettingsDirPath() + L"\\Settings.xml";
		DWORD attrs = GetFileAttributesW(settingsPath.c_str());
		bool settingsFileExists = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
		if (!settingsFileExists)
		{
			printf("Settings.xml not found (first launch). Opening Settings window.\n");
			ShowSettingsWindow(hInstance, hWnd);
		}

		// Check for updates on start if enabled.
		// Run on a background thread so a slow/absent network connection
		// does not block the main message loop at startup.
		bool checkForUpdateOnStart = true;
		LoadCheckForUpdateOnStart(checkForUpdateOnStart);
		if (checkForUpdateOnStart)
		{
			HANDLE hUpdateThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
					// Aspetta fino a 60 secondi che la rete sia disponibile (es. WiFi lento all'avvio).
					constexpr int kMaxRetries = 6;
					constexpr DWORD kRetryIntervalMs = 10000; // 10 secondi
					for (int i = 0; i < kMaxRetries; ++i)
					{
						Sleep(kRetryIntervalMs);
						DWORD flags = 0;
						if (InternetGetConnectedState(&flags, 0))
							break;
						printf("AutoUpdate: rete non disponibile, tentativo %d/%d...\n", i + 1, kMaxRetries);
					}
					CheckForUpdateInteractive(reinterpret_cast<HWND>(param), true);
					return 0;
				}, hWnd, 0, nullptr);
				if (hUpdateThread) CloseHandle(hUpdateThread);
		}
	}

	// Ctrl+Shift+F12 clears the debug console.
	const int kHotkeyIdClearConsole = 1;
	if (!RegisterHotKey(hWnd, kHotkeyIdClearConsole, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12))
		printf("RegisterHotKey Ctrl+Shift+F12 FAILED (error %lu)\n", GetLastError());
	else
		printf("Ctrl+Shift+F12 hotkey registered for console clear.\n");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

#ifdef _DEBUG
    if (CONSOLE_ALLOCATION)
        FreeConsole();
#endif

    if (hInstanceMutex)
        CloseHandle(hInstanceMutex);
    
    CoUninitialize();
    return 0;
}

void consoleAllocation()
{
    if (AllocConsole())
    {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        FILE* fp;
        freopen_s(&fp, "CONIN$", "r", stdin);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio(true);
        std::cout.clear();
        std::cin.clear();
    }
}
