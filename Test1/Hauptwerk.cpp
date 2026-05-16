#include "Hauptwerk.h"
#include "StreamDeck.h"
#include "Qt.h"
#include "Xml.h"
#include "TrayIcon.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <shlobj.h>
#include <knownfolders.h>


namespace
{
    struct WindowSearchContext
    {
        DWORD processId;
        HWND hwnd;
    };

    // Return true for plain top-level windows belonging to a process.
    // The original implementation also required the window to have
    // child windows, but recent Hauptwerk versions may not create
    // child windows on the main frame, which caused the search for
    // the main window to fail. We now only enforce that the window is
    // valid and not owned/child.
    bool IsTopLevelWithChild(HWND hWnd)
    {
        if (!hWnd || !IsWindow(hWnd))
        {
            return false;
        }

        if (GetWindow(hWnd, GW_OWNER) != nullptr || GetParent(hWnd) != nullptr)
        {
            return false;
        }

        return true;
    }

    BOOL CALLBACK FindHauptwerkWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto *context = reinterpret_cast<WindowSearchContext *>(lParam);
        if (!context)
        {
            return TRUE;
        }

        if (!IsTopLevelWithChild(hwnd))
        {
            return TRUE;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != context->processId)
        {
            return TRUE;
        }

        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
        if (title[0] == L'\0')
        {
            return TRUE;
        }

        // Accept both the plain "Hauptwerk" title and titles starting
        // with "Hauptwerk -" (when an organ is loaded), so we can
        // always bind to the main application window.
        if (_wcsicmp(title, L"Hauptwerk") == 0 || wcsncmp(title, L"Hauptwerk -", 11) == 0)
        {
            context->hwnd = hwnd;
            return FALSE;
        }

        return TRUE;
    }

    HWND FindHauptwerkMainWindow(DWORD processId)
    {
        unsigned int attempts = 0;
        while (true)
        {
            ++attempts;
            WindowSearchContext context{ processId, nullptr };
            EnumWindows(FindHauptwerkWindowProc, reinterpret_cast<LPARAM>(&context));
            if (context.hwnd)
            {
                printf("Hauptwerk window found after %u attempt(s).\n", attempts);
                return context.hwnd;
            }

            Sleep(200);
        }
    }

    // Return the process ID for the first process whose executable name
    // matches the given name (case-insensitive). Returns 0 if not found.
    DWORD FindProcessId(const wchar_t* processName)
    {
        if (!processName || !*processName)
        {
            return 0;
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, processName) == 0)
                {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return pid;
    }

    bool IsProcessRunning(const wchar_t *processName)
    {
        return FindProcessId(processName) != 0;
    }
}


    void CloseProcessByName(const wchar_t* processName)
    {
        if (!processName || !*processName)
        {
            return;
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, processName) == 0)
                {
                    HANDLE processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                    if (processHandle)
                    {
                        TerminateProcess(processHandle, 0);
                        CloseHandle(processHandle);
                    }
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
    }

bool IsProcessRunningByName(const wchar_t* processName)
{
    return IsProcessRunning(processName);
}

