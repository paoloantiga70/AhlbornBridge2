#include "Hauptwerk.h"
#include "StreamDeck.h"
#include "Qt.h"
#include "Xml.h"
#include "TrayIcon.h"
#include "StreamDeck_profiler.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <chrono>
#include <mutex>
#include <cmath>
#include <knownfolders.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winsvc.h>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "Ws2_32.lib")

extern bool isOrganLoaded;

namespace
{
    struct WindowSearchContext
    {
        DWORD processId;
        HWND hwnd;
    };

    std::atomic<bool> g_hauptwerkRealtimeEnforcerRunning{ false };

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

    struct BiduleWindowEnumContext
    {
        DWORD pid = 0;
        HWND firstMainWindow = nullptr;
        HWND firstVisibleMainWindow = nullptr;
        HWND largestMainWindow = nullptr;
        HWND largestVisibleMainWindow = nullptr;
        LONG largestArea = 0;
        LONG largestVisibleArea = 0;
        bool foundCandidate = false;
    };

    BOOL CALLBACK EnumBiduleWindowsProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<BiduleWindowEnumContext*>(lParam);
        if (!ctx || !IsWindow(hwnd))
            return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr || GetParent(hwnd) != nullptr)
            return TRUE;

        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0 || processId != ctx->pid)
            return TRUE;

        ctx->foundCandidate = true;

        if (GetWindow(hwnd, GW_CHILD) == nullptr)
            return TRUE;

        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
        if (title[0] == L'\0')
            return TRUE;

        if (!ctx->firstMainWindow)
            ctx->firstMainWindow = hwnd;

        RECT rc{};
        if (GetWindowRect(hwnd, &rc))
        {
            LONG width = rc.right - rc.left;
            LONG height = rc.bottom - rc.top;
            LONG area = (width > 0 && height > 0) ? (width * height) : 0;
            if (area > ctx->largestArea)
            {
                ctx->largestArea = area;
                ctx->largestMainWindow = hwnd;
            }

            if (IsWindowVisible(hwnd) && area > ctx->largestVisibleArea)
            {
                ctx->largestVisibleArea = area;
                ctx->largestVisibleMainWindow = hwnd;
            }
        }

        if (IsWindowVisible(hwnd) && !ctx->firstVisibleMainWindow)
            ctx->firstVisibleMainWindow = hwnd;

        return TRUE;
    }

    static void DisableWindowCloseButton(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        HMENU hSysMenu = GetSystemMenu(hwnd, FALSE);
        if (hSysMenu)
        {
            EnableMenuItem(hSysMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
        }
    }

    static void EnableWindowCloseButton(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        GetSystemMenu(hwnd, TRUE); // Revert to standard menu with SC_CLOSE enabled
    }

    static std::atomic<bool> g_biduleMinimizeWatcherRunning{ false };
    static std::atomic<HWND> g_watchedBiduleWindow{ nullptr };

    static void StartBiduleMinimizeWatcher(HWND hwnd)
    {
        g_watchedBiduleWindow.store(hwnd);
        if (g_biduleMinimizeWatcherRunning.exchange(true))
            return; // watcher already running

        HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            while (g_biduleMinimizeWatcherRunning.load())
            {
                Sleep(200);
                HWND target = g_watchedBiduleWindow.load();
                if (!target || !IsWindow(target))
                {
                    // Check if Bidule is still running; if not, exit watcher
                    if (!IsProcessRunningByName(L"bidule.exe") && !IsProcessRunningByName(L"Bidule.exe"))
                        break;
                    continue;
                }

                if (IsIconic(target)) // User clicked Minimize
                {
                    printf("[Bidule] Minimize detected on HWND=%p -> hiding window completely.\n", target);
                    HideBiduleWindow();
                    // Once hidden, reset watched window until next ShowBiduleWindow
                    g_watchedBiduleWindow.store(nullptr);
                }
            }
            g_biduleMinimizeWatcherRunning.store(false);
            return 0;
        }, nullptr, 0, nullptr);

        if (hThread)
            CloseHandle(hThread);
    }

    HWND FindPrimaryBiduleWindow(DWORD bidulePid, bool preferVisible)
    {
        BiduleWindowEnumContext ctx{};
        ctx.pid = bidulePid;
        EnumWindows(EnumBiduleWindowsProc, reinterpret_cast<LPARAM>(&ctx));

        if (preferVisible)
        {
            if (ctx.largestVisibleMainWindow)
                return ctx.largestVisibleMainWindow;
            if (ctx.firstVisibleMainWindow)
                return ctx.firstVisibleMainWindow;
        }

        if (ctx.largestMainWindow)
            return ctx.largestMainWindow;
        return ctx.firstMainWindow;
    }

    bool HasAnyBiduleTopLevelWindow(DWORD bidulePid)
    {
        BiduleWindowEnumContext ctx{};
        ctx.pid = bidulePid;
        EnumWindows(EnumBiduleWindowsProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.foundCandidate;
    }

    std::wstring TrimPipeResponse(const std::wstring& input)
    {
        size_t start = 0;
        while (start < input.size() && (iswspace(input[start]) || input[start] == L'\0' || input[start] == 0xFEFF))
            ++start;

        size_t end = input.size();
        while (end > start && (iswspace(input[end - 1]) || input[end - 1] == L'\0'))
            --end;

        return input.substr(start, end - start);
    }

    bool TryInstallProcessManagerService()
    {
        wchar_t modulePath[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath))))
            return false;

        std::wstring serviceExePath(modulePath);
        size_t lastSlash = serviceExePath.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos)
            return false;

        serviceExePath = serviceExePath.substr(0, lastSlash + 1) + L"ProcessManagerService.exe";
        DWORD attrs = GetFileAttributesW(serviceExePath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            printf("[ProcessManager] Service installer executable not found: %S\n", serviceExePath.c_str());
            return false;
        }

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = serviceExePath.c_str();
        sei.lpParameters = L"--install";
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExW(&sei))
        {
            printf("[ProcessManager] Service install launch failed (err=%lu).\n", GetLastError());
            return false;
        }

        if (sei.hProcess)
        {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            if (exitCode != 0)
            {
                printf("[ProcessManager] Service install returned exit code %lu.\n", exitCode);
                return false;
            }
        }

        printf("[ProcessManager] Service install completed.\n");
        return true;
    }

    bool EnsureProcessManagerServiceRunning()
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
        {
            printf("[ProcessManager] OpenSCManager failed (err=%lu).\n", GetLastError());
            return false;
        }

        SC_HANDLE service = OpenServiceW(scm, L"AhlbornBridgeProcessManager", SERVICE_QUERY_STATUS | SERVICE_START);
        if (!service)
        {
            DWORD err = GetLastError();
            if (err == ERROR_SERVICE_DOES_NOT_EXIST)
            {
                printf("[ProcessManager] OpenService failed (err=%lu): service not installed. Attempting self-install...\n", err);
                if (TryInstallProcessManagerService())
                    service = OpenServiceW(scm, L"AhlbornBridgeProcessManager", SERVICE_QUERY_STATUS | SERVICE_START);
            }

            if (!service)
            {
                printf("[ProcessManager] OpenService failed (err=%lu).\n", GetLastError());
                CloseServiceHandle(scm);
                return false;
            }
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD bytesNeeded = 0;
        bool ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded) != FALSE;
        if (ok && status.dwCurrentState == SERVICE_RUNNING)
        {
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return true;
        }

        printf("[ProcessManager] Service not running, attempting auto-start...\n");

        if (!StartServiceW(service, 0, nullptr))
        {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING)
            {
                printf("[ProcessManager] StartService failed (err=%lu).\n", err);
                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return false;
            }
        }

        const DWORD startTick = GetTickCount();
        const DWORD timeoutMs = 8000;
        while (GetTickCount() - startTick < timeoutMs)
        {
            ZeroMemory(&status, sizeof(status));
            bytesNeeded = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded))
            {
                printf("[ProcessManager] QueryServiceStatusEx failed (err=%lu).\n", GetLastError());
                break;
            }

            if (status.dwCurrentState == SERVICE_RUNNING)
            {
                printf("[ProcessManager] Service auto-started successfully.\n");
                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return true;
            }

            if (status.dwCurrentState == SERVICE_STOPPED)
                break;

            Sleep(250);
        }

        printf("[ProcessManager] Service did not reach RUNNING state after auto-start attempt.\n");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }

    bool SendProcessManagerCommand(const std::wstring& command, std::wstring& response)
    {
        HANDLE hPipe = CreateFileW(
            L"\\\\.\\pipe\\AhlbornBridgeProcessManager",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            const DWORD firstPipeErr = GetLastError();
            printf("[ProcessManager] Pipe open failed (err=%lu), trying service auto-start...\n", firstPipeErr);

            if (!EnsureProcessManagerServiceRunning())
                return false;

            hPipe = CreateFileW(
                L"\\\\.\\pipe\\AhlbornBridgeProcessManager",
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                printf("[ProcessManager] Pipe open failed after auto-start (err=%lu).\n", GetLastError());
                return false;
            }
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

        DWORD bytesWritten = 0;
        const DWORD bytesToWrite = static_cast<DWORD>(command.size() * sizeof(wchar_t));
        BOOL writeOk = WriteFile(hPipe, command.data(), bytesToWrite, &bytesWritten, nullptr);
        if (!writeOk || bytesWritten != bytesToWrite)
        {
            CloseHandle(hPipe);
            return false;
        }

        wchar_t buffer[512] = {};
        DWORD bytesRead = 0;
        BOOL readOk = ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, nullptr);
        CloseHandle(hPipe);
        if (!readOk || bytesRead == 0)
            return false;

        buffer[bytesRead / sizeof(wchar_t)] = L'\0';
        response = TrimPipeResponse(buffer);
        return true;
    }

    bool IsPriorityResponse(const std::wstring& response, const wchar_t* priorityName)
    {
        std::wstring upper = response;
        for (wchar_t& ch : upper)
            ch = static_cast<wchar_t>(towupper(ch));

        std::wstring token = L"PRIORITY=";
        token += priorityName;
        return upper.find(token) != std::wstring::npos;
    }

    bool IsOrganCurrentlyLoadedInHauptwerk()
    {
        // Require both the canonical runtime flag and a confirmed loaded-title
        // shape to avoid transient false positives at startup.
        if (!isOrganLoaded)
            return false;

        HWND mainWindow = g_hauptwerkMainWindow.load();
        if (!mainWindow || !IsWindow(mainWindow))
            return false;

        wchar_t title[512] = {};
        if (GetWindowTextW(mainWindow, title, static_cast<int>(_countof(title))) <= 0)
            return false;

        return std::wcsncmp(title, L"Hauptwerk -", 11) == 0;
    }

    void RequestHauptwerkRealtimePriorityViaService()
    {
        bool expected = false;
        if (!g_hauptwerkRealtimeEnforcerRunning.compare_exchange_strong(expected, true))
            return;

        std::thread([]()
            {
                printf("[ProcessManager] Priority policy enforcer started (priority from settings).\n");
                bool realtimeAppliedForLoadedState = false;

                std::wstring idlePriority, loadedPriority;
                LoadHauptwerkPrioritySettings(idlePriority, loadedPriority);
                printf("[ProcessManager] Priority settings: idle=%S loaded=%S\n", idlePriority.c_str(), loadedPriority.c_str());

                bool lastOrganLoaded = false;
                while (IsProcessRunning(L"Hauptwerk.exe"))
                {
                    Sleep(2000);

                    // Reload settings each cycle so UI changes take effect without restart
                    LoadHauptwerkPrioritySettings(idlePriority, loadedPriority);

                    const bool organLoaded = IsOrganCurrentlyLoadedInHauptwerk();
                    std::wstring check;
                    if (!SendProcessManagerCommand(L"GET_PRIORITY Hauptwerk.exe", check))
                        continue;

                    if (organLoaded != lastOrganLoaded)
                    {
                        printf("[ProcessManager] organLoaded state changed -> %d\n", organLoaded ? 1 : 0);
                        lastOrganLoaded = organLoaded;
                    }

                    if (organLoaded)
                    {
                        if (!IsPriorityResponse(check, loadedPriority.c_str()))
                        {
                            std::wstring apply;
                            if (SendProcessManagerCommand(L"SET_PRIORITY Hauptwerk.exe " + loadedPriority, apply))
                            {
                                printf("[ProcessManager] Organ loaded: enforcing %S -> %S\n", loadedPriority.c_str(), apply.c_str());
                                realtimeAppliedForLoadedState = true;
                            }
                        }
                        else
                        {
                            realtimeAppliedForLoadedState = true;
                        }
                    }
                    else
                    {
                        if (!IsPriorityResponse(check, idlePriority.c_str()))
                        {
                            std::wstring restore;
                            if (SendProcessManagerCommand(L"SET_PRIORITY Hauptwerk.exe " + idlePriority, restore))
                                printf("[ProcessManager] Organ not loaded: setting %S -> %S\n", idlePriority.c_str(), restore.c_str());
                        }

                        realtimeAppliedForLoadedState = false;
                    }
                }

                g_hauptwerkRealtimeEnforcerRunning.store(false);
                printf("[ProcessManager] Priority policy enforcer stopped.\n");
            }).detach();
    }
}

