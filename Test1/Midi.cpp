#include "Midi.h"
#include "StreamDeck.h"
#include "Xml.h"
#include "TrayIcon.h"
#include "Hauptwerk.h"
#include "Qt.h"
#include "Midi2Endpoint.h"
#include <cwchar>
#include <string>
#include <shlobj.h>
#include <knownfolders.h>

HMIDIIN hMidiIn = nullptr;
HMIDIIN hMidiIn2 = nullptr;
HMIDIOUT hMidiOut = nullptr;
HMIDIOUT hMidiOut2 = nullptr;
HANDLE hThread = nullptr;
CRITICAL_SECTION g_midiOutLock;

std::atomic<uint64_t> noteState[kChannels][2] = {};
std::atomic<uint64_t> outputNoteState[kChannels][2] = {};
std::atomic<TimePoint> lastActiveSensingTime{};
std::atomic<bool> running{ false };
TimePoint programStart{};
std::atomic<TimePoint> lastFeTime{};
std::atomic<DeviceState> deviceState{ DeviceState::Disconnected };
std::atomic<HWND> g_hauptwerkMainWindow{ nullptr };
std::atomic<bool> g_hauptwerkKeyHeld{ false };
std::atomic<bool> g_0xFF_firstTime{ false };
std::atomic<bool> g_ignoreKeyReleaseOnDisconnect{ false };
std::atomic<bool> g_midiRouterEnabled{ false };
bool isOrganLoaded = false;
std::atomic<bool> g_inputDeviceOpen{ false };
std::atomic<bool> g_input2DeviceOpen{ false };
std::atomic<bool> g_outputDeviceOpen{ false };
std::atomic<bool> g_output2DeviceOpen{ false };
std::atomic<bool> g_isLoadingOrgan{ false };
std::atomic<int> g_currentLoadedFavoriteIndex{ 0 };
std::atomic<int> g_currentLoadedInstalledOrganIndex{ 0 };
std::atomic<TrayIconImageStatus> g_trayIconImageStatus{ TrayIconImageStatus::Disabled };
std::wstring g_hauptwerkOrganTitle;

namespace
{
    std::atomic<bool> g_hauptwerkWorkerActive{ false };

    // ---------------------------------------------------------------
    // Lock-free SPSC ring buffer for MIDI output routing.
    // The MIDI input callback enqueues messages here; a dedicated
    // worker thread dequeues and forwards them via midiOutShortMsg,
    // keeping multimedia API calls out of the callback context.
    // ---------------------------------------------------------------
    constexpr unsigned int kMidiOutQueueCapacity = 1024; // must be power of 2
    static_assert((kMidiOutQueueCapacity & (kMidiOutQueueCapacity - 1)) == 0,
        "kMidiOutQueueCapacity must be a power of 2");

    struct MidiOutQueue
    {
        DWORD buffer[kMidiOutQueueCapacity]{};
        std::atomic<unsigned int> head{ 0 }; // written by producer (callback)
        std::atomic<unsigned int> tail{ 0 }; // written by consumer (worker)

        bool TryEnqueue(DWORD msg)
        {
            unsigned int h = head.load(std::memory_order_relaxed);
            unsigned int next = (h + 1) & (kMidiOutQueueCapacity - 1);
            if (next == tail.load(std::memory_order_acquire))
                return false; // full
            buffer[h] = msg;
            head.store(next, std::memory_order_release);
            return true;
        }

        bool TryDequeue(DWORD& msg)
        {
            unsigned int t = tail.load(std::memory_order_relaxed);
            if (t == head.load(std::memory_order_acquire))
                return false; // empty
            msg = buffer[t];
            tail.store((t + 1) & (kMidiOutQueueCapacity - 1), std::memory_order_release);
            return true;
        }
    };

    MidiOutQueue g_midiOutQueue;
    HANDLE g_midiOutWorkerThread = nullptr;

    DWORD WINAPI MidiOutWorkerThread(LPVOID)
    {
        while (running)
        {
            DWORD msg;
            bool dispatched = false;
            while (g_midiOutQueue.TryDequeue(msg))
            {
                EnterCriticalSection(&g_midiOutLock);
                HMIDIOUT out = hMidiOut;
                if (out != nullptr)
                {
                    midiOutShortMsg(out, msg);
                }
                HMIDIOUT out2 = hMidiOut2;
                if (out2 != nullptr)
                {
                    midiOutShortMsg(out2, msg);
                }
                LeaveCriticalSection(&g_midiOutLock);
                dispatched = true;
            }
            if (!dispatched)
            {
                Sleep(1);
            }
        }
        // Drain remaining messages on shutdown
        DWORD msg;
        while (g_midiOutQueue.TryDequeue(msg))
        {
            EnterCriticalSection(&g_midiOutLock);
            HMIDIOUT out = hMidiOut;
            if (out != nullptr)
            {
                midiOutShortMsg(out, msg);
            }
            HMIDIOUT out2 = hMidiOut2;
            if (out2 != nullptr)
            {
                midiOutShortMsg(out2, msg);
            }
            LeaveCriticalSection(&g_midiOutLock);
        }
        return 0;
    }

    void StartMidiOutWorker()
    {
        if (!g_midiOutWorkerThread)
        {
            g_midiOutWorkerThread = CreateThread(nullptr, 0, MidiOutWorkerThread, nullptr, 0, nullptr);
        }
    }

    // ---------------------------------------------------------------
    // Deferred UI command queue.
    // The MIDI callback must not call blocking UI / COM / accessibility
    // functions directly.  Instead it enqueues a lightweight command
    // here; a dedicated worker thread picks it up and executes it on a
    // normal thread where those calls are safe.
    // ---------------------------------------------------------------
    enum class DeferredCmd : DWORD
    {
        None = 0,
        LoadFavoriteOrgan0,
        LoadFavoriteOrgan1,
        LoadFavoriteOrgan2,
        LoadFavoriteOrgan3,
        LoadFavoriteOrgan4,
        LoadFavoriteOrgan5,
        LoadFavoriteOrgan6,
        LoadFavoriteOrgan7,
        LoadFavoriteOrgan8,
        LoadFavoriteOrgan9,
        LoadFavoriteOrgan10,
        LoadFavoriteOrgan11,
        LoadFavoriteOrgan12,
        LoadFavoriteOrgan13,
        LoadFavoriteOrgan14,
        LoadFavoriteOrgan15,
        LoadFavoriteOrgan16,
        LoadFavoriteOrgan17,
        LoadFavoriteOrgan18,
        LoadFavoriteOrgan19,
        LoadFavoriteOrgan20,
        LoadFavoriteOrgan21,
        LoadFavoriteOrgan22,
        LoadFavoriteOrgan23,
        LoadFavoriteOrgan24,
        LoadFavoriteOrgan25,
        LoadFavoriteOrgan26,
        LoadFavoriteOrgan27,
        LoadFavoriteOrgan28,
        LoadFavoriteOrgan29,
        LoadFavoriteOrgan30,
        LoadFavoriteOrgan31,
        LoadFavoriteOrgan32,
        LoadInstalledOrgan      // payload: g_pendingInstalledOrganIndex (1-based)
    };

    constexpr unsigned int kDeferredCmdQueueCapacity = 64; // power of 2
    static_assert((kDeferredCmdQueueCapacity & (kDeferredCmdQueueCapacity - 1)) == 0,
        "kDeferredCmdQueueCapacity must be a power of 2");

    struct DeferredCmdQueue
    {
        DWORD buffer[kDeferredCmdQueueCapacity]{};
        std::atomic<unsigned int> head{ 0 };
        std::atomic<unsigned int> tail{ 0 };

        bool TryEnqueue(DeferredCmd cmd)
        {
            unsigned int h = head.load(std::memory_order_relaxed);
            unsigned int next = (h + 1) & (kDeferredCmdQueueCapacity - 1);
            if (next == tail.load(std::memory_order_acquire))
                return false;
            buffer[h] = static_cast<DWORD>(cmd);
            head.store(next, std::memory_order_release);
            return true;
        }

        bool TryDequeue(DeferredCmd& cmd)
        {
            unsigned int t = tail.load(std::memory_order_relaxed);
            if (t == head.load(std::memory_order_acquire))
                return false;
            cmd = static_cast<DeferredCmd>(buffer[t]);
            tail.store((t + 1) & (kDeferredCmdQueueCapacity - 1), std::memory_order_release);
            return true;
        }
    };

    DeferredCmdQueue g_deferredCmdQueue;
    HANDLE g_deferredCmdWorkerThread = nullptr;
    std::atomic<int> g_pendingInstalledOrganIndex{ 0 };

