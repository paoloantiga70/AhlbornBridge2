#include "AhlbornBridge.h"
#include "Midi.h"
#include "TrayIcon.h"
#include "Hauptwerk.h"
#include "StreamDeck.h"
#include "Xml.h"
#include "AutoUpdate.h"
#include "StreamDeck_profiler.h"
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

    CloseProcessByName(L"Hauptwerk.exe");
   
    // Allocate a console for a GUI application
//#ifdef _DEBUG
	if (CONSOLE_ALLOCATION) consoleAllocation();

	// Apply saved console visibility setting.
	{
		bool showConsole = true;
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
	bool hauptwerkPathsOk = InitHauptwerkPaths(); // Detect / configure Hauptwerk folders
	if (hauptwerkPathsOk)
	{
		StartOrganFolderWatcher(); // Monitor OrganDefinitions for installs/uninstalls
		printf("[Startup] OrganFolderWatcher started.\n");
	}
	else
	{
		printf("[Startup] Hauptwerk paths not configured, OrganFolderWatcher not started.\n");
	}
	StartStreamDeckPipeServer(); // Named pipe IPC for Stream Deck plugin
	initMidiState(); // Initialize MIDI state
	printf("[Startup] MIDI state initialized.\n");
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

	if (!hauptwerkPathsOk)
	{
		UpdateTrayIconTooltip(L"Hauptwerk not installed!");
		PWSTR roaming = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming)
		{
			std::wstring errIcon(roaming);
			CoTaskMemFree(roaming);
			errIcon += L"\\AhlbornBridge\\Icons\\A_Error.png";
			UpdateTrayIconFromFile(errIcon.c_str());
		}
		g_trayIconImageStatus = TrayIconImageStatus::Error;

		MessageBoxW(hWnd,
			L"Hauptwerk Virtual Pipe Organ was not found on this system.\n"
			L"Please verify it is installed correctly and restart AhlbornBridge.",
			L"AhlbornBridge",
			MB_OK | MB_ICONERROR);
	}
	else
	{
		// First launch: if no MIDI input device is configured yet, open
		// Settings automatically so the user can pick the correct ports.
		UINT dummyId = 0;
		if (!LoadSelectedDeviceId(dummyId))
		{
			printf("No MIDI device configured in Settings.xml. Opening Settings window.\n");
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

	/*
	// F12 clears the debug console.
    const int kHotkeyIdClearConsole = 1;
    if (!RegisterHotKey(hWnd, kHotkeyIdClearConsole, 0, VK_F12))
        printf("RegisterHotKey F12 FAILED (error %lu)\n", GetLastError());
    else
        printf("F12 hotkey registered for console clear.\n");
    */

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
        FILE* fp;
        freopen_s(&fp, "CONIN$", "r", stdin);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio(true);
        std::cout.clear();
        std::cin.clear();
    }
}