void ApplyHauptwerkPriorityNow()
{
    if (!IsProcessRunning(L"Hauptwerk.exe"))
        return;

    std::wstring idlePriority, loadedPriority;
    LoadHauptwerkPrioritySettings(idlePriority, loadedPriority);

    const bool organLoaded = IsOrganCurrentlyLoadedInHauptwerk();
    const std::wstring& target = organLoaded ? loadedPriority : idlePriority;

    std::wstring response;
    if (SendProcessManagerCommand(L"SET_PRIORITY Hauptwerk.exe " + target, response))
        printf("[ProcessManager] ApplyHauptwerkPriorityNow: organLoaded=%d -> %S (%S)\n",
            (int)organLoaded, target.c_str(), response.c_str());
    else
        printf("[ProcessManager] ApplyHauptwerkPriorityNow: SET_PRIORITY failed\n");
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

std::wstring DetectBiduleExePath()
{
    std::wstring savedPath;
    if (LoadBidulePath(savedPath) && !savedPath.empty())
    {
        DWORD attrs = GetFileAttributesW(savedPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return savedPath;
    }

    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Plogue\\Bidule\\bidule.exe",
        L"C:\\Program Files\\Plogue\\Bidule\\Bidule.exe",
        L"C:\\Program Files (x86)\\Plogue\\Bidule\\bidule.exe",
        L"C:\\Program Files (x86)\\Plogue\\Bidule\\Bidule.exe"
    };

    for (const wchar_t* candidate : candidates)
    {
        DWORD attrs = GetFileAttributesW(candidate);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            SaveBidulePath(candidate);
            return candidate;
        }
    }

    return {};
}