    DWORD WINAPI DeferredCmdWorkerThread(LPVOID)
    {
        while (running)
        {
            DeferredCmd cmd;
            bool dispatched = false;
            while (g_deferredCmdQueue.TryDequeue(cmd))
            {
                dispatched = true;
                switch (cmd)
                {
                    
				case DeferredCmd::LoadFavoriteOrgan0:
					printf("[Deferred] Unload organ ...\n");
					if (!ClickMenu(g_hauptwerkMainWindow,  mUNLOAD_ORGAN ))
					{
						printf("Bridge is unchecked!!..\n");
					}
					break;
				case DeferredCmd::LoadInstalledOrgan:
					{
						int idx = g_pendingInstalledOrganIndex.load();
						printf("[Deferred] LoadInstalledOrgan %d â€” loading via GUI automation\n", idx);

						std::vector<std::wstring> names = LoadInstalledOrganNames();
						if (idx >= 1 && idx <= static_cast<int>(names.size()) && !names[idx - 1].empty())
						{
							const std::wstring& name = names[idx - 1];
							printf("[Deferred] Organ name: %S\n", name.c_str());
							if (!LoadOrganByName(g_hauptwerkMainWindow, name))
							{
								printf("Bridge is unchecked!!..\n");
							}
						}
						else
						{
							printf("[Deferred] Installed organ index %d not found (list size=%zu).\n",
								idx, names.size());
						}
						break;
					}
				default:
				{
					DWORD cmdVal = static_cast<DWORD>(cmd);
					DWORD base   = static_cast<DWORD>(DeferredCmd::LoadFavoriteOrgan1);
					DWORD top    = static_cast<DWORD>(DeferredCmd::LoadFavoriteOrgan32);
					if (cmdVal >= base && cmdVal <= top)
					{
						int organIdx = static_cast<int>(cmdVal - base) + 1;
						printf("[Deferred] Loading favorite organ %d...\n", organIdx);
						std::wstring label = std::to_wstring(organIdx) + L": ";
						if (!ClickMenuPath(g_hauptwerkMainWindow,
							{ L"Organ", L"Load favorite organ", label.c_str() }))
						{
							printf("Bridge is unchecked!!..\n");
						}
					}
					break;
				}
                }
            }
            if (!dispatched)
            {
                Sleep(10);
            }
        }
        return 0;
    }

    void StartDeferredCmdWorker()
    {
        if (!g_deferredCmdWorkerThread)
        {
            g_deferredCmdWorkerThread = CreateThread(nullptr, 0, DeferredCmdWorkerThread, nullptr, 0, nullptr);
        }
    }

    // ---------------------------------------------------------------