bool LaunchHauptwerkAndDismissWelcome()
{
	// If Hauptwerk is already running (e.g., started manually by the
	// user), locate its main window and wire it into the global handle
	// so that StartHauptwerkTitleMonitor can still track its title and
	// update the tray icon correctly.
	
    if (g_hauptwerkMainWindow) return 0;
    
    if (IsProcessRunning(L"Hauptwerk.exe"))
	{
		DWORD pid = FindProcessId(L"Hauptwerk.exe");
		if (pid != 0)
		{
			HWND mainWindow = FindHauptwerkMainWindow(pid);
			if (mainWindow)
			{
				printf("Hauptwerk already running, main window detected.\n");
				printf("Hauptwerk window HWND ID = %p\n", mainWindow);
				g_hauptwerkMainWindow = mainWindow;
				printf("g_hauptwerkMainWindow ID = %p\n", g_hauptwerkMainWindow.load());
					ReloadStandbyOrgans();
					// When invoked from the tray icon while Hauptwerk is
					// already running, bring the existing window to the
					// foreground so the user sees an effect.
					ShowWindow(mainWindow, SW_RESTORE);
					SetForegroundWindow(mainWindow);
			}
		}
		// Return false so callers know we didn't launch a new instance;
		// the title monitor thread will still work with the discovered
		// window handle.
		return false;
	}

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Build the executable path from the configured Hauptwerk application folder.
    std::wstring appPath;
    if (!LoadHauptwerkAppPath(appPath) || appPath.empty())
        appPath = L"C:\\Program Files\\Hauptwerk Virtual Pipe Organ";
    std::wstring exePath = appPath + L"\\Hauptwerk.exe";
    // CreateProcessW requires a writable buffer for the command line.
    std::vector<wchar_t> cmdLine(exePath.begin(), exePath.end());
    cmdLine.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        printf("Failed to start Hauptwerk.exe (error %lu).\n", GetLastError());
        StopTrayIconFlashing();
        UpdateTrayIconTooltip(L"Hauptwerk non installato!");
        // Build error icon path from AppData\Roaming.
        {
            PWSTR roaming = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming)
            {
                std::wstring errIcon(roaming);
                CoTaskMemFree(roaming);
                errIcon += L"\\AhlbornBridge\\Icons\\A_Error.png";
                UpdateTrayIconFromFile(errIcon.c_str());
            }
        }
        g_trayIconImageStatus = TrayIconImageStatus::Error;
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    while (true)
    {
        HWND welcome = FindWindowW(nullptr, L"Welcome to Hauptwerk");
        if (welcome)
        {
            ShowWindow(welcome, SW_SHOW);

            DWORD welcomeThreadId = GetWindowThreadProcessId(welcome, nullptr);
            DWORD currentThreadId = GetCurrentThreadId();
            bool attached = false;
            if (welcomeThreadId != 0 && welcomeThreadId != currentThreadId)
            {
                attached = AttachThreadInput(currentThreadId, welcomeThreadId, TRUE) != FALSE;
            }

            SetForegroundWindow(welcome);
            SetActiveWindow(welcome);
            SetFocus(welcome);

            if (attached)
            {
                AttachThreadInput(currentThreadId, welcomeThreadId, FALSE);
            }

            Sleep(50);
            PostMessageW(welcome, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(welcome, WM_KEYUP, VK_RETURN, 0);
            printf("Sent ENTER to Welcome to Hauptwerk.\n");
            break;
        }

        Sleep(200);
    }

    HWND mainWindow = FindHauptwerkMainWindow(pi.dwProcessId);
    if (!mainWindow)
    {
        printf("Hauptwerk main window not detected or missing child windows.\n");
        return false;
    }

    printf("Hauptwerk main window detected with child windows.\n");
	printf("Hauptwerk window HWND ID = %p\n", mainWindow);
	g_hauptwerkMainWindow = mainWindow;
	printf("g_hauptwerkMainWindow ID = %p\n", g_hauptwerkMainWindow.load());

	ShowWindow(mainWindow, SW_RESTORE);
	SetForegroundWindow(mainWindow);

	ReloadStandbyOrgans();

	// --- Reset Stream Deck buttons
	printf("\nFrom Hauptwerk.cpp line %d: Sending MIDI message to reset Stream Deck buttons...\n", __LINE__);
    SendUnloadOrganMidiMessage();

    // --- Mostra menu disponibili dell'applicazione Hauptwerk ---
    std::wcout << L"\n========================================\n";
    std::wcout << L"  Menu disponibili nella MenuBar:\n";
    std::wcout << L"========================================\n";

    IAccessible* pRoot = nullptr;
    AccessibleObjectFromWindow(g_hauptwerkMainWindow, OBJID_CLIENT, IID_IAccessible, (void**)&pRoot);
    if (pRoot)
    {
        IAccessible* menuBar = FindChildByRoleAndName(pRoot, ROLE_SYSTEM_MENUBAR, nullptr, 2);
        if (menuBar)
        {
            long mbChildren = 0;
            menuBar->get_accChildCount(&mbChildren);
            if (mbChildren > 0)
            {
                ListChildren(menuBar);
            }
            menuBar->Release();
        }
        pRoot->Release();
    }


    return true;
}