bool LaunchBidule()
{
    if (IsProcessRunningByName(L"bidule.exe") || IsProcessRunningByName(L"Bidule.exe"))
    {
        printf("[Bidule] LaunchBidule: process already running; no new instance started.\n");
        return true;
    }

    std::wstring exePath = DetectBiduleExePath();
    if (exePath.empty())
    {
        printf("[Bidule] LaunchBidule: executable path not detected.\n");
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi = {};

    std::wstring cmd = L"\"" + exePath + L"\"";

    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(L'\0');

    std::wstring workingDir;
    size_t lastBackslash = exePath.find_last_of(L"\\/");
    if (lastBackslash != std::wstring::npos)
        workingDir = exePath.substr(0, lastBackslash);

    printf("[Bidule] LaunchBidule: starting exe=%S (dir=%S)\n", exePath.c_str(), workingDir.c_str());
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi))
    {
        printf("[Bidule] LaunchBidule: CreateProcess failed (error %lu).\n", GetLastError());
        return false;
    }

    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    // Retain process handle in a background watcher to diagnose early exit / monitor lifetime
    HANDLE hProcessDup = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &hProcessDup,
                        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
    {
        HANDLE hWatcher = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            HANDLE hProc = static_cast<HANDLE>(param);
            if (!hProc)
                return 0;

            // Monitor in steps up to 10 seconds: 1s, 2s, 3s, 5s, 10s
            const DWORD checkPoints[] = { 1000, 1000, 1000, 2000, 5000 };
            DWORD totalWaited = 0;
            for (DWORD stepWait : checkPoints)
            {
                DWORD waitRes = WaitForSingleObject(hProc, stepWait);
                totalWaited += stepWait;
                DWORD exitCode = STILL_ACTIVE;
                GetExitCodeProcess(hProc, &exitCode);
                if (waitRes == WAIT_OBJECT_0)
                {
                    printf("[Bidule] Diagnostics: process terminated at ~%lu ms. exitCode=%lu (0x%08lX)\n",
                           totalWaited, exitCode, exitCode);
                    CloseHandle(hProc);
                    return 0;
                }
                else
                {
                    printf("[Bidule] Diagnostics: process still active at ~%lu ms (exitCode=STILL_ACTIVE).\n", totalWaited);
                }
            }

            printf("[Bidule] Diagnostics: process remained active after %lu ms.\n", totalWaited);
            CloseHandle(hProc);
            return 0;
        }, hProcessDup, 0, nullptr);

        if (hWatcher)
            CloseHandle(hWatcher);
    }

    CloseHandle(pi.hProcess);
    printf("[Bidule] LaunchBidule: process started (PID=%lu).\n", pid);
    return true;
}