    // Icon paths are constructed at runtime from the Roaming AppData folder
    static std::wstring BuildIconsBasePath()
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path != nullptr)
        {
            std::wstring base(path);
            CoTaskMemFree(path);
            base += L"\\AhlbornBridge\\Icons";
            return base;
        }

        // Fallback to previous hardcoded location
        return std::wstring(L"C:\\Users\\Default\\AppData\\Roaming\\AhlbornBridge\\Icons");
    }

    static const wchar_t* IconPath_A(const wchar_t* filename)
    {
        static std::wstring cachedDisabled;
        if (cachedDisabled.empty())
        {
            std::wstring base = BuildIconsBasePath();
            // Preallocate common names into static storage so returned pointer remains valid
            // We'll store a concatenation prefix here; individual getters below will replace it.
            (void)base;
        }

        static std::wstring storage[10];
        static int idx = 0;
        int cur = idx % 10;
        storage[cur].clear();
        storage[cur] = BuildIconsBasePath();
        storage[cur] += L"\\";
        storage[cur] += filename;
        idx++;
        return storage[cur].c_str();
    }

    static const wchar_t* kIconDisabled = IconPath_A(L"A_Disabled.png");
    static const wchar_t* kIconDisabled1 = IconPath_A(L"A_Disabled1.png");
    static const wchar_t* kIconStandbye = IconPath_A(L"A_Standbye.png");
    static const wchar_t* kIconStandbye1 = IconPath_A(L"A_Standbye1.png");
    static const wchar_t* kIconOnline = IconPath_A(L"A_Online.png");
    static const wchar_t* kIconOnline1 = IconPath_A(L"A_Online1.png");
    static const wchar_t* kIconError = IconPath_A(L"A_Error.png");
    static const wchar_t* kIconOffline = IconPath_A(L"A_Offline.png");
    static const wchar_t* kIconFlash = IconPath_A(L"A_Flash.png");
    constexpr std::chrono::milliseconds kTrayFlashInterval(500);
    constexpr std::chrono::milliseconds kHauptwerkKeyTapThreshold(300);
	constexpr wchar_t kHauptwerkOrganDefinitions[] = L"E:\\Audio applications\\h\\Hauptwerk\\HauptwerkSampleSetsAndComponents\\OrganDefinitions";

    std::atomic<bool> g_trayFlashActive{ false };
    std::atomic<bool> g_trayFlashFromFe{ false };
    std::atomic<bool> g_pendingOrganLoad{ false };
    std::atomic<bool> g_organLoadCancelled{ false };
    HANDLE g_trayFlashThread = nullptr;
    HANDLE g_hauptwerkTitleMonitorThread = nullptr;
    HANDLE g_organLoadingWatchThread = nullptr;
    HANDLE g_streamDeckMonitorThread = nullptr;

    TimePoint g_hauptwerkKeyLastPressTime{};
    std::atomic<TimePoint> g_firstFeTime{}; // timestamp of first received FE in current session

    std::wstring ExtractOrganTitle(const wchar_t* windowTitle)
    {
        constexpr wchar_t kPrefix[] = L"Hauptwerk -";
        constexpr size_t kPrefixLen = (sizeof(kPrefix) / sizeof(wchar_t)) - 1;

        if (!windowTitle || std::wcsncmp(windowTitle, kPrefix, kPrefixLen) != 0)
        {
            return {};
        }

        const wchar_t* start = windowTitle + kPrefixLen;
        while (*start == L' ' || *start == L'\t')
        {
            ++start;
        }

        return std::wstring(start);
    }

    // Choose the appropriate standby icon based on the current device state.
    // If the console is connected use the alternative standby icon, otherwise
    // fall back to the default standby image.
    const wchar_t* GetStandbyIconForCurrentDeviceState()
    {
        return (deviceState.load() == DeviceState::Connected)
            ? kIconStandbye1
            : kIconStandbye;
    }

    // Choose the appropriate online icon based on the current device state.
    // If the console is connected use the alternative online icon, otherwise
    // fall back to the default online image.
    const wchar_t* GetOnlineIconForCurrentDeviceState()
    {
        return (deviceState.load() == DeviceState::Connected)
            ? kIconOnline1
            : kIconOnline;
    }

        void OnHauptwerkShortPress()
        {
			// Short press of the Hauptwerk "fissatore" key detected (press+release within threshold).
			printf("Hauptwerk [FISSATORE] short press detected.\n");

			// Toggle Hauptwerk main window visibility if it is available.
			HWND mainWindow = g_hauptwerkMainWindow;
			if (!mainWindow || !IsWindow(mainWindow))
			{
				return;
			}

			// If the window is visible, hide it; otherwise, show it.
			if (IsWindowVisible(mainWindow))
			{
				ShowWindow(mainWindow, SW_HIDE);
				UpdateTrayIconFromFile(kIconFlash);
				//g_trayIconImageStatus = TrayIconImageStatus::Standby;
			}
			else
			{
				ShowWindow(mainWindow, SW_SHOW);
				// Ripristina l'ultima immagine salvata per la tray icon.
				TrayIconImageStatus lastStatus = g_trayIconImageStatus.load();
				switch (lastStatus)
				{
				case TrayIconImageStatus::Disabled:
					UpdateTrayIconFromFile(kIconDisabled);
					break;
				case TrayIconImageStatus::Standby:
					UpdateTrayIconFromFile(GetStandbyIconForCurrentDeviceState());
					break;
				case TrayIconImageStatus::Online:
					UpdateTrayIconFromFile(GetOnlineIconForCurrentDeviceState());
					break;
				case TrayIconImageStatus::Error:
					UpdateTrayIconFromFile(kIconError);
					break;
				case TrayIconImageStatus::Offline:
					UpdateTrayIconFromFile(kIconOffline);
					break;
				case TrayIconImageStatus::Unknown:
				default:
					break;
				}
			}
		}

    DWORD WINAPI TrayFlashThread(LPVOID)
    {
        bool showOnline = false;
        while (running)
        {
            if (!g_trayFlashActive.load())
            {
                Sleep(100);
                continue;
            }

            if (g_hauptwerkMainWindow != nullptr)
            {
                g_trayFlashActive = false;
                break;
            }

            UpdateTrayIconTooltip(L"STARTING HAUPTWERK...");
            showOnline = !showOnline;
            UpdateTrayIconFromFile(showOnline ? (g_trayFlashFromFe.load() ? kIconStandbye1 : kIconStandbye) : kIconDisabled);
            g_trayIconImageStatus = showOnline ? TrayIconImageStatus::Standby : TrayIconImageStatus::Disabled;
            Sleep(static_cast<DWORD>(kTrayFlashInterval.count()));
        }

        if (g_hauptwerkMainWindow != nullptr)
        {
            // If a Hauptwerk window is now available, pick the correct
            // final icon based on the actual window title: if the title
            // indicates an organ is loaded ("Hauptwerk - ...") show the
            // Online icon, otherwise show Standby.
            wchar_t title[512] = {};
            if (IsWindow(g_hauptwerkMainWindow) &&
                GetWindowTextW(g_hauptwerkMainWindow, title, static_cast<int>(_countof(title))) > 0 &&
                std::wcsncmp(title, L"Hauptwerk -", 11) == 0)
            {
                UpdateTrayIconTooltip(title);
                UpdateTrayIconFromFile(GetOnlineIconForCurrentDeviceState());
                g_trayIconImageStatus = TrayIconImageStatus::Online;
                g_hauptwerkOrganTitle = ExtractOrganTitle(title);
                NotifyOrganInfoTitleChanged();
            }
            else
            {
                UpdateTrayIconTooltip(L"HAUPTWERK");
                UpdateTrayIconFromFile(GetStandbyIconForCurrentDeviceState());
                g_trayIconImageStatus = TrayIconImageStatus::Standby;
                g_hauptwerkOrganTitle.clear();
                NotifyOrganInfoTitleChanged();
            }
        }

        return 0;
    }

    DWORD WINAPI HauptwerkTitleMonitorThread(LPVOID)
    {
        enum class IconState
        {
            Unknown,
            Standby,
            Online
        };

        IconState lastState = IconState::Unknown;
        DeviceState lastDeviceState = deviceState.load();
        while (running)
        {
            // Track whether the physical console connection state has
            // changed so we can refresh the tray icon even if the
            // Hauptwerk window title (standby/online) has not.
            DeviceState currentDeviceState = deviceState.load();
            bool deviceStateChanged = (currentDeviceState != lastDeviceState);

            if (g_trayFlashActive.load())
            {
                Sleep(200);
                continue;
            }

            HWND mainWindow = g_hauptwerkMainWindow;
            if (!mainWindow)
            {
                Sleep(200);
                continue;
            }

            // If the Hauptwerk window handle is no longer valid, assume the
            // user has closed Hauptwerk manually and update tray/icon state.
            if (!IsWindow(mainWindow))
            {
                printf("Hauptwerk.exe process closed (detected by title monitor).\n");
				
                // Send an unload message to the Stream Deck plugin so it can reset
                // the buttons.
                printf("From Midi.cpp, line %d: Sending MIDI message to reset Stream Deck buttons...\n", __LINE__);
                SendUnloadOrganMidiMessage();
                
                g_hauptwerkMainWindow = nullptr;
                isOrganLoaded = false;
                g_hauptwerkOrganTitle.clear();
                NotifyOrganInfoTitleChanged();
                
                // Mirror the disconnected visual state when Hauptwerk is
                // closed manually: use the disabled icon and corresponding
                // tooltip so the tray clearly shows the console as OFF.
                UpdateTrayIconTooltip(L"Hauptwerk Closed!");
                UpdateTrayIconFromFile(deviceState.load() == DeviceState::Connected
                    ? kIconDisabled1
                    : kIconDisabled);
                g_trayIconImageStatus = TrayIconImageStatus::Disabled;
                // Force next valid window title check to refresh the tray
                // icon/tooltip by resetting the cached state.
                lastState = IconState::Unknown;
                lastDeviceState = currentDeviceState;

                if (deviceState.load() == DeviceState::Disconnected)
                {
                    CloseSettingsWindowIfAutoClose();
                }

                Sleep(200);
                continue;
            }

            wchar_t title[512] = {};
            if (GetWindowTextW(mainWindow, title, static_cast<int>(_countof(title))) == 0)
            {
                Sleep(200);
                continue;
            }

            if (std::wcscmp(title, L"Hauptwerk") == 0)
            {
                if (lastState != IconState::Standby || deviceStateChanged)
                {
                    UpdateTrayIconTooltip(title);
                    UpdateTrayIconFromFile(GetStandbyIconForCurrentDeviceState());
                    lastState = IconState::Standby;
                    isOrganLoaded = false;
                    g_currentLoadedFavoriteIndex = 0;
                    g_currentLoadedInstalledOrganIndex = 0;
                    g_hauptwerkOrganTitle.clear();
                    NotifyOrganInfoTitleChanged();
                    g_trayIconImageStatus = TrayIconImageStatus::Standby;
                    if (!g_isLoadingOrgan.load() && ! g_pendingOrganLoad.load())
                    {
                        SendUnloadOrganMidiMessage();
                    }
                    
                }
            }
            else if (std::wcsncmp(title, L"Hauptwerk -", 11) == 0)
            {
                if (lastState != IconState::Online || deviceStateChanged)
                {
                    UpdateTrayIconTooltip(title);
                    UpdateTrayIconFromFile(GetOnlineIconForCurrentDeviceState());
					lastState = IconState::Online;
					isOrganLoaded = true;
					g_pendingOrganLoad = false;
					g_hauptwerkOrganTitle = ExtractOrganTitle(title);
                    NotifyOrganInfoTitleChanged();
                    g_trayIconImageStatus = TrayIconImageStatus::Online;
                }
            }

            lastDeviceState = currentDeviceState;
            Sleep(200);
        }

        return 0;
    }

    // Recursively collect accessible name/value strings from a window tree.
    void CollectAccessibleText(IAccessible* pAcc, std::vector<std::wstring>& texts, int depth = 0)
    {
        if (!pAcc || depth > 5) return;

        VARIANT varSelf;
        varSelf.vt = VT_I4;
        varSelf.lVal = CHILDID_SELF;

        BSTR bstrName = nullptr;
        if (SUCCEEDED(pAcc->get_accName(varSelf, &bstrName)) && bstrName)
        {
            std::wstring name(bstrName);
            SysFreeString(bstrName);
            if (!name.empty())
                texts.push_back(name);
        }

        BSTR bstrValue = nullptr;
        if (SUCCEEDED(pAcc->get_accValue(varSelf, &bstrValue)) && bstrValue)
        {
            std::wstring val(bstrValue);
            SysFreeString(bstrValue);
            if (!val.empty())
                texts.push_back(val);
        }

        long childCount = 0;
        pAcc->get_accChildCount(&childCount);
        if (childCount <= 0 || childCount > 200) return;

        std::vector<VARIANT> children(childCount);
        for (auto& v : children) VariantInit(&v);

        long obtained = 0;
        if (FAILED(AccessibleChildren(pAcc, 0, childCount, children.data(), &obtained)))
            return;

        for (long i = 0; i < obtained; ++i)
        {
            if (children[i].vt == VT_DISPATCH && children[i].pdispVal)
            {
                IAccessible* pChild = nullptr;
                if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible, (void**)&pChild)))
                {
                    CollectAccessibleText(pChild, texts, depth + 1);
                    pChild->Release();
                }
                children[i].pdispVal->Release();
            }
            else if (children[i].vt == VT_I4)
            {
                VARIANT varChild;
                varChild.vt = VT_I4;
                varChild.lVal = children[i].lVal;
                BSTR bstrChildName = nullptr;
                if (SUCCEEDED(pAcc->get_accName(varChild, &bstrChildName)) && bstrChildName)
                {
                    std::wstring cname(bstrChildName);
                    SysFreeString(bstrChildName);
                    if (!cname.empty())
                        texts.push_back(cname);
                }
            }
            else
            {
                VariantClear(&children[i]);
            }
        }
    }

    // Try to extract the organ title from the "Loading organ" dialog using
    // both EnumChildWindows (standard Win32 child controls) and IAccessible
    // (Qt / MSAA accessible objects).  Returns the best candidate string
    // that looks like an organ name, or empty if nothing useful was found.
    std::wstring TryExtractOrganTitleFromLoadingWindow(HWND hLoadingWnd)
    {
        if (!hLoadingWnd || !IsWindow(hLoadingWnd))
            return {};

        std::vector<std::wstring> texts;

        // 1) Enumerate standard Win32 child windows.
        EnumChildWindows(hLoadingWnd, [](HWND hChild, LPARAM lp) -> BOOL {
            auto* v = reinterpret_cast<std::vector<std::wstring>*>(lp);
            wchar_t buf[512] = {};
            if (GetWindowTextW(hChild, buf, static_cast<int>(_countof(buf))) > 0)
                v->emplace_back(buf);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&texts));

        // 2) Use IAccessible to walk the accessible tree.
        IAccessible* pAcc = nullptr;
        if (SUCCEEDED(AccessibleObjectFromWindow(hLoadingWnd, OBJID_CLIENT,
                IID_IAccessible, (void**)&pAcc)) && pAcc)
        {
            CollectAccessibleText(pAcc, texts);
            pAcc->Release();
        }

        // Log everything found for diagnostics.
        for (const auto& t : texts)
            printf("[OrganLoadingWatch] text: %S\n", t.c_str());

        // Heuristic: pick the longest string that isn't a generic label,
        // button caption, or short progress indicator.
        std::wstring best;
        for (const auto& t : texts)
        {
            if (_wcsicmp(t.c_str(), L"Loading organ") == 0) continue;
            if (_wcsicmp(t.c_str(), L"Cancel") == 0) continue;
            if (_wcsicmp(t.c_str(), L"OK") == 0) continue;
            if (_wcsicmp(t.c_str(), L"Close") == 0) continue;
            // Skip short percentage strings like "42%"
            if (t.size() < 6 && t.find(L'%') != std::wstring::npos) continue;
            if (t.size() > best.size())
                best = t;
        }

        // Clean up the extracted title.
        // The IAccessible text typically looks like:
        //   "Loading: StAnnesMoseley\nPlease wait."
        // Strip the "Loading: " prefix and "\nPlease wait." suffix
        // so that only the organ name remains.
        constexpr wchar_t kLoadingPrefix[] = L"Loading: ";
        constexpr size_t kLoadingPrefixLen = (sizeof(kLoadingPrefix) / sizeof(wchar_t)) - 1;
        if (best.size() > kLoadingPrefixLen &&
            _wcsnicmp(best.c_str(), kLoadingPrefix, kLoadingPrefixLen) == 0)
        {
            best = best.substr(kLoadingPrefixLen);
        }
        // Remove everything from the first newline onwards ("\nPlease wait." etc.)
        size_t nl = best.find(L'\n');
        if (nl != std::wstring::npos)
        {
            best = best.substr(0, nl);
        }
        // Trim trailing whitespace
        while (!best.empty() && (best.back() == L' ' || best.back() == L'\t' || best.back() == L'\r'))
        {
            best.pop_back();
        }

        return best;
    }

    DWORD WINAPI OrganLoadingWatchThread(LPVOID)
    {
        bool wasLoading = false;
        HWND hLastLoadingWnd = nullptr;
        bool titleExtracted = false;
        bool cancelDetected = false;
        while (running)
        {
            HWND hWnd = FindWindowW(nullptr, L"Loading organ");
            bool loading = (hWnd != nullptr && IsWindow(hWnd));
            if (loading)
            {
                hLastLoadingWnd = hWnd;

                // Detect cancellation by monitoring window visibility.
                // When the user (or our code) presses Cancel, Hauptwerk
                // hides the dialog before destroying it.  A window that
                // still exists but is no longer visible means the loading
                // was cancelled.
                if (!cancelDetected && !IsWindowVisible(hWnd))
                {
                    cancelDetected = true;
                    printf("[OrganLoadingWatch] Window hidden (HWND=%p) - waiting 1s to verify...\n", (void*)hWnd);
                    Sleep(1000);

                    bool organLoaded = isOrganLoaded;
                    if (!organLoaded)
                    {
                        g_organLoadCancelled = true;
                        SendUnloadOrganMidiMessage();
                        printf("[OrganLoadingWatch] CANCEL confirmed (isOrganLoaded=%d)\n", (int)organLoaded);
                    }
                    else
                    {
                        g_organLoadCancelled = false;
                        printf("[OrganLoadingWatch] Load OK (isOrganLoaded=%d)\n", (int)organLoaded);
                    }
                }

                // While loading, try to extract the organ title early.
                if (!titleExtracted)
                {
                    std::wstring title = TryExtractOrganTitleFromLoadingWindow(hWnd);
                    if (!title.empty())
                    {
                        printf("[OrganLoadingWatch] Extracted organ title: %S\n", title.c_str());
                        g_hauptwerkOrganTitle = title;
                        NotifyOrganInfoTitleChanged();
                        titleExtracted = true;

                        // Compare with standby organs stored in Settings.xml
                        // using exact match on the cleaned organ title.
                        std::vector<std::wstring> standbyNames = LoadStandbyOrganNames();
                        for (int idx = 0; idx < 32; ++idx)
                        {
                            if (!standbyNames[idx].empty() && title == standbyNames[idx])
                            {
                                printf("[OrganLoadingWatch] MATCH Standby_Organ0%d: %S\n",
                                    idx + 1, standbyNames[idx].c_str());

                                g_currentLoadedFavoriteIndex = idx + 1;
                                printf("[OrganLoadingWatch] g_currentLoadedFavoriteIndex = %d\n", idx + 1);

                                // Send MIDI out: BF 50 <1-based index>
                                DWORD midiMsg = CC_ch16 | (BF_0x50 << 8) | ((idx + 1) << 16);
                                EnqueueMidiOutMessage(midiMsg);
                                printf("[OrganLoadingWatch] Sent MIDI BF 50 %02X\n", idx + 1);
                                break;
                            }
                        }

                        // Also compare with the installed organs list so that BF 51
                        // buttons are lit correctly when the Stream Deck starts late.
                        std::vector<std::wstring> installedNames = LoadInstalledOrganNames();
                        for (int iidx = 0; iidx < static_cast<int>(installedNames.size()); ++iidx)
                        {
                            if (!installedNames[iidx].empty() && title == installedNames[iidx])
                            {
                                printf("[OrganLoadingWatch] MATCH InstalledOrgan %d: %S\n",
                                    iidx + 1, installedNames[iidx].c_str());

                                g_currentLoadedInstalledOrganIndex = iidx + 1;
                                printf("[OrganLoadingWatch] g_currentLoadedInstalledOrganIndex = %d\n", iidx + 1);

                                // Send MIDI out: BF 51 <1-based index>
                                DWORD midiMsg = CC_ch16 | (BF_0x51 << 8) | ((iidx + 1) << 16);
                                EnqueueMidiOutMessage(midiMsg);
                                printf("[OrganLoadingWatch] Sent MIDI BF 51 %02X\n", iidx + 1);

                                // Notify Stream Deck plugin of loaded organ
                                NotifyStreamDeckOrganState(iidx + 1);
                                break;
                            }
                        }
                    }
                }
            }
            if (loading != wasLoading)
            {
                if (!loading)
                    {
                        printf("[OrganLoadingWatch] Window destroyed (HWND=%p, cancelDetected=%d)\n",
                            (void*)hLastLoadingWnd, (int)cancelDetected);

                        if (!cancelDetected)
                        {
                            // Window was destroyed without becoming invisible first.
                            // Wait and verify as fallback.
                            printf("[OrganLoadingWatch] Fallback: waiting 1s to verify outcome...\n");
                            Sleep(1000);

                            bool organLoaded = isOrganLoaded;
                            g_organLoadCancelled = !organLoaded;

                            printf("[OrganLoadingWatch] Loading organ %s (isOrganLoaded=%d)\n",
                                organLoaded ? "COMPLETED" : "CANCELLED", (int)organLoaded);

                            if (!organLoaded)
                            {
                                SendUnloadOrganMidiMessage();
                                printf("[OrganLoadingWatch] Cancel notification sent\n");
                            }
                        }

                        hLastLoadingWnd = nullptr;
                        titleExtracted = false;
                        cancelDetected = false;
                    }
                else
                {
                    printf("[OrganLoadingWatch] Loading organ started (HWND=%p)\n", (void*)hWnd);
                    g_organLoadCancelled = false;
                    cancelDetected = false;
                }
                wasLoading = loading;
            }
            g_isLoadingOrgan = loading;
            Sleep(200);
        }
        g_isLoadingOrgan = false;
        return 0;
    }

    void StartTrayIconFlashing()
    {
        g_trayFlashActive = true;
        if (g_trayFlashThread)
        {
            DWORD exitCode = 0;
            if (GetExitCodeThread(g_trayFlashThread, &exitCode) && exitCode != STILL_ACTIVE)
            {
                CloseHandle(g_trayFlashThread);
                g_trayFlashThread = nullptr;
            }
        }

        if (!g_trayFlashThread)
        {
            g_trayFlashThread = CreateThread(nullptr, 0, TrayFlashThread, nullptr, 0, nullptr);
        }
    }

    void StopTrayIconFlashingInternal()
    {
        g_trayFlashActive = false;
    }

    // Logs the names of the currently open MIDI output ports.
    void LogOpenOutputPorts()
    {
        EnterCriticalSection(&g_midiOutLock);
        HMIDIOUT out1 = hMidiOut;
        HMIDIOUT out2 = hMidiOut2;
        LeaveCriticalSection(&g_midiOutLock);

        if (out1 != nullptr)
        {
            UINT id1 = 0;
            MIDIOUTCAPSW caps1{};
            if (midiOutGetID(out1, &id1) == MMSYSERR_NOERROR &&
                midiOutGetDevCapsW(id1, &caps1, sizeof(caps1)) == MMSYSERR_NOERROR)
                printf("[StreamDeck] Output 01 open: [%u] %S\n", id1, caps1.szPname);
        }
        else
        {
            printf("[StreamDeck] Output 01: NOT open\n");
        }

        if (out2 != nullptr)
        {
            UINT id2 = 0;
            MIDIOUTCAPSW caps2{};
            if (midiOutGetID(out2, &id2) == MMSYSERR_NOERROR &&
                midiOutGetDevCapsW(id2, &caps2, sizeof(caps2)) == MMSYSERR_NOERROR)
                printf("[StreamDeck] Output 02 open: [%u] %S\n", id2, caps2.szPname);
        }
        else
        {
            printf("[StreamDeck] Output 02: NOT open\n");
        }
    }

    // Sends the current organ state (favorite and/or installed) to the Stream Deck,
    // retrying at 500 ms, 2000 ms and 4000 ms after plugin detection
    // to cover slow plugin initialisation times.
    void SyncStreamDeckOrganState()
    {
        int favIdx  = g_currentLoadedFavoriteIndex.load();
        int instIdx = g_currentLoadedInstalledOrganIndex.load();
        if (favIdx <= 0 && instIdx <= 0)
        {
            printf("[StreamDeck] No organ loaded, skipping state sync.\n");
            return;
        }

        constexpr DWORD kRetryDelays[] = { 500, 1500, 2000 }; // cumulative gaps: 500, 2000, 4000 ms
        for (DWORD delay : kRetryDelays)
        {
            if (!running) break;
            Sleep(delay);

            favIdx  = g_currentLoadedFavoriteIndex.load();
            instIdx = g_currentLoadedInstalledOrganIndex.load();
            if (favIdx <= 0 && instIdx <= 0)
            {
                printf("[StreamDeck] Organ unloaded during retry, aborting sync.\n");
                break;
            }

            LogOpenOutputPorts();

            if (favIdx > 0)
            {
                DWORD midiMsg = CC_ch16 | (BF_0x50 << 8) | (static_cast<DWORD>(favIdx) << 16);
                bool enqueued = EnqueueMidiOutMessage(midiMsg);
                printf("[StreamDeck] Send BF 50 %02X (favorite %d): %s\n",
                    favIdx, favIdx, enqueued ? "enqueued OK" : "FAILED (queue full?)");
            }

            if (instIdx > 0)
            {
                DWORD midiMsg = CC_ch16 | (BF_0x51 << 8) | (static_cast<DWORD>(instIdx) << 16);
                bool enqueued = EnqueueMidiOutMessage(midiMsg);
                printf("[StreamDeck] Send BF 51 %02X (installed %d): %s\n",
                    instIdx, instIdx, enqueued ? "enqueued OK" : "FAILED (queue full?)");
            }
        }
    }

    DWORD WINAPI StreamDeckMonitorThread(LPVOID)
    {
        constexpr wchar_t kPluginProcess[] = L"se.trevligaspel.midiplugin.exe";
        bool wasRunning = false;
        while (running)
        {
            bool isRunning = IsProcessRunningByName(kPluginProcess);
            if (isRunning && !wasRunning)
            {
                printf("[StreamDeck] Plugin process started: %S\n", kPluginProcess);
                SyncStreamDeckOrganState();
            }
            else if (!isRunning && wasRunning)
            {
                printf("[StreamDeck] Plugin process stopped: %S\n", kPluginProcess);
            }
            wasRunning = isRunning;
            Sleep(500);
        }
        return 0;
    }

    void StartStreamDeckMonitor()
    {
        if (g_streamDeckMonitorThread)
        {
            DWORD exitCode = 0;
            if (GetExitCodeThread(g_streamDeckMonitorThread, &exitCode) && exitCode != STILL_ACTIVE)
            {
                CloseHandle(g_streamDeckMonitorThread);
                g_streamDeckMonitorThread = nullptr;
            }
        }

        if (!g_streamDeckMonitorThread)
        {
            g_streamDeckMonitorThread = CreateThread(nullptr, 0, StreamDeckMonitorThread, nullptr, 0, nullptr);
        }
    }

    void StartHauptwerkTitleMonitor()
    {
        if (g_hauptwerkTitleMonitorThread)
        {
            DWORD exitCode = 0;
            if (GetExitCodeThread(g_hauptwerkTitleMonitorThread, &exitCode) && exitCode != STILL_ACTIVE)
            {
                CloseHandle(g_hauptwerkTitleMonitorThread);
                g_hauptwerkTitleMonitorThread = nullptr;
            }
        }

        if (!g_hauptwerkTitleMonitorThread)
        {
            g_hauptwerkTitleMonitorThread = CreateThread(nullptr, 0, HauptwerkTitleMonitorThread, nullptr, 0, nullptr);
        }

        if (g_organLoadingWatchThread)
        {
            DWORD exitCode = 0;
            if (GetExitCodeThread(g_organLoadingWatchThread, &exitCode) && exitCode != STILL_ACTIVE)
            {
                CloseHandle(g_organLoadingWatchThread);
                g_organLoadingWatchThread = nullptr;
            }
        }

        if (!g_organLoadingWatchThread)
        {
            g_organLoadingWatchThread = CreateThread(nullptr, 0, OrganLoadingWatchThread, nullptr, 0, nullptr);
        }
    }

    bool OpenMidiDevice(UINT devIndex)
    {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(devIndex, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        {
            printf("[OpenMidiDevice] : Failed to query device 01 capabilities for device %u.\n", devIndex);
            g_inputDeviceOpen = false;
            return false;
        }

        printf("[OpenMidiDevice] : Opening Midi Input device 01: [%S]\n", caps.szPname);

        if (midiInOpen(&hMidiIn,
            devIndex,
            (DWORD_PTR)MidiInProc,
            0,
            CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        {
            printf("[OpenMidiDevice] : Failed to open MIDI device 01.\n");
            g_inputDeviceOpen = false;
            return false;
        }

        midiInStart(hMidiIn);
        g_inputDeviceOpen = true;
        return true;
    }

    bool OpenMidiDevice2(UINT devIndex)
    {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(devIndex, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        {
            printf("[OpenMidiDevice2] : Failed to query device 02 capabilities for device %u.\n", devIndex);
            g_input2DeviceOpen = false;
            return false;
        }

        printf("[OpenMidiDevice2] : Opening Midi Input device 02: [%S]\n", caps.szPname);

        if (midiInOpen(&hMidiIn2,
            devIndex,
            (DWORD_PTR)MidiInProc,
            0,
            CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        {
            printf("[OpenMidiDevice2] : Failed to open MIDI device 02.\n");
            g_input2DeviceOpen = false;
            return false;
        }

        midiInStart(hMidiIn2);
        g_input2DeviceOpen = true;
		printf("\n");
        return true;
    }

bool OpenMidiOutputDevice(UINT devIndex)
{
	MIDIOUTCAPS caps;
	if (midiOutGetDevCaps(devIndex, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
	{
		printf("[OpenMidiOutputDevice] : Failed to query output device capabilities for device %u.\n", devIndex);
		g_outputDeviceOpen = false;
		return false;
	}

	printf("[OpenMidiOutputDevice] : Opening Midi output device 01: [%S]\n", caps.szPname);

	if (midiOutOpen(&hMidiOut, devIndex, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
	{
		printf("[OpenMidiOutputDevice] : Failed to open MIDI output device 01.\n");
		g_outputDeviceOpen = false;
		return false;
	}

	printf("\n");
	g_outputDeviceOpen = true;
	return true;
}

bool OpenMidiOutput2Device(UINT devIndex)
{
	MIDIOUTCAPS caps;
	if (midiOutGetDevCaps(devIndex, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
	{
		printf("Failed to query output device 2 capabilities for device %u.\n", devIndex);
		g_output2DeviceOpen = false;
		return false;
	}

	printf("Opening Midi output device 02: [%S]\n", caps.szPname);

	if (midiOutOpen(&hMidiOut2, devIndex, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
	{
		printf("Failed to open MIDI output device 02.\n");
		g_output2DeviceOpen = false;
		return false;
	}

	printf("\n");
	g_output2DeviceOpen = true;
	return true;
}

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

        return GetWindow(hWnd, GW_CHILD) != nullptr;
    }

  
}

// Public helper so other modules (e.g., tray icon handler) can trigger the
// same tray flashing behaviour used when Hauptwerk is launched due to FE.
void TriggerTrayIconStartupFlash()
{
    g_trayFlashFromFe = false;
    StartTrayIconFlashing();
}

// Public helper to stop the tray icon flashing animation from other modules.
void StopTrayIconFlashing()
{
    g_trayFlashActive = false;
}

bool EnqueueMidiOutMessage(DWORD msg)
{
    return g_midiOutQueue.TryEnqueue(msg);
}

void EnqueueLoadInstalledOrgan(int index)
{
    if (index < 1 || index > 127) return;
    if (g_isLoadingOrgan)
    {
        HWND hLoadingWnd = FindWindowW(nullptr, L"Loading organ");
        if (hLoadingWnd)
        {
            PostMessageW(hLoadingWnd, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(hLoadingWnd, WM_KEYUP, VK_RETURN, 0);
        }
    }
    g_pendingOrganLoad = true;
    g_pendingInstalledOrganIndex.store(index);
    g_deferredCmdQueue.TryEnqueue(DeferredCmd::LoadInstalledOrgan);
}

void EnqueueUnloadOrgan()
{
    g_deferredCmdQueue.TryEnqueue(DeferredCmd::LoadFavoriteOrgan0);
}

void clearAllNotes()
{
    for (int ch = 0; ch < kChannels; ++ch)
        clearChannel(ch);
}

bool anyNoteActive()
{
    for (int ch = 0; ch < kChannels; ++ch)
    {
        if (noteState[ch][0].load(std::memory_order_relaxed) != 0 ||
            noteState[ch][1].load(std::memory_order_relaxed) != 0)
            return true;
    }
    return false;
}

bool IsChannelActive(int ch)
{
    if (ch < 0 || ch >= kChannels)
    {
        return false;
    }

    return noteState[ch][0].load(std::memory_order_relaxed) != 0
        || noteState[ch][1].load(std::memory_order_relaxed) != 0;
}

bool IsOutputChannelActive(int ch)
{
    if (!g_midiRouterEnabled.load())
    {
        return false;
    }

    if (ch < 0 || ch >= kChannels)
    {
        return false;
    }

    return outputNoteState[ch][0].load(std::memory_order_relaxed) != 0
        || outputNoteState[ch][1].load(std::memory_order_relaxed) != 0;
}

void ClearOutputNotes()
{
    for (int ch = 0; ch < kChannels; ++ch)
    {
        outputNoteState[ch][0] = 0;
        outputNoteState[ch][1] = 0;
    }
}

FeStatus GetFeStatus()
{
    auto lastFe = lastFeTime.load();
    if (lastFe.time_since_epoch().count() == 0)
    {
        return FeStatus::None;
    }

    auto now = Clock::now();
    if ((now - lastFe) > feTimeout)
    {
        if (deviceState.load() == DeviceState::Disconnected)
        {
            return FeStatus::None;
        }

        return FeStatus::Interrupted;
    }

    return FeStatus::Active;
}

void CloseHauptwerkProcess()
{
	HWND mainWindow = g_hauptwerkMainWindow;
	g_hauptwerkMainWindow = nullptr;

	// Try a graceful close first by sending WM_CLOSE to the main
	// window, then fall back to a hard terminate if the process does
	// not exit within a short timeout.
	if (mainWindow && IsWindow(mainWindow))
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(mainWindow, &pid);
		printf("Requesting Hauptwerk close via WM_CLOSE. HWND=%p PID=%lu\n", mainWindow, static_cast<unsigned long>(pid));
		SendMessageW(mainWindow, WM_CLOSE, 0, 0);

		if (pid != 0)
		{
			HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (hProc)
			{
				DWORD waitResult = WaitForSingleObject(hProc, 5000); // wait up to 5s
				CloseHandle(hProc);
				if (waitResult != WAIT_OBJECT_0)
				{
					printf("Hauptwerk did not exit after WM_CLOSE, forcing termination.\n");
					CloseProcessByName(L"Hauptwerk.exe");

                    printf("From Midi.cpp, line %d: Sending MIDI message to reset Stream Deck buttons...\n", __LINE__);
                    SendUnloadOrganMidiMessage();
				}
				else
				{
					printf("Hauptwerk.exe process closed gracefully via WM_CLOSE. (line %d)\n", __LINE__);
                    SendUnloadOrganMidiMessage();
				}
			}
			else
			{
				// Could not open process handle, fall back to name-based terminate.
				CloseProcessByName(L"Hauptwerk.exe");
               
			}
		}
		else
		{
			// No PID associated with the window, use name-based terminate.
			CloseProcessByName(L"Hauptwerk.exe");
           
		}
	}
	else
	{
		// No valid window handle, still ensure the process is not left
		// running by terminating it by name.
		printf("No valid Hauptwerk window handle, forcing termination by name.\n");
		CloseProcessByName(L"Hauptwerk.exe");
       
	}
}

DWORD WINAPI WatchdogThread(LPVOID)
{
    while (running)
    {
        auto now = Clock::now();

        bool feAlive =
            (now - lastActiveSensingTime.load()) <= feTimeout;

        auto lastFe = lastFeTime.load();
        bool feMessageAlive = lastFe.time_since_epoch().count() != 0
            && (now - lastFe) <= feTimeout;

        bool notesAlive = anyNoteActive();

        bool disconnected = !(feAlive || notesAlive);
        if (disconnected && g_ignoreKeyReleaseOnDisconnect.load())
        {
            Sleep(50);
            continue;
        }

        if (disconnected && g_hauptwerkKeyHeld.load())
        {
            g_ignoreKeyReleaseOnDisconnect = true;
        }

        if (!disconnected)
        {
            g_ignoreKeyReleaseOnDisconnect = false;
        }

        bool shouldConnect = feMessageAlive
            || (deviceState.load() == DeviceState::Connected && (feAlive || notesAlive));

        DeviceState newState =
            shouldConnect
            ? DeviceState::Connected
            : DeviceState::Disconnected;

        // Apply an initial grace period after the first FE has been seen:
        // during this window, transient gaps in FE should not be treated as
        // real disconnections, to accommodate devices that send a single FE
        // on power-up, pause, then start a regular FE stream.
        auto firstFe = g_firstFeTime.load();
        if (newState == DeviceState::Disconnected &&
            deviceState.load() == DeviceState::Connected &&
            firstFe.time_since_epoch().count() != 0)
        {
            auto sinceFirstFe = now - firstFe;
            using namespace std::chrono;
            if (sinceFirstFe < seconds(10)) // grace period duration
            {
                newState = DeviceState::Connected;
            }
        }
        


        if (newState != deviceState.load())
        {
            if (newState == DeviceState::Disconnected)
            {
                printf("\n>>> *************************** >>>\n");
                printf(">>> AHLBORN 250 SL DISCONNECTED >>>\n");
                printf(">>> *************************** >>>\n");

                StopTrayIconFlashingInternal();
				UpdateTrayIconTooltip(L"AHLBORN 250 SL (OFF)");
                UpdateTrayIconFromFile(kIconDisabled);
                g_trayIconImageStatus = TrayIconImageStatus::Disabled;

                // Reset first-FE timestamp so that, on the next console
                // power-up, we again apply the initial grace period for
                // unstable FE streams (single FE, pause, then stable).
                g_firstFeTime = TimePoint{};
				
                if (!g_hauptwerkKeyHeld.load() )
                {
                    printf("Closing Hauptwerk (no key held).\n");
					
                    printf("g_midi_reset set to false.\n");
                    CloseHauptwerkProcess();

                    if (g_hauptwerkMainWindow == nullptr)
                    {
                        CloseSettingsWindowIfAutoClose();
                    }
                }
                else
                {
                    printf("Keeping Hauptwerk running (key held).\n");
                    UpdateTrayIconTooltip(L"AHLBORN 250 SL (standbye)");
                    UpdateTrayIconFromFile(kIconError);
                    g_trayIconImageStatus = TrayIconImageStatus::Error;
                    //g_hauptwerkMainWindow = nullptr;
                }
                clearAllNotes(); // panic safety
            }
            else
            {
                printf("\n>>> ************************ >>>\n");
                printf(">>> AHLBORN 250 SL CONNECTED >>>\n");
                printf(">>> ************************ >>>\n");
                g_hauptwerkKeyHeld = false;
                if (feMessageAlive && g_hauptwerkMainWindow == nullptr)
                    {
                        g_trayFlashFromFe = true;
                        StartTrayIconFlashing();
                    }
                else
                {
                   
                    if (isOrganLoaded)
                    {
                         
                         UpdateTrayIconFromFile(GetOnlineIconForCurrentDeviceState());
                         g_trayIconImageStatus = TrayIconImageStatus::Online;
                    }
                    else
                    {

                        UpdateTrayIconTooltip(L"Hauptwerk");
                        UpdateTrayIconFromFile(GetStandbyIconForCurrentDeviceState());
                        g_trayIconImageStatus = TrayIconImageStatus::Standby;
                    }
                }
                LaunchHauptwerkAndDismissWelcome();
                
            }

            deviceState = newState;
        }

        Sleep(50);
    }

    return 0;
}

bool startsWithFe()
{
    // initialize program start timestamp. do NOT set lastActiveSensingTime
   // to program start: keep it zero so we don't print "CONNECTED" on
   // first run when no FE has been received yet.
    programStart = Clock::now();
    lastActiveSensingTime = TimePoint{};
    lastFeTime = TimePoint{};
    g_firstFeTime = TimePoint{};
    running = true;

    hThread = CreateThread(
        nullptr,
        0,
        WatchdogThread,
        nullptr,
        0,
        nullptr);

    StartMidiOutWorker();
    StartDeferredCmdWorker();
    StartHauptwerkTitleMonitor();
    StartStreamDeckMonitor();

    if (hThread == NULL)
    {
        printf("Failed to create watchdog thread.\n");
        midiInStop(hMidiIn);
        midiInClose(hMidiIn);
        running = false;
        return false;
    }

    return true;
}

bool initMidiState()
{
    InitializeCriticalSection(&g_midiOutLock);

    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0)
    {
        printf("[initMidiState] : No MIDI input devices found.\n");
        g_inputDeviceOpen = false;
        return 1;
    }

    UINT devIndex = 0;
    UINT savedDeviceId = 0;
    bool hasSavedDevice = LoadSelectedDeviceId(savedDeviceId) && savedDeviceId < numDevs;
    if (hasSavedDevice)
    {
        devIndex = savedDeviceId;
    }

    // List available MIDI input devices and prompt the user to choose one.
    printf("\n[initMidiState] : Available MIDI input devices:\n\n");
    for (UINT i = 0; i < numDevs; ++i)
    {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            printf("  [%u] %S\n", i, caps.szPname);
        }
        else
        {
            printf("  [%u] (unknown)\n", i);
        }
    }

        printf("\n");

    // List available MIDI output devices
    UINT numOutDevsDbg = midiOutGetNumDevs();
    printf("[initMidiState] : Available MIDI output devices:\n\n");
    for (UINT i = 0; i < numOutDevsDbg; ++i)
    {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            printf("  [%u] %S\n", i, caps.szPname);
        }
        else
        {
            printf("  [%u] (unknown)\n", i);
        }
    }

        printf("\n");

    if (hasSavedDevice)
    {
       // printf("Using saved MIDI input device [%u].\n", devIndex);
    }
    else
    {
        printf("[initMidiState] : Using default MIDI input device [0].\n");
    }

    if (!OpenMidiDevice(devIndex))
    {
        g_inputDeviceOpen = false;
        return 1;
    }
    {
        bool input1Enabled = true;
        LoadMidiInput1DeviceEnabled(input1Enabled);
        if (!input1Enabled)
            CloseMidiInputDeviceOnly();
    }

    UINT savedDeviceId2 = 0;
    if (LoadSelectedInput2DeviceId(savedDeviceId2) && savedDeviceId2 < numDevs)
    {
        OpenMidiDevice2(savedDeviceId2);
        bool input2Enabled = true;
        LoadMidiInput2DeviceEnabled(input2Enabled);
        if (!input2Enabled)
            CloseMidiInput2DeviceOnly();
    }

    UINT numOutDevs = midiOutGetNumDevs();
    if (numOutDevs > 0)
    {
        UINT savedOutputDeviceId = 0;
        if (LoadSelectedOutputDeviceId(savedOutputDeviceId) && savedOutputDeviceId < numOutDevs)
        {
            SwitchMidiOutputDevice(savedOutputDeviceId);
        }
        else
        {
            SwitchMidiOutputDevice(0);
        }
        {
            bool output1Enabled = true;
            LoadMidiOutput1DeviceEnabled(output1Enabled);
            if (!output1Enabled)
                CloseMidiOutputDeviceOnly();
        }

        UINT savedOutput2DeviceId = 0;
        if (LoadSelectedOutput2DeviceId(savedOutput2DeviceId) && savedOutput2DeviceId < numOutDevs)
        {
            SwitchMidiOutput2Device(savedOutput2DeviceId);
            bool output2Enabled = true;
            LoadMidiOutput2DeviceEnabled(output2Enabled);
            if (!output2Enabled)
                CloseMidiOutput2DeviceOnly();
        }
    }
    else
    {
        g_outputDeviceOpen = false;
        g_output2DeviceOpen = false;
    }

    bool routerEnabled = false;
    if (LoadMidiRouterEnabled(routerEnabled))
    {
        g_midiRouterEnabled = routerEnabled;
    }
    printf("[initMidiState] : HAUPTWERK bridge enabled: [%s]\n\n", g_midiRouterEnabled.load() ? "true" : "false");

    // Start the Windows MIDI Services virtual endpoint (graceful no-op if unavailable)
    EnableMidi2Endpoint();

    RefreshSettingsFile();

    return true;
}

bool SwitchMidiInputDevice(UINT deviceId)
{
    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0)
    {
        printf("[SwitchMidiInputDevice] : No MIDI input devices found.\n");
        g_inputDeviceOpen = false;
        return false;
    }

    if (deviceId >= numDevs)
    {
        printf("[SwitchMidiInputDevice] : Selected MIDI device %u is out of range.\n", deviceId);
        g_inputDeviceOpen = false;
        return false;
    }

    if (hMidiIn != nullptr)
    {
        UINT currentId = 0;
        if (midiInGetID(hMidiIn, &currentId) == MMSYSERR_NOERROR)
        {
            printf("[SwitchMidiInputDevice] : Closing MIDI device [%u].\n", currentId);
        }
        else
        {
            printf("[SwitchMidiInputDevice] : Closing current MIDI device.\n");
        }

        midiInStop(hMidiIn);
        midiInReset(hMidiIn);
        midiInClose(hMidiIn);
        hMidiIn = nullptr;
    }

    return OpenMidiDevice(deviceId);
}

bool SwitchMidiInput2Device(UINT deviceId)
{
    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0)
    {
        printf("[SwitchMidiInput2Device] : No MIDI input devices found.\n");
        g_input2DeviceOpen = false;
        return false;
    }

    if (deviceId >= numDevs)
    {
        printf("[SwitchMidiInput2Device] : Selected MIDI device 2 %u is out of range.\n", deviceId);
        g_input2DeviceOpen = false;
        return false;
    }

    if (hMidiIn2 != nullptr)
    {
        UINT currentId = 0;
        if (midiInGetID(hMidiIn2, &currentId) == MMSYSERR_NOERROR)
        {
            printf("[SwitchMidiInput2Device] : Closing MIDI device 2 [%u].\n", currentId);
        }
        else
        {
            printf("[SwitchMidiInput2Device] : Closing current MIDI device 2.\n");
        }

        midiInStop(hMidiIn2);
        midiInReset(hMidiIn2);
        midiInClose(hMidiIn2);
        hMidiIn2 = nullptr;
    }

    return OpenMidiDevice2(deviceId);
}

bool RefreshMidiInputDevice()
{
    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0)
    {
        printf("[RefreshMidiInputDevice] : No MIDI input devices found.\n");
        g_inputDeviceOpen = false;
        return false;
    }

    UINT deviceId = 0;
    if (hMidiIn != nullptr && midiInGetID(hMidiIn, &deviceId) == MMSYSERR_NOERROR)
    {
        return SwitchMidiInputDevice(deviceId);
    }

    if (LoadSelectedDeviceId(deviceId) && deviceId < numDevs)
    {
        return SwitchMidiInputDevice(deviceId);
    }

    return SwitchMidiInputDevice(0);
}

bool SwitchMidiOutputDevice(UINT deviceId)
{
    UINT numDevs = midiOutGetNumDevs();
    if (numDevs == 0)
    {
        printf("[SwitchMidiOutputDevice] : No MIDI output devices found.\n");
        g_outputDeviceOpen = false;
        return false;
    }

    if (deviceId >= numDevs)
    {
        printf("[SwitchMidiOutputDevice] : Selected MIDI output device %u is out of range.\n", deviceId);
        g_outputDeviceOpen = false;
        return false;
    }

    EnterCriticalSection(&g_midiOutLock);
    if (hMidiOut != nullptr)
    {
        printf("[SwitchMidiOutputDevice] : Closing current MIDI output device.\n");
        midiOutReset(hMidiOut);
        midiOutClose(hMidiOut);
        hMidiOut = nullptr;
    }
    LeaveCriticalSection(&g_midiOutLock);

    return OpenMidiOutputDevice(deviceId);
}

bool SwitchMidiOutput2Device(UINT deviceId)
{
    UINT numDevs = midiOutGetNumDevs();
    if (numDevs == 0)
    {
        printf("[SwitchMidiOutput2Device] : No MIDI output devices found.\n");
        g_output2DeviceOpen = false;
        return false;
    }

    if (deviceId >= numDevs)
    {
        printf("[SwitchMidiOutput2Device] : Selected MIDI output device 2 %u is out of range.\n", deviceId);
        g_output2DeviceOpen = false;
        return false;
    }

    EnterCriticalSection(&g_midiOutLock);
    if (hMidiOut2 != nullptr)
    {
        printf("[SwitchMidiOutput2Device] : Closing current MIDI output device 2.\n");
        midiOutReset(hMidiOut2);
        midiOutClose(hMidiOut2);
        hMidiOut2 = nullptr;
    }
    LeaveCriticalSection(&g_midiOutLock);

    return OpenMidiOutput2Device(deviceId);
}

void CloseMidiInputDeviceOnly()
{
    if (hMidiIn != nullptr)
    {
        midiInStop(hMidiIn);
        midiInReset(hMidiIn);
        midiInClose(hMidiIn);
        hMidiIn = nullptr;
    }
    g_inputDeviceOpen = false;
}

void CloseMidiInput2DeviceOnly()
{
    if (hMidiIn2 != nullptr)
    {
        midiInStop(hMidiIn2);
        midiInReset(hMidiIn2);
        midiInClose(hMidiIn2);
        hMidiIn2 = nullptr;
    }
    g_input2DeviceOpen = false;
}

void CloseMidiOutputDeviceOnly()
{
    EnterCriticalSection(&g_midiOutLock);
    if (hMidiOut != nullptr)
    {
        midiOutReset(hMidiOut);
        midiOutClose(hMidiOut);
        hMidiOut = nullptr;
    }
    LeaveCriticalSection(&g_midiOutLock);
    g_outputDeviceOpen = false;
}

void CloseMidiOutput2DeviceOnly()
{
    EnterCriticalSection(&g_midiOutLock);
    if (hMidiOut2 != nullptr)
    {
        midiOutReset(hMidiOut2);
        midiOutClose(hMidiOut2);
        hMidiOut2 = nullptr;
    }
    LeaveCriticalSection(&g_midiOutLock);
    g_output2DeviceOpen = false;
}

bool IsMidiInputDeviceOpen()
{
    return g_inputDeviceOpen.load();
}

bool IsMidiInput2DeviceOpen()
{
    return g_input2DeviceOpen.load();
}

bool IsMidiOutputDeviceOpen()
{
    return g_outputDeviceOpen.load();
}

bool IsMidiOutput2DeviceOpen()
{
    return g_output2DeviceOpen.load();
}

void CleanupMidiLocks()
{
    DisableMidi2Endpoint();
    DeleteCriticalSection(&g_midiOutLock);
}

void RefreshMidiDeviceStatus()
{
    if (hMidiIn != nullptr)
    {
        UINT currentId = 0;
        g_inputDeviceOpen = midiInGetID(hMidiIn, &currentId) == MMSYSERR_NOERROR;
    }
    else
    {
        g_inputDeviceOpen = false;
    }

    if (hMidiIn2 != nullptr)
    {
        UINT currentId = 0;
        g_input2DeviceOpen = midiInGetID(hMidiIn2, &currentId) == MMSYSERR_NOERROR;
    }
    else
    {
        g_input2DeviceOpen = false;
    }

    if (hMidiOut != nullptr)
    {
        UINT currentId = 0;
        g_outputDeviceOpen = midiOutGetID(hMidiOut, &currentId) == MMSYSERR_NOERROR;
    }
    else
    {
        g_outputDeviceOpen = false;
    }

    if (hMidiOut2 != nullptr)
    {
        UINT currentId = 0;
        g_output2DeviceOpen = midiOutGetID(hMidiOut2, &currentId) == MMSYSERR_NOERROR;
    }
    else
    {
        g_output2DeviceOpen = false;
    }
}

void clearChannel(int ch)
{
    noteState[ch][0] = 0;
    noteState[ch][1] = 0;
    outputNoteState[ch][0] = 0;
    outputNoteState[ch][1] = 0;
}

void CALLBACK MidiInProc(
    HMIDIIN hMidiIn,
    UINT wMsg,
    DWORD_PTR dwInstance,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2)
{
	if (wMsg != MIM_DATA)
		return;

	DWORD msg = (DWORD)dwParam1;

	uint8_t status = msg & 0xFF;
	uint8_t data1 = (msg >> 8) & 0xFF;
	uint8_t data2 = (msg >> 16) & 0xFF;

	if (g_midiRouterEnabled.load() && hMidiOut != nullptr)
	{
		// Do not forward command messages (CC ch16 + BF_0x50/BF_0x51) to avoid
		// signal loops when the Stream Deck receives on the output port.
		bool isCommandMsg = (status == CC_ch16 && (data1 == BF_0x50 || data1 == BF_0x51));
		if (!isCommandMsg)
		{
			g_midiOutQueue.TryEnqueue(msg);
		}
	}

	// Also publish the message on the Windows MIDI Services virtual endpoint
	ForwardToMidi2Endpoint(msg);

	if (status == ALL_RESET)
	{
		printf("Received MIDI All Reset (0xFF). Clearing all notes.\n");
		printf("g_midi_reset set to true.\n");

		return;
	}

	if (status == CC_ch16 && data1 == AHLBORN_FISSATORE_CC)
	{
		if (data2 == AHLBORN_FISSATORE_DN && !g_hauptwerkKeyHeld.load())
		{
			g_hauptwerkKeyHeld = true;
				g_hauptwerkKeyLastPressTime = Clock::now();
				printf("[MidiInProc] : Hauptwerk [FISSATORE] held: true\n");
		}
		else if (data2 == AHLBORN_FISSATORE_UP && g_hauptwerkKeyHeld.load())
		{
			auto nowKey = Clock::now();
			auto pressTime = g_hauptwerkKeyLastPressTime;
			if (pressTime.time_since_epoch().count() != 0)
			{
				auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(nowKey - pressTime);
				if (delta <= kHauptwerkKeyTapThreshold)
				{
					OnHauptwerkShortPress();
				}
			}

			g_hauptwerkKeyLastPressTime = TimePoint{};
			g_hauptwerkKeyHeld = false;
			printf("[MidiInProc] : Hauptwerk [FISSATORE] held: false\n");
		}

		if (data2 == AHLBORN_FISSATORE_UP || data2 == AHLBORN_FISSATORE_DN)
		{
			return;
		}
	}

	auto now = Clock::now();

	// Treat any incoming MIDI message as recent device activity.
	// This ensures that if Active Sensing (0xFE) is not present but
	// other MIDI traffic continues, the watchdog will still consider
	// the device alive.
	lastActiveSensingTime = now;

	// compute timestamp relative to program start
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - programStart).count();

	// Active Sensing
	if (status == ACTIVE_SENSING)
	{
		// Remember time of the first FE received after startup so we can
		// apply an initial grace period where FE gaps are tolerated.
		if (g_firstFeTime.load().time_since_epoch().count() == 0)
		{
			g_firstFeTime.store(now);
		}

		// print delta since last FE
		auto prev = lastFeTime.load();
		long long delta = 0;
		bool prev_valid = prev.time_since_epoch().count() != 0;
		if (prev_valid)
			delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - prev).count();

		bool over_timeout = prev_valid && (delta > (long long)feTimeout.count());

		if (over_timeout)
		{
			// append '***' when delta is beyond timeout and add an empty line after
			//printf("[FE] +%lldms delta=%lldms ***\n\n", (long long)ms, (long long)delta);
		}
		else
		{
			//printf("[FE] +%lldms delta=%lldms\n", (long long)ms, (long long)delta);
		}

		lastFeTime = now;
		return;
	}

	uint8_t type = status & 0xF0;
	uint8_t ch = status & 0x0F;

	// For Note On/Off only print a short marker '---'
	if (type == 0x90 || type == 0x80)
	{
		printf("[MIDI ACTIVITY] +%lldms ---\n", (long long)ms);
	}
	else
	{
		printf("[MIDI ACTIVITY] +%lldms status=0x%02X data1=%u data2=%u\n", (long long)ms, status, data1, data2);
	}

	switch (type)
	{
	case 0x90: // Note On
		if (data2 > 0)
			setNoteOn(ch, data1);
		else
			setNoteOff(ch, data1);
		if (g_midiRouterEnabled.load() && hMidiOut != nullptr)
		{
			if (data2 > 0)
				setOutputNoteOn(ch, data1);
			else
				setOutputNoteOff(ch, data1);
		}
		break;

	case 0x80: // Note Off
		setNoteOff(ch, data1);
		if (g_midiRouterEnabled.load() && hMidiOut != nullptr)
		{
			setOutputNoteOff(ch, data1);
		}
		break;

	case 0xB0: // Control Change channels 1-16
	{
		if (data1 == 123 || data1 == 120)
			clearChannel(ch);
       
		// Esegue Unload organ se viene ricevuto CC ch16, data1= BF_0x50 o BF_0x51, data2 = 0
		// oppure Cancel se l'organo Ã¨ in caricamento
		if (ch == 0x0F && (data1 == BF_0x50 || data1 == BF_0x51) && data2 == 0)
		{
			printf("[MidiInProc] : Received MIDI message: CC ch16, data1= 0x%02X, data2 = 0\n", data1);
            if (isOrganLoaded)
            {
                printf("[MidiInProc] : Organo caricato : eseguo Unload organ\n");
                g_deferredCmdQueue.TryEnqueue(DeferredCmd::LoadFavoriteOrgan0);
                break;
            }
			if (g_isLoadingOrgan )
			{
				printf("[MidiInProc] : Organo in caricamento : eseguo Cancel\n");
				HWND hLoadingWnd = FindWindowW(nullptr, L"Loading organ");
				if (hLoadingWnd )
				{
					PostMessageW(hLoadingWnd, WM_KEYDOWN, VK_RETURN, 0);
					PostMessageW(hLoadingWnd, WM_KEYUP, VK_RETURN, 0);
				}
				break;
			}
		}
		
		// Esegue Load favorite organ 1..32 se viene ricevuto CC ch16, data1= BF_0x50, data2 = 1..32
		if (ch == 0x0F && data1 == BF_0x50 && data2 >= 1 && data2 <= 32)
		{
			int organIdx = static_cast<int>(data2);
			printf("[MidiInProc] : Received MIDI message: CC ch16, data1= BF_0x50, data2 = %d\n", organIdx);
			if (g_isLoadingOrgan)
			{
				printf("[MidiInProc] : Organo in caricamento : eseguo Cancel\n");
				HWND hLoadingWnd = FindWindowW(nullptr, L"Loading organ");
				if (hLoadingWnd)
				{
					PostMessageW(hLoadingWnd, WM_KEYDOWN, VK_RETURN, 0);
					PostMessageW(hLoadingWnd, WM_KEYUP, VK_RETURN, 0);
				}
			}
			g_pendingOrganLoad = true;
			SendUnloadOrganMidiMessage();
			EnqueueMidiOutMessage(CC_ch16 | (BF_0x50 << 8) | (static_cast<DWORD>(organIdx) << 16));
			DeferredCmd organCmd = static_cast<DeferredCmd>(
				static_cast<DWORD>(DeferredCmd::LoadFavoriteOrgan1) + static_cast<DWORD>(organIdx) - 1);
			g_deferredCmdQueue.TryEnqueue(organCmd);
			break;
		}

		// Esegue Load installed organ xx se viene ricevuto CC ch16, data1= BF_0x51, data2 = 1..127
		if (ch == 0x0F && data1 == BF_0x51 && data2 >= 1 && data2 <= 127)
		{
			int organIdx = static_cast<int>(data2);
			printf("[MidiInProc] : Received MIDI message: CC ch16, data1= BF_0x51 (Load installed organ %d)\n", organIdx);
			if (g_isLoadingOrgan)
			{
				printf("[MidiInProc] : Organo in caricamento : eseguo Cancel\n");
				HWND hLoadingWnd = FindWindowW(nullptr, L"Loading organ");
				if (hLoadingWnd)
				{
					PostMessageW(hLoadingWnd, WM_KEYDOWN, VK_RETURN, 0);
					PostMessageW(hLoadingWnd, WM_KEYUP, VK_RETURN, 0);
				}
			}
			g_pendingOrganLoad = true;
			EnqueueMidiOutMessage(CC_ch16 | (BF_0x51 << 8) | (0 << 16));
			EnqueueMidiOutMessage(CC_ch16 | (BF_0x51 << 8) | (static_cast<DWORD>(organIdx) << 16));
			g_pendingInstalledOrganIndex.store(organIdx);
			g_deferredCmdQueue.TryEnqueue(DeferredCmd::LoadInstalledOrgan);
			break;
		}
	}
		break;

	default:
		break;
	}
}