static bool SendBiduleOscSingleStringCommand(const char* address, const std::wstring& value, unsigned short port)
{
    if (!address || !*address || value.empty())
        return false;

    // Respect the Bidule OSC output configuration. When the caller used the
    // default port (3210) resolve the user-configured output port instead.
    LoadBiduleOscPorts();
    if (!IsBiduleOscOutEnabled())
    {
        printf("[BiduleOSC] Output disabled in settings; skipping %s.\n", address);
        return false;
    }
    if (port == 3210)
        port = static_cast<unsigned short>(GetBiduleOscOutPort());

    auto appendOscString = [](std::vector<char>& buffer, const std::string& text)
    {
        buffer.insert(buffer.end(), text.begin(), text.end());
        buffer.push_back('\0');
        while (buffer.size() % 4 != 0)
            buffer.push_back('\0');
    };

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 1)
        return false;

    std::string valueUtf8(static_cast<size_t>(utf8Len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, valueUtf8.data(), utf8Len, nullptr, nullptr);

    std::vector<char> packet;
    appendOscString(packet, address);
    appendOscString(packet, ",s");
    appendOscString(packet, valueUtf8);

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int sent = sendto(sock, packet.data(), static_cast<int>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    closesocket(sock);
    WSACleanup();

    return sent == static_cast<int>(packet.size());
}

bool SendBiduleOscOpenProfile(const std::wstring& profilePath, unsigned short port)
{
    bool ok = SendBiduleOscSingleStringCommand("/open", profilePath, port);
    if (ok)
        printf("[BiduleOSC] Sent /open %S to 127.0.0.1:%u\n", profilePath.c_str(), static_cast<unsigned>(port));
    return ok;
}

bool SendBiduleOscFileSaveAs(const std::wstring& profilePath, unsigned short port)
{
    bool ok = SendBiduleOscSingleStringCommand("/file_saveas", profilePath, port);
    if (!ok)
        ok = SendBiduleOscSingleStringCommand("/file_saveas", L"\"" + profilePath + L"\"", port);
    if (ok)
        printf("[BiduleOSC] Sent /file_saveas %S to 127.0.0.1:%u\n", profilePath.c_str(), static_cast<unsigned>(port));
    return ok;
}

bool SendBiduleOscPlay(bool play, unsigned short port)
{
    const std::wstring val = play ? L"true" : L"false";
    bool ok = SendBiduleOscSingleStringCommand("/play", val, port);
    if (ok)
        printf("[BiduleOSC] Sent /play %S to 127.0.0.1:%u\n", val.c_str(), static_cast<unsigned>(port));
    return ok;
}

bool SendBiduleOscStringCommand(const std::wstring& address, const std::wstring& value, unsigned short port)
{
    if (address.empty() || value.empty())
        return false;

    int utf8AddrLen = WideCharToMultiByte(CP_UTF8, 0, address.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8AddrLen <= 1)
        return false;

    std::string addressUtf8(static_cast<size_t>(utf8AddrLen - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, address.c_str(), -1, addressUtf8.data(), utf8AddrLen, nullptr, nullptr);

    bool ok = SendBiduleOscSingleStringCommand(addressUtf8.c_str(), value, port);
    if (ok)
        printf("[BiduleOSC] Sent %S %S to 127.0.0.1:%u\n", address.c_str(), value.c_str(), static_cast<unsigned>(port));
    return ok;
}

// Sends an OSC message with a single float32 argument (tag ",f"). Bidule knob/slider
// parameters expect a normalized float value (typically 0.0-1.0).
bool SendBiduleOscFloatCommand(const std::wstring& address, float value, unsigned short port)
{
    if (address.empty())
        return false;

    // Respect the Bidule OSC output configuration (enable flag + port).
    LoadBiduleOscPorts();
    if (!IsBiduleOscOutEnabled())
    {
        printf("[BiduleOSC] Output disabled in settings; skipping %S.\n", address.c_str());
        return false;
    }
    if (port == 3210)
        port = static_cast<unsigned short>(GetBiduleOscOutPort());

    int utf8AddrLen = WideCharToMultiByte(CP_UTF8, 0, address.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8AddrLen <= 1)
        return false;

    std::string addressUtf8(static_cast<size_t>(utf8AddrLen - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, address.c_str(), -1, addressUtf8.data(), utf8AddrLen, nullptr, nullptr);

    auto appendOscString = [](std::vector<char>& buffer, const std::string& text)
    {
        buffer.insert(buffer.end(), text.begin(), text.end());
        buffer.push_back('\0');
        while (buffer.size() % 4 != 0)
            buffer.push_back('\0');
    };

    std::vector<char> packet;
    appendOscString(packet, addressUtf8);
    appendOscString(packet, ",f");

    // OSC float32 argument is big-endian (network byte order).
    uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t be = htonl(bits);
    const char* p = reinterpret_cast<const char*>(&be);
    packet.insert(packet.end(), p, p + sizeof(be));

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int sent = sendto(sock, packet.data(), static_cast<int>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    closesocket(sock);
    WSACleanup();

    bool ok = (sent == static_cast<int>(packet.size()));
    if (ok)
        printf("[BiduleOSC] Sent %S %.5f to 127.0.0.1:%u\n", address.c_str(), value, static_cast<unsigned>(port));
    return ok;
}

void CloseBiduleProcess()
{
    printf("[Bidule] CloseBiduleProcess CALLED.\n");
    bool runningBefore = IsProcessRunningByName(L"bidule.exe") || IsProcessRunningByName(L"Bidule.exe");
    if (!runningBefore)
    {
        printf("[Bidule] CloseBiduleProcess: not running.\n");
        return;
    }

    DWORD bidulePid = FindProcessId(L"bidule.exe");
    if (bidulePid == 0)
        bidulePid = FindProcessId(L"Bidule.exe");

    HWND mainWindow = (bidulePid != 0) ? FindPrimaryBiduleWindow(bidulePid, false) : nullptr;
    if (mainWindow && IsWindow(mainWindow))
    {
        EnableWindowCloseButton(mainWindow);
        printf("[Bidule] CloseBiduleProcess: posting WM_CLOSE to HWND=%p PID=%lu\n", mainWindow, static_cast<unsigned long>(bidulePid));
        PostMessageW(mainWindow, WM_CLOSE, 0, 0);
        Sleep(250);
    }

    CloseProcessByName(L"bidule.exe");
    CloseProcessByName(L"Bidule.exe");

    const DWORD waitStart = GetTickCount();
    const DWORD waitTimeout = 5000;
    while (GetTickCount() - waitStart < waitTimeout)
    {
        if (!IsProcessRunningByName(L"bidule.exe") && !IsProcessRunningByName(L"Bidule.exe"))
        {
            printf("[Bidule] CloseBiduleProcess: running before=1 after=0\n");
            return;
        }
        Sleep(200);
    }

    printf("[Bidule] CloseBiduleProcess: process still running after graceful+forced close attempt.\n");
    CloseProcessByName(L"bidule.exe");
    CloseProcessByName(L"Bidule.exe");
    printf("[Bidule] CloseBiduleProcess: running before=1 after=%d\n",
        (IsProcessRunningByName(L"bidule.exe") || IsProcessRunningByName(L"Bidule.exe")) ? 1 : 0);
}

bool IsBiduleWindowVisible()
{
    DWORD bidulePid = FindProcessId(L"bidule.exe");
    if (bidulePid == 0)
        bidulePid = FindProcessId(L"Bidule.exe");
    if (bidulePid == 0)
        return false;

    HWND visibleWindow = FindPrimaryBiduleWindow(bidulePid, true);
    return visibleWindow != nullptr && IsWindowVisible(visibleWindow);
}

bool EnsureBiduleWindowReady(unsigned int timeoutMs)
{
    DWORD bidulePid = FindProcessId(L"bidule.exe");
    if (bidulePid == 0)
        bidulePid = FindProcessId(L"Bidule.exe");
    if (bidulePid == 0)
        return false;

    const DWORD startTick = GetTickCount();
    const DWORD timeout = timeoutMs < 1000 ? 1000 : timeoutMs;
    const LONG kMinReadyArea = 300000;
    unsigned int stableReadyHits = 0;

    while (GetTickCount() - startTick < timeout)
    {
        HWND mainWindow = FindPrimaryBiduleWindow(bidulePid, false);
        if (mainWindow && IsWindow(mainWindow))
        {
            RECT rc{};
            LONG area = 0;
            if (GetWindowRect(mainWindow, &rc))
            {
                LONG width = rc.right - rc.left;
                LONG height = rc.bottom - rc.top;
                if (width > 0 && height > 0)
                    area = width * height;
            }

            if (area >= kMinReadyArea)
            {
                ++stableReadyHits;
                if (stableReadyHits >= 3)
                    return true;
            }
            else
            {
                stableReadyHits = 0;
            }
        }
        else if (HasAnyBiduleTopLevelWindow(bidulePid))
        {
            stableReadyHits = 0;
            Sleep(150);
        }
        else
        {
            stableReadyHits = 0;
            Sleep(120);
        }

        Sleep(120);
    }

    return false;
}

bool ShowBiduleWindow()
{
    DWORD bidulePid = FindProcessId(L"bidule.exe");
    if (bidulePid == 0)
        bidulePid = FindProcessId(L"Bidule.exe");
    if (bidulePid == 0)
    {
        printf("[Bidule] ShowBiduleWindow: process not found.\n");
        return false;
    }

    HWND primaryWindow = FindPrimaryBiduleWindow(bidulePid, false);
    if (primaryWindow && IsWindow(primaryWindow))
    {
        ShowWindowAsync(primaryWindow, SW_SHOW);
        ShowWindowAsync(primaryWindow, SW_RESTORE);
        SetForegroundWindow(primaryWindow);
        DisableWindowCloseButton(primaryWindow);
        StartBiduleMinimizeWatcher(primaryWindow);
        printf("[Bidule] ShowBiduleWindow: restored primary window HWND=%p for PID=%lu (close disabled, minimize watcher active)\n",
               primaryWindow, static_cast<unsigned long>(bidulePid));
        return true;
    }

    // Fallback: if no single primary window was identified, restore the first valid top-level window
    HWND fallbackWindow = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* pTargetHwnd = reinterpret_cast<HWND*>(lParam);
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hwnd, &wndPid);
        DWORD targetPid = FindProcessId(L"bidule.exe");
        if (targetPid == 0) targetPid = FindProcessId(L"Bidule.exe");
        if (wndPid != 0 && wndPid == targetPid && IsWindow(hwnd))
        {
            if (GetWindow(hwnd, GW_OWNER) == nullptr && GetParent(hwnd) == nullptr)
            {
                *pTargetHwnd = hwnd;
                return FALSE; // found top-level candidate, stop enumeration
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fallbackWindow));

    if (fallbackWindow && IsWindow(fallbackWindow))
    {
        ShowWindowAsync(fallbackWindow, SW_SHOW);
        ShowWindowAsync(fallbackWindow, SW_RESTORE);
        SetForegroundWindow(fallbackWindow);
        DisableWindowCloseButton(fallbackWindow);
        StartBiduleMinimizeWatcher(fallbackWindow);
        printf("[Bidule] ShowBiduleWindow: fallback restored HWND=%p for PID=%lu (close disabled, minimize watcher active)\n",
               fallbackWindow, static_cast<unsigned long>(bidulePid));
        return true;
    }

    printf("[Bidule] ShowBiduleWindow: no suitable window found for PID=%lu\n",
           static_cast<unsigned long>(bidulePid));
    return false;
}

bool HideBiduleWindow()
{
    DWORD bidulePid = FindProcessId(L"bidule.exe");
    if (bidulePid == 0)
        bidulePid = FindProcessId(L"Bidule.exe");
    if (bidulePid == 0)
    {
        printf("[Bidule] HideBiduleWindow: process not found.\n");
        return false;
    }

    int hiddenCount = 0;
    // Iterate all top-level windows of Bidule process and hide any visible ones,
    // logging each window's HWND, class, title and visibility.
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* pCount = reinterpret_cast<int*>(lParam);
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hwnd, &wndPid);
        DWORD targetPid = FindProcessId(L"bidule.exe");
        if (targetPid == 0) targetPid = FindProcessId(L"Bidule.exe");
        if (wndPid != 0 && wndPid == targetPid && IsWindow(hwnd))
        {
            wchar_t title[256] = {};
            GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
            wchar_t cls[256] = {};
            GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
            BOOL visBefore = IsWindowVisible(hwnd);
            if (visBefore)
            {
                ShowWindow(hwnd, SW_HIDE);
                BOOL visAfter = IsWindowVisible(hwnd);
                printf("[Bidule] HideBiduleWindow: HWND=%p Class='%S' Title='%S' VisBefore=%d VisAfter=%d\n",
                       hwnd, cls, title, visBefore ? 1 : 0, visAfter ? 1 : 0);
                if (pCount) (*pCount)++;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hiddenCount));

    printf("[Bidule] HideBiduleWindow: finished hiding %d window(s) for PID=%lu.\n",
           hiddenCount, static_cast<unsigned long>(bidulePid));

    return true;
}

bool ToggleBiduleWindowVisibility()
{
    if (IsBiduleWindowVisible())
        return HideBiduleWindow();
    return ShowBiduleWindow();
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
					RequestHauptwerkRealtimePriorityViaService();
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
                errIcon += L"\\AhlbornBridge2\\Icons\\A_Error.png";
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
	RequestHauptwerkRealtimePriorityViaService();

	ReloadStandbyOrgans();

	// --- Reset Stream Deck buttons (startup: do not reset switch visuals, organ load will restore them)
	printf("\nFrom Hauptwerk.cpp line %d: Sending MIDI message to reset Stream Deck buttons...\n", __LINE__);
	SendUnloadOrganMidiMessage(false);

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

// ===========================================================================
// Incoming OSC listeners (DSP parameters on 5000, audio levels on 5001)
// ===========================================================================
//
// Design:
//   * Two UDP listeners, one per port, each in its own thread (blocking recvfrom).
//   * Incoming addresses look like "/DSP_FRONT/RvrbTime" where the first path
//     segment is the DSP target (DSP_FRONT / DSP_SIDE / DSP_REAR) and the second
//     is the parameter name. We split the two and look the parameter up in a
//     dispatch table, so a single table row serves all three DSPs.
//   * Two SEPARATE tables: g_dspCommands (port 5000) and g_levelCommands (5001),
//     keeping the high-rate meter traffic isolated from parameter commands.
//   * Audio level updates are throttled to one push to the Stream Deck every 50 ms
//     per DSP target, to avoid flooding the plugin pipe.
//
// To add a new command: append a row to the relevant table. Unknown parameters
// are logged and ignored, so the tables can grow incrementally.

namespace
{
    constexpr unsigned short kOscDspPort = 5000;
    constexpr unsigned short kOscLevelPort = 5001;
    constexpr int kLevelThrottleMs = 50;

    // Parsed OSC message: address plus (optionally) one float and/or one string arg.
    struct OscMessage
    {
        std::string address;     // e.g. "/DSP_FRONT/RvrbTime"
        bool        hasFloat = false;
        float       floatArg = 0.0f;
        bool        hasString = false;
        std::string stringArg;
    };

    // Handler signature: receives the DSP target ("DSP_FRONT") and the parsed message.
    using OscHandler = std::function<void(const std::string& target, const OscMessage& msg)>;

    struct OscCommand
    {
        std::string param;      // e.g. "RvrbTime", "IR_Autom", "Level"
        OscHandler  handler;
    };

    std::atomic<bool> g_oscListenersRunning{ false };
    std::thread       g_oscDspThread;
    std::thread       g_oscLevelThread;
    SOCKET            g_oscDspSocket = INVALID_SOCKET;
    SOCKET            g_oscLevelSocket = INVALID_SOCKET;

    // One-shot trigger: when armed, the first datagram received on the DSP port
    // (5000) sends a single "/play true" to Bidule, then disarms. See
    // ArmBidulePlayOnNextDspPacket() for rationale.
    std::atomic<bool> g_playOnNextDspPacket{ false };

    // Per-DSP throttle timestamps for the audio level meter (port 5001).
    std::mutex g_levelThrottleMutex;
    std::map<std::string, std::chrono::steady_clock::time_point> g_levelLastPush;

    // Reads a 4-byte-aligned OSC string starting at offset; advances offset past it.
    bool ReadOscString(const char* data, int len, int& offset, std::string& out)
    {
        if (offset >= len) return false;
        int start = offset;
        while (offset < len && data[offset] != '\0')
            ++offset;
        if (offset >= len) return false;          // no terminator
        out.assign(data + start, data + offset);
        ++offset;                                 // skip the '\0'
        offset = (offset + 3) & ~3;               // 4-byte align
        return true;
    }

    // Minimal OSC parser: address + typetag, supporting a single ',f' and/or ',s'.
    bool ParseOscPacket(const char* data, int len, OscMessage& msg)
    {
        int offset = 0;
        if (!ReadOscString(data, len, offset, msg.address))
            return false;
        if (msg.address.empty() || msg.address[0] != '/')
            return false;

        std::string typeTags;
        if (!ReadOscString(data, len, offset, typeTags))
            return true;                          // address only, no args
        if (typeTags.empty() || typeTags[0] != ',')
            return true;

        for (size_t i = 1; i < typeTags.size(); ++i)
        {
            char tag = typeTags[i];
            if (tag == 'f')
            {
                if (offset + 4 > len) return false;
                uint32_t be = 0;
                std::memcpy(&be, data + offset, 4);
                uint32_t hostBits = ntohl(be);
                float f = 0.0f;
                std::memcpy(&f, &hostBits, 4);
                msg.floatArg = f;
                msg.hasFloat = true;
                offset += 4;
            }
            else if (tag == 'i')
            {
                if (offset + 4 > len) return false;
                uint32_t be = 0;
                std::memcpy(&be, data + offset, 4);
                msg.floatArg = static_cast<float>(static_cast<int32_t>(ntohl(be)));
                msg.hasFloat = true;
                offset += 4;
            }
            else if (tag == 's')
            {
                std::string s;
                if (!ReadOscString(data, len, offset, s)) return false;
                msg.stringArg = s;
                msg.hasString = true;
            }
            else
            {
                // Unknown tag — stop parsing further args, keep what we have.
                break;
            }
        }
        return true;
    }

    // Splits "/DSP_FRONT/RvrbTime" into target="DSP_FRONT", param="RvrbTime".
    bool SplitOscAddress(const std::string& address, std::string& target, std::string& param)
    {
        if (address.size() < 2 || address[0] != '/')
            return false;
        size_t secondSlash = address.find('/', 1);
        if (secondSlash == std::string::npos)
            return false;
        target = address.substr(1, secondSlash - 1);
        param = address.substr(secondSlash + 1);
        // Drop any trailing path segments for now (we only use the first param token).
        size_t extra = param.find('/');
        if (extra != std::string::npos)
            param = param.substr(0, extra);
        return !target.empty() && !param.empty();
    }

    // ----- Handlers -------------------------------------------------------

    // /DSP_xxx/IR_Autom <value>  — selects a church from the IR table.
    // The float value is the IR coefficient (e.g. 0.013659579) that must match a
    // church's front/side/rear column; the DSP target selects which column to use.
    // Highlights the matching church button on the Stream Deck.
    void OnIrAutom(const std::string& target, const OscMessage& msg)
    {
        std::vector<ChurchInfo> churches = LoadChurches();
        if (churches.empty())
        {
            printf("[OSC] IR_Autom from %s but no churches loaded.\n", target.c_str());
            return;
        }

        std::string churchName;

        if (msg.hasString)
        {
            // Direct church name match (case-insensitive).
            for (const auto& c : churches)
            {
                if (_stricmp(c.name.c_str(), msg.stringArg.c_str()) == 0)
                {
                    churchName = c.name;
                    break;
                }
            }
        }
        else if (msg.hasFloat)
        {
            // Select which IR column to compare against based on the DSP target.
            // DSP_FRONT -> front, DSP_SIDE -> side, DSP_REAR -> rear.
            // The target is matched case-insensitively and defaults to front.
            auto columnFor = [&](const ChurchInfo& c) -> const std::string& {
                if (target.find("SIDE") != std::string::npos || target.find("side") != std::string::npos)
                    return c.side;
                if (target.find("REAR") != std::string::npos || target.find("rear") != std::string::npos)
                    return c.rear;
                return c.front;
            };

            // Find the church whose column value is closest to the received value
            // within a small tolerance (the table stores values as text).
            double value = static_cast<double>(msg.floatArg);
            double bestDiff = 1e-6;               // absolute tolerance
            for (const auto& c : churches)
            {
                const std::string& col = columnFor(c);
                if (col.empty())
                    continue;
                double colValue = 0.0;
                try { colValue = std::stod(col); }
                catch (...) { continue; }
                double diff = std::fabs(colValue - value);
                if (diff <= bestDiff)
                {
                    bestDiff = diff;
                    churchName = c.name;
                }
            }
        }

        if (churchName.empty())
        {
            printf("[OSC] IR_Autom from %s: could not resolve church (str='%s', num=%.9f).\n",
                target.c_str(), msg.hasString ? msg.stringArg.c_str() : "", msg.floatArg);
            return;
        }

        printf("[OSC] IR_Autom from %s -> highlight church '%s'.\n", target.c_str(), churchName.c_str());
        NotifyStreamDeckChurchHighlight(churchName);
    }

    // /DSP_xxx/RvrbTime <float>  — pushes the value to a Stream Deck LCD display.
    void OnRvrbTime(const std::string& target, const OscMessage& msg)
    {
        if (!msg.hasFloat)
        {
            printf("[OSC] RvrbTime from %s without float arg — ignored.\n", target.c_str());
            return;
        }
        NotifyStreamDeckDspValue(target, "RvrbTime", msg.floatArg);
    }

    // /DSP_xxx/Level <float>  — real-time audio meter (port 5001), throttled to 50 ms.
    void OnAudioLevel(const std::string& target, const OscMessage& msg)
    {
        if (!msg.hasFloat)
            return;

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(g_levelThrottleMutex);
            auto it = g_levelLastPush.find(target);
            if (it != g_levelLastPush.end())
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
                if (elapsed < kLevelThrottleMs)
                    return;                       // drop: too soon since last push
            }
            g_levelLastPush[target] = now;
        }

        NotifyStreamDeckDspValue(target, "Level", msg.floatArg);
    }

    // ----- Dispatch tables (extend by adding rows) ------------------------

    const std::vector<OscCommand> g_dspCommands = {
        { "IR_Autom", &OnIrAutom },
        { "RvrbTime", &OnRvrbTime },
        // Add more DSP parameter commands here.
    };

    const std::vector<OscCommand> g_levelCommands = {
        { "Level", &OnAudioLevel },
        // Bidule streams the real-time meter as /AUDIO_FRONT_L/Maximum_sample.
        { "Maximum_sample", &OnAudioLevel },
        // Add more audio-level commands here.
    };

    void DispatchOsc(const std::vector<OscCommand>& table, const OscMessage& msg)
    {
        std::string target, param;
        if (!SplitOscAddress(msg.address, target, param))
        {
            printf("[OSC] Malformed address '%s' — ignored.\n", msg.address.c_str());
            return;
        }
        for (const auto& cmd : table)
        {
            if (cmd.param == param)
            {
                cmd.handler(target, msg);
                return;
            }
        }
        // Unknown parameters are common (Altiverb streams the full DSP state);
        // silently ignore them so the log is not flooded.
    }

    // Processes a raw OSC packet which may be either a single message or an
    // OSC bundle (#bundle\0 + 8-byte timetag + repeated [int32 size][element]).
    // Bundles may nest; each contained message is parsed and dispatched.
    void ProcessOscData(const char* data, int len, const std::vector<OscCommand>& table)
    {
        if (len <= 0)
            return;

        // Bundle: starts with the literal "#bundle\0".
        if (len >= 16 && std::memcmp(data, "#bundle", 7) == 0 && data[7] == '\0')
        {
            int offset = 16;                      // skip "#bundle\0" (8) + timetag (8)
            while (offset + 4 <= len)
            {
                uint32_t be = 0;
                std::memcpy(&be, data + offset, 4);
                int elemSize = static_cast<int>(ntohl(be));
                offset += 4;
                if (elemSize <= 0 || offset + elemSize > len)
                    break;                        // malformed / truncated
                ProcessOscData(data + offset, elemSize, table);
                offset += elemSize;
            }
            return;
        }

        // Plain message.
        OscMessage msg;
        if (ParseOscPacket(data, len, msg))
            DispatchOsc(table, msg);
    }

    // Blocking UDP receive loop for one port and one dispatch table.
    void OscListenerLoop(SOCKET& sockOut, unsigned short port, const std::vector<OscCommand>& table, const char* label)
    {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET)
        {
            printf("[OSC][%s] socket() failed: %d\n", label, WSAGetLastError());
            return;
        }

        // Allow the port to be re-bound quickly after a restart, and let other
        // OSC monitors (e.g. Protokol) coexist on the same port if needed.
        BOOL reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        // Bind to all interfaces (INADDR_ANY) so datagrams are received
        // regardless of which local IP they target. Binding only to the
        // loopback address drops packets sent to the machine's LAN IP.
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            printf("[OSC][%s] bind() to port %u failed: %d\n", label, static_cast<unsigned>(port), WSAGetLastError());
            closesocket(sock);
            return;
        }

        sockOut = sock;
        printf("[OSC][%s] Listening on 0.0.0.0:%u (all interfaces)\n", label, static_cast<unsigned>(port));

        char buffer[2048];
        while (g_oscListenersRunning.load())
        {
            int received = recvfrom(sock, buffer, sizeof(buffer), 0, nullptr, nullptr);
            if (received == SOCKET_ERROR)
            {
                if (!g_oscListenersRunning.load())
                    break;                        // socket closed during shutdown
                int err = WSAGetLastError();
                if (err == WSAEINTR || err == WSAENOTSOCK)
                    break;
                continue;                         // transient error — keep listening
            }
            if (received <= 0)
                continue;

            // If a profile load armed the one-shot play trigger, the arrival of
            // the first DSP parameter datagram proves Bidule finished loading the
            // profile. That is the right moment to start the audio engine. Only
            // the DSP port (5000) acts as this readiness signal.
            if (port == kOscDspPort && g_playOnNextDspPacket.exchange(false))
            {
                SendBiduleOscPlay(true);
                printf("[Bidule] First DSP packet received -> activated audio engine with /play true.\n");
            }

            // Handles both plain OSC messages and OSC bundles (#bundle...).
            ProcessOscData(buffer, received, table);
        }

        printf("[OSC][%s] Listener stopped.\n", label);
    }
} // namespace

void ArmBidulePlayOnNextDspPacket()
{
    g_playOnNextDspPacket.store(true);
    printf("[Bidule] Armed /play true on next DSP packet (port 5000).\n");
}

void StartOscListeners()
{
    if (g_oscListenersRunning.exchange(true))
        return;                                   // already running

    // Pick up the latest configured ports/enable flags from Settings.xml.
    LoadBiduleOscPorts();

    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (IsBiduleDspPortEnabled())
    {
        unsigned short dspPort = static_cast<unsigned short>(GetBiduleDspPort());
        g_oscDspThread = std::thread([dspPort]() {
            OscListenerLoop(g_oscDspSocket, dspPort, g_dspCommands, "DSP");
        });
    }
    if (IsBiduleLevelPortEnabled())
    {
        unsigned short levelPort = static_cast<unsigned short>(GetBiduleLevelPort());
        g_oscLevelThread = std::thread([levelPort]() {
            OscListenerLoop(g_oscLevelSocket, levelPort, g_levelCommands, "Level");
        });
    }
}

void StopOscListeners()
{
    if (!g_oscListenersRunning.exchange(false))
        return;                                   // not running

    // Closing the sockets unblocks the recvfrom calls.
    if (g_oscDspSocket != INVALID_SOCKET)
    {
        closesocket(g_oscDspSocket);
        g_oscDspSocket = INVALID_SOCKET;
    }
    if (g_oscLevelSocket != INVALID_SOCKET)
    {
        closesocket(g_oscLevelSocket);
        g_oscLevelSocket = INVALID_SOCKET;
    }

    if (g_oscDspThread.joinable())
        g_oscDspThread.join();
    if (g_oscLevelThread.joinable())
        g_oscLevelThread.join();

    WSACleanup();
}
