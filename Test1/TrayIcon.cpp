#include "TrayIcon.h"
#include "MidiAssignmentWindow.h"
#include "Midi.h"
#include "Xml.h"
#include "Hauptwerk.h"
#include "AutoUpdate.h"
#include "StreamDeck.h"
#include "StreamDeck_profiler.h"
#include "Version.h"

#include <shellapi.h>
#include <gdiplus.h>
#include <dbt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <atomic>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

static NOTIFYICONDATAW g_nid = {};
static HICON g_trayIcon = nullptr;
static const wchar_t kSettingsClassName[] = L"AhlbornBridgeSettingsWindow";
static const wchar_t kLedStripClassName[] = L"AhlbornBridgeMidiLedStrip";
static const wchar_t kSettingsPageClassName[] = L"AhlbornBridgeSettingsPage";
static HWND g_settingsHwnd = nullptr;
static HWND g_settingsTabHwnd = nullptr;
static HWND g_settingsMidiPageHwnd = nullptr;
static HWND g_settingsInfoPageHwnd = nullptr;
static HWND g_settingsOrganInfoPageHwnd = nullptr;
static HWND g_settingsHauptwerkPageHwnd = nullptr;
static HWND g_settingsBidulePageHwnd = nullptr;
static HWND g_settingsOrganInfoGroupHwnd = nullptr;
static HWND g_settingsAboutPageHwnd = nullptr;
static HWND g_settingsStreamDeckPageHwnd = nullptr;
static HWND g_feLedHwnd = nullptr;
static HWND g_activeSensingOutputComboHwnd = nullptr;
static HWND g_hauptwerkOrganListHwnd = nullptr;
static HWND g_hauptwerkAudioDeviceComboHwnd = nullptr;
static HWND g_bidulePathEditHwnd = nullptr;
static HWND g_biduleCloseOnUnloadCheckHwnd = nullptr;
static std::atomic<bool> g_closeSettingsOnDisconnect{ false };

constexpr UINT kLedTimerId = 1;
constexpr UINT kLedTimerIntervalMs = 30;
constexpr DWORD kInputRefreshBlinkIntervalMs = 150;
constexpr DWORD kInputRefreshDurationMs = 1200;
constexpr int kAutoCloseCheckId = 201;
constexpr int kShowConsoleCheckId = 202;
constexpr int kCheckForUpdateOnStartCheckId = 203;
constexpr int kSdSendButtonId = 304;
constexpr int kSdPluginUpdateButtonId = 305;
constexpr int kMainWindowBehaviorComboId = 401;
constexpr int kMainDebugConsoleComboId = 402;
constexpr int kActiveSensingCheckId = 403;
constexpr int kActiveSensingOutputComboId = 404;
constexpr int kMainUpdateModeComboId = 403;
constexpr int kMainCheckNowButtonId = 404;
constexpr int kHauptwerkOrganListId = 501;
constexpr int kHauptwerkAudioDeviceComboId = 502;
constexpr int kHauptwerkSaveAudioAssignmentButtonId = 503;
constexpr int kBiduleBrowseButtonId = 601;
constexpr int kBiduleAutoDetectButtonId = 602;
constexpr int kBiduleCloseOnUnloadCheckId = 603;

namespace
{
    std::vector<InstalledOrganInfo> g_hauptwerkOrgans;
    std::vector<AudioDeviceInfo> g_hauptwerkAudioDevices;

    void HandleMidiDeviceChange(HWND hWnd)
    {
        RefreshMidiDeviceStatus();
        RefreshSettingsFile();
        RefreshMidiAssignmentWindowIfOpen();
        if (g_feLedHwnd)
        {
            InvalidateRect(g_feLedHwnd, nullptr, FALSE);
        }
    }

    void UpdateOrganInfoGroupTitle();

    void RefreshBiduleSettingsUI()
    {
        if (g_bidulePathEditHwnd)
        {
            std::wstring bidulePath = DetectBiduleExePath();
            SetWindowTextW(g_bidulePathEditHwnd, bidulePath.c_str());
        }

        if (g_biduleCloseOnUnloadCheckHwnd)
        {
            bool closeOnUnload = true;
            LoadBiduleCloseOnUnload(closeOnUnload);
            SendMessageW(g_biduleCloseOnUnloadCheckHwnd, BM_SETCHECK,
                closeOnUnload ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    bool PickBiduleExecutable(HWND hOwner, std::wstring& path)
    {
        wchar_t fileName[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hOwner;
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = _countof(fileName);
        ofn.lpstrFilter = L"Executable files (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle = L"Select Bidule executable";
        if (!GetOpenFileNameW(&ofn))
            return false;

        path = fileName;
        return true;
    }

    void RefreshHauptwerkAudioAssignmentsUI()
    {
        g_hauptwerkOrgans = LoadInstalledOrganInfos();
        g_hauptwerkAudioDevices = LoadAudioOutputDevices();

        if (g_hauptwerkOrganListHwnd)
        {
            ListView_DeleteAllItems(g_hauptwerkOrganListHwnd);
            for (int i = 0; i < static_cast<int>(g_hauptwerkOrgans.size()); ++i)
            {
                const auto& organ = g_hauptwerkOrgans[i];
                LVITEMW item = {};
                item.mask = LVIF_TEXT;
                item.iItem = i;
                item.pszText = const_cast<LPWSTR>(organ.name.c_str());
                ListView_InsertItem(g_hauptwerkOrganListHwnd, &item);

                ListView_SetItemText(g_hauptwerkOrganListHwnd, i, 1, const_cast<LPWSTR>(organ.id.c_str()));
                ListView_SetItemText(g_hauptwerkOrganListHwnd, i, 2, const_cast<LPWSTR>(organ.uniqueOrganId.c_str()));
                std::wstring channels = organ.numberChannels > 0 ? std::to_wstring(organ.numberChannels) : L"";
                ListView_SetItemText(g_hauptwerkOrganListHwnd, i, 3, const_cast<LPWSTR>(channels.c_str()));

                std::wstring deviceName;
                for (const auto& device : g_hauptwerkAudioDevices)
                {
                    if (device.id == organ.outputDeviceId)
                    {
                        deviceName = device.name;
                        break;
                    }
                }
                ListView_SetItemText(g_hauptwerkOrganListHwnd, i, 4, const_cast<LPWSTR>(deviceName.c_str()));
                ListView_SetItemText(g_hauptwerkOrganListHwnd, i, 5, const_cast<LPWSTR>(organ.biduleProfile.c_str()));
            }
        }

        if (g_hauptwerkAudioDeviceComboHwnd)
        {
            SendMessageW(g_hauptwerkAudioDeviceComboHwnd, CB_RESETCONTENT, 0, 0);
            for (const auto& device : g_hauptwerkAudioDevices)
                SendMessageW(g_hauptwerkAudioDeviceComboHwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
            SendMessageW(g_hauptwerkAudioDeviceComboHwnd, CB_SETCURSEL, 0, 0);
        }
    }

    void SyncHauptwerkAudioComboToSelection()
    {
        if (!g_hauptwerkOrganListHwnd || !g_hauptwerkAudioDeviceComboHwnd)
            return;

        int sel = ListView_GetNextItem(g_hauptwerkOrganListHwnd, -1, LVNI_SELECTED);
        if (sel < 0 || sel >= static_cast<int>(g_hauptwerkOrgans.size()))
            return;

        const auto& organ = g_hauptwerkOrgans[sel];
        for (int i = 0; i < static_cast<int>(g_hauptwerkAudioDevices.size()); ++i)
        {
            if (g_hauptwerkAudioDevices[i].id == organ.outputDeviceId)
            {
                SendMessageW(g_hauptwerkAudioDeviceComboHwnd, CB_SETCURSEL, i, 0);
                return;
            }
        }
    }

    bool EnsureGdiplusStarted()
    {
        static ULONG_PTR token = 0;
        static bool started = false;
        if (started)
        {
            return true;
        }

        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        {
            return false;
        }

        started = true;
        return true;
    }

    bool EnsureLedStripClass(HINSTANCE hInstance)
    {
        WNDCLASSW wc = {};
        if (GetClassInfoW(hInstance, kLedStripClassName, &wc))
        {
            return true;
        }

        wc.lpfnWndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
        {
            switch (message)
            {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps = {};
                HDC hdc = BeginPaint(hWnd, &ps);
                RECT rect = {};
                GetClientRect(hWnd, &rect);
                HDC memDc = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rect.right - rect.left, rect.bottom - rect.top);
                HBITMAP oldBitmap = reinterpret_cast<HBITMAP>(SelectObject(memDc, memBitmap));
                FillRect(memDc, &rect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

                constexpr int ledSize = 12;
                constexpr int spacing = 4;
                constexpr int margin = 4;
                int y = margin;
                LONG_PTR controlId = GetWindowLongPtrW(hWnd, GWLP_ID);
                bool isFe = controlId == 106;
                if (isFe)
                {
                    RECT ledRect{ margin, y, margin + ledSize, y + ledSize };
                    COLORREF color = RGB(0, 0, 0);
                    switch (GetFeStatus())
                    {
                    case FeStatus::Active:
                        color = RGB(0, 200, 0);
                        break;
                    case FeStatus::Interrupted:
                    {
                        constexpr DWORD kBlinkIntervalMs = 300;
                        DWORD tick = GetTickCount();
                        bool showGreen = (tick / kBlinkIntervalMs) % 2 == 0;
                        color = showGreen ? RGB(0, 200, 0) : RGB(220, 0, 0);
                        break;
                    }
                    case FeStatus::None:
                    default:
                        color = RGB(0, 0, 0);
                        break;
                    }

                    HBRUSH brush = CreateSolidBrush(color);
                    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(memDc, brush));
                    Ellipse(memDc, ledRect.left, ledRect.top, ledRect.right, ledRect.bottom);
                    SelectObject(memDc, oldBrush);
                    DeleteObject(brush);
                }

                BitBlt(hdc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, oldBitmap);
                DeleteObject(memBitmap);
                DeleteDC(memDc);

                EndPaint(hWnd, &ps);
                return 0;
            }
            default:
                break;
            }

            return DefWindowProcW(hWnd, message, wParam, lParam);
        };
        wc.hInstance = hInstance;
        wc.lpszClassName = kLedStripClassName;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        return RegisterClassW(&wc) != 0;
    }

    bool EnsureSettingsPageClass(HINSTANCE hInstance)
    {
        WNDCLASSW wc = {};
        if (GetClassInfoW(hInstance, kSettingsPageClassName, &wc))
        {
            return true;
        }

        wc.lpfnWndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
        {
            switch (message)
            {
            case WM_COMMAND:
            case WM_NOTIFY:
            {
                HWND parent = GetParent(hWnd);
                if (parent)
                {
                    return SendMessageW(parent, message, wParam, lParam);
                }
                break;
            }
            default:
                break;
            }

            return DefWindowProcW(hWnd, message, wParam, lParam);
        };
        wc.hInstance = hInstance;
        wc.lpszClassName = kSettingsPageClassName;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        return RegisterClassW(&wc) != 0;
    }

    void RefreshMidiComboSelections(HWND hWnd)
    {
        HWND owner = g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd;
        HWND hCombo = GetDlgItem(owner, 101);
        if (hCombo)
        {
            int currentSel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            PopulateMidiInputs(hCombo);
            UINT savedDeviceId = 0;
            int count = static_cast<int>(SendMessageW(hCombo, CB_GETCOUNT, 0, 0));
            if (count > 0 && LoadSelectedDeviceId(savedDeviceId) && savedDeviceId < static_cast<UINT>(count))
            {
                SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(savedDeviceId), 0);
            }
            else if (currentSel >= 0 && currentSel < count)
            {
                SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(currentSel), 0);
            }
        }

        HWND hCombo2 = GetDlgItem(owner, 109);
        if (hCombo2)
        {
            int currentSel = static_cast<int>(SendMessageW(hCombo2, CB_GETCURSEL, 0, 0));
            PopulateMidiInputs(hCombo2);
            UINT savedDeviceId2 = 0;
            int count2 = static_cast<int>(SendMessageW(hCombo2, CB_GETCOUNT, 0, 0));
            if (count2 > 0 && LoadSelectedInput2DeviceId(savedDeviceId2) && savedDeviceId2 < static_cast<UINT>(count2))
            {
                SendMessageW(hCombo2, CB_SETCURSEL, static_cast<WPARAM>(savedDeviceId2), 0);
            }
            else if (currentSel >= 0 && currentSel < count2)
            {
                SendMessageW(hCombo2, CB_SETCURSEL, static_cast<WPARAM>(currentSel), 0);
            }
        }

        HWND hOutputCombo = GetDlgItem(owner, 102);
        if (hOutputCombo)
        {
            int currentSel = static_cast<int>(SendMessageW(hOutputCombo, CB_GETCURSEL, 0, 0));
            PopulateMidiOutputs(hOutputCombo);
            UINT savedOutputDeviceId = 0;
            int outputCount = static_cast<int>(SendMessageW(hOutputCombo, CB_GETCOUNT, 0, 0));
            if (outputCount > 0 && LoadSelectedOutputDeviceId(savedOutputDeviceId)
                && savedOutputDeviceId < static_cast<UINT>(outputCount))
            {
                SendMessageW(hOutputCombo, CB_SETCURSEL, static_cast<WPARAM>(savedOutputDeviceId), 0);
            }
            else if (currentSel >= 0 && currentSel < outputCount)
            {
                SendMessageW(hOutputCombo, CB_SETCURSEL, static_cast<WPARAM>(currentSel), 0);
            }
        }

        HWND hOutput2Combo = GetDlgItem(owner, 111);
        if (hOutput2Combo)
        {
            int currentSel = static_cast<int>(SendMessageW(hOutput2Combo, CB_GETCURSEL, 0, 0));
            PopulateMidiOutputs(hOutput2Combo);
            UINT savedOutput2DeviceId = 0;
            int output2Count = static_cast<int>(SendMessageW(hOutput2Combo, CB_GETCOUNT, 0, 0));
            if (output2Count > 0 && LoadSelectedOutput2DeviceId(savedOutput2DeviceId)
                && savedOutput2DeviceId < static_cast<UINT>(output2Count))
            {
                SendMessageW(hOutput2Combo, CB_SETCURSEL, static_cast<WPARAM>(savedOutput2DeviceId), 0);
            }
            else if (currentSel >= 0 && currentSel < output2Count)
            {
                SendMessageW(hOutput2Combo, CB_SETCURSEL, static_cast<WPARAM>(currentSel), 0);
            }
        }
    }

    void UpdateOrganInfoGroupTitle()
    {
        if (!g_settingsOrganInfoGroupHwnd)
        {
            return;
        }
        static std::wstring lastTitle;
        std::wstring title;
        if (!g_hauptwerkOrganTitle.empty())
        {
            title = L"Organ - ";
            title += g_hauptwerkOrganTitle;
        }

        if (title == lastTitle)
        {
            return;
        }

        lastTitle = title;
        SetWindowTextW(g_settingsOrganInfoGroupHwnd, title.c_str());
    }
}

void PopulateMidiInputs(HWND hCombo)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    UINT numDevs = midiInGetNumDevs();
    if (numDevs == 0)
    {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(No MIDI inputs found)");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        return;
    }

    MIDIINCAPS caps = {};
    for (UINT i = 0; i < numDevs; ++i)
    {
        if (midiInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
        }
    }

    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

void PopulateMidiOutputs(HWND hCombo)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    UINT numDevs = midiOutGetNumDevs();
    if (numDevs == 0)
    {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(No MIDI outputs found)");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        return;
    }

    MIDIOUTCAPS caps = {};
    for (UINT i = 0; i < numDevs; ++i)
    {
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
        }
    }

    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        RECT clientRect = {};
        GetClientRect(hWnd, &clientRect);
        const int margin = 8;
        g_settingsTabHwnd = CreateWindowW(WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            margin, margin,
            clientRect.right - clientRect.left - (margin * 2),
            clientRect.bottom - clientRect.top - (margin * 2),
            hWnd, nullptr, nullptr, nullptr);

        TCITEMW tabItem = {};
        tabItem.mask = TCIF_TEXT;
        tabItem.pszText = const_cast<wchar_t*>(L"MIDI");
        TabCtrl_InsertItem(g_settingsTabHwnd, 0, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Main");
        TabCtrl_InsertItem(g_settingsTabHwnd, 1, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Organ Info...");
        TabCtrl_InsertItem(g_settingsTabHwnd, 2, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Hauptwerk");
        TabCtrl_InsertItem(g_settingsTabHwnd, 3, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Bidule");
        TabCtrl_InsertItem(g_settingsTabHwnd, 4, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Stream Deck");
        TabCtrl_InsertItem(g_settingsTabHwnd, 5, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"About");
        TabCtrl_InsertItem(g_settingsTabHwnd, 6, &tabItem);

        RECT tabRect = {};
        GetClientRect(g_settingsTabHwnd, &tabRect);
        TabCtrl_AdjustRect(g_settingsTabHwnd, FALSE, &tabRect);
        MapWindowPoints(g_settingsTabHwnd, hWnd, reinterpret_cast<LPPOINT>(&tabRect), 2);

        g_settingsMidiPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD | WS_VISIBLE,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsInfoPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsOrganInfoPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsHauptwerkPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsAboutPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsBidulePageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        g_settingsStreamDeckPageHwnd = CreateWindowW(kSettingsPageClassName, nullptr, WS_CHILD,
            tabRect.left, tabRect.top,
            tabRect.right - tabRect.left, tabRect.bottom - tabRect.top,
            hWnd, nullptr, nullptr, nullptr);

        // --- Stream Deck page controls ---
        {
            CreateWindowW(L"BUTTON", L"Stream Deck Profile", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 8, 300, 100, g_settingsStreamDeckPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Create a Stream Deck profile with one\n"
                L"button for each installed organ.",
                WS_CHILD | WS_VISIBLE,
                22, 32, 270, 36, g_settingsStreamDeckPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Create Profile",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                22, 76, 120, 24, g_settingsStreamDeckPageHwnd, (HMENU)kSdSendButtonId, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Plugin Update", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 116, 300, 80, g_settingsStreamDeckPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Check GitHub for a newer version of the\n"
                L"AhlbornBridge Stream Deck plugin.",
                WS_CHILD | WS_VISIBLE,
                22, 140, 270, 36, g_settingsStreamDeckPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Check for Update",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                22, 176, 140, 24, g_settingsStreamDeckPageHwnd, (HMENU)kSdPluginUpdateButtonId, nullptr, nullptr);
        }

        {
            CreateWindowW(L"BUTTON", L"Bidule Integration", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 8, 740, 220, g_settingsBidulePageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Executable:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                24, 42, 100, 20, g_settingsBidulePageHwnd, nullptr, nullptr, nullptr);

            g_bidulePathEditHwnd = CreateWindowW(L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                136, 38, 430, 24, g_settingsBidulePageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Browse...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                580, 38, 90, 24, g_settingsBidulePageHwnd, (HMENU)kBiduleBrowseButtonId, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Auto-detect",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                580, 72, 90, 24, g_settingsBidulePageHwnd, (HMENU)kBiduleAutoDetectButtonId, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Bidule is launched automatically when the selected organ uses Hauptwerk VST Link.",
                WS_CHILD | WS_VISIBLE,
                136, 76, 410, 18, g_settingsBidulePageHwnd, nullptr, nullptr, nullptr);

            g_biduleCloseOnUnloadCheckHwnd = CreateWindowW(L"BUTTON", L"Close Bidule automatically on Unload organ",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                136, 114, 360, 20, g_settingsBidulePageHwnd, (HMENU)kBiduleCloseOnUnloadCheckId, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"If Bidule is not found automatically, choose Bidule.exe manually here.",
                WS_CHILD | WS_VISIBLE,
                136, 146, 430, 18, g_settingsBidulePageHwnd, nullptr, nullptr, nullptr);

            RefreshBiduleSettingsUI();
        }

        {
            CreateWindowW(L"BUTTON", L"About AhlbornBridge", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 8, 300, 120, g_settingsAboutPageHwnd, nullptr, nullptr, nullptr);

            wchar_t versionBuf[128] = {};
            wsprintfW(versionBuf, L"Version: %hs", APP_VERSION);
            CreateWindowW(L"STATIC", versionBuf, WS_CHILD | WS_VISIBLE,
                22, 32, 270, 20, g_settingsAboutPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"\x00A9 2025 paoloantiga70", WS_CHILD | WS_VISIBLE,
                22, 56, 270, 20, g_settingsAboutPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"github.com/paoloantiga70/AhlbornBridge", WS_CHILD | WS_VISIBLE,
                22, 80, 270, 20, g_settingsAboutPageHwnd, nullptr, nullptr, nullptr);
        }

        RECT organRect = {};
        GetClientRect(g_settingsOrganInfoPageHwnd, &organRect);
        g_settingsOrganInfoGroupHwnd = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            organRect.left, organRect.top,
            organRect.right - organRect.left, organRect.bottom - organRect.top,
            g_settingsOrganInfoPageHwnd, nullptr, nullptr, nullptr);
        UpdateOrganInfoGroupTitle();

        {
            CreateWindowW(L"BUTTON", L"Hauptwerk Integration", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                10, 8, 740, 500, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Installed organs audio output",
                WS_CHILD | WS_VISIBLE,
                24, 32, 260, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Select an organ, then assign the audio output device to be written into Output_Device.",
                WS_CHILD | WS_VISIBLE,
                24, 52, 680, 18, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                24, 78, 700, 2, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            g_hauptwerkOrganListHwnd = CreateWindowW(WC_LISTVIEWW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                24, 96, 700, 300, g_settingsHauptwerkPageHwnd, (HMENU)kHauptwerkOrganListId, nullptr, nullptr);

            ListView_SetExtendedListViewStyle(g_hauptwerkOrganListHwnd, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<LPWSTR>(L"Organ"); col.cx = 180; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 0, &col);
            col.pszText = const_cast<LPWSTR>(L"ID"); col.cx = 50; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 1, &col);
            col.pszText = const_cast<LPWSTR>(L"Unique ID"); col.cx = 120; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 2, &col);
            col.pszText = const_cast<LPWSTR>(L"Channels"); col.cx = 70; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 3, &col);
            col.pszText = const_cast<LPWSTR>(L"Audio Device"); col.cx = 180; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 4, &col);
            col.pszText = const_cast<LPWSTR>(L"Bidule Profile"); col.cx = 220; ListView_InsertColumn(g_hauptwerkOrganListHwnd, 5, &col);

            CreateWindowW(L"STATIC", L"Assign device:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                24, 418, 120, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            g_hauptwerkAudioDeviceComboHwnd = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                156, 414, 410, 220, g_settingsHauptwerkPageHwnd, (HMENU)kHauptwerkAudioDeviceComboId, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Save assignment",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                584, 414, 140, 24, g_settingsHauptwerkPageHwnd, (HMENU)kHauptwerkSaveAudioAssignmentButtonId, nullptr, nullptr);

            RefreshHauptwerkAudioAssignmentsUI();
        }

        {
            const int leftLabel = 26;
            const int leftControl = 248;
            const int controlWidth = 320;
            const int rowHeight = 24;
            const int rowGap = 44;
            const int baseY = 58;

            CreateWindowW(L"STATIC", L"Main preferences",
                WS_CHILD | WS_VISIBLE,
                24, 16, 220, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"Configure the default behavior of AhlbornBridge in a single place.",
                WS_CHILD | WS_VISIBLE,
                24, 36, 470, 18, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                24, 64, 700, 2, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Settings window behavior:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                leftLabel, baseY + 8, 206, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            HWND hWindowBehaviorCombo = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                leftControl, baseY, controlWidth, 200,
                g_settingsInfoPageHwnd, (HMENU)kMainWindowBehaviorComboId, nullptr, nullptr);
            SendMessageW(hWindowBehaviorCombo, CB_ADDSTRING, 0, (LPARAM)L"Keep preferences window open");
            SendMessageW(hWindowBehaviorCombo, CB_ADDSTRING, 0, (LPARAM)L"Close automatically when all MIDI devices disconnect");

            bool closeOnDisconnect = false;
            LoadCloseSettingsOnDisconnect(closeOnDisconnect);
            g_closeSettingsOnDisconnect = closeOnDisconnect;
            SendMessageW(hWindowBehaviorCombo, CB_SETCURSEL, closeOnDisconnect ? 1 : 0, 0);

            CreateWindowW(L"STATIC", L"Debug console visibility:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                leftLabel, baseY + rowGap + 8, 206, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            HWND hDebugConsoleCombo = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                leftControl, baseY + rowGap, controlWidth, 200,
                g_settingsInfoPageHwnd, (HMENU)kMainDebugConsoleComboId, nullptr, nullptr);
            SendMessageW(hDebugConsoleCombo, CB_ADDSTRING, 0, (LPARAM)L"Show debug console window");
            SendMessageW(hDebugConsoleCombo, CB_ADDSTRING, 0, (LPARAM)L"Hide debug console window");

            bool showConsole = true;
            LoadShowDebugConsole(showConsole);
            SendMessageW(hDebugConsoleCombo, CB_SETCURSEL, showConsole ? 0 : 1, 0);

            CreateWindowW(L"STATIC", L"Application update checks:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                leftLabel, baseY + (rowGap * 2) + 8, 206, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            HWND hUpdateModeCombo = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                leftControl, baseY + (rowGap * 2), controlWidth, 200,
                g_settingsInfoPageHwnd, (HMENU)kMainUpdateModeComboId, nullptr, nullptr);
            SendMessageW(hUpdateModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Check GitHub for updates at startup");
            SendMessageW(hUpdateModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Check manually only");

            bool checkForUpdateOnStart = true;
            LoadCheckForUpdateOnStart(checkForUpdateOnStart);
            SendMessageW(hUpdateModeCombo, CB_SETCURSEL, checkForUpdateOnStart ? 0 : 1, 0);

            CreateWindowW(L"STATIC", L"Current application version:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                leftLabel, baseY + (rowGap * 3) + 8, 206, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            wchar_t versionText[128] = {};
            wsprintfW(versionText, L"%hs", APP_VERSION);
            CreateWindowW(L"STATIC", versionText,
                WS_CHILD | WS_VISIBLE,
                leftControl + 6, baseY + (rowGap * 3) + 8, 180, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Update source:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                leftLabel, baseY + (rowGap * 4) + 8, 206, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"GitHub Releases - paoloantiga70/AhlbornBridge2",
                WS_CHILD | WS_VISIBLE,
                leftControl + 6, baseY + (rowGap * 4) + 8, 360, 20, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Check for Update Now",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                leftControl, baseY + (rowGap * 5), 170, rowHeight,
                g_settingsInfoPageHwnd, (HMENU)kMainCheckNowButtonId, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"This first tab uses a cleaner Hauptwerk-style aligned layout. The other tabs can follow the same model.",
                WS_CHILD | WS_VISIBLE,
                24, baseY + (rowGap * 6), 700, 18, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);
        }

        CreateWindowW(L"BUTTON", L"FE (Active sensing)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 10, 500, 106, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        g_feLedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            22, 30, 20, 20, g_settingsMidiPageHwnd, (HMENU)106, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"FE", WS_CHILD | WS_VISIBLE,
            48, 32, 120, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        {
            bool activeSensingEnabled = true;
            LoadActiveSensingEnabled(activeSensingEnabled);
            g_activeSensingEnabled.store(activeSensingEnabled);
            HWND hActiveSensingCheck = CreateWindowW(L"BUTTON", L"Enable Active Sensing sender (0xFE every 200 ms)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                22, 52, 460, 18, g_settingsMidiPageHwnd, (HMENU)kActiveSensingCheckId, nullptr, nullptr);
            SendMessageW(hActiveSensingCheck, BM_SETCHECK, activeSensingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        {
            CreateWindowW(L"STATIC", L"Output port:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                22, 80, 100, 18, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

            g_activeSensingOutputComboHwnd = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                128, 76, 370, 200, g_settingsMidiPageHwnd,
                (HMENU)kActiveSensingOutputComboId, nullptr, nullptr);

            // Populate with all available MIDI output devices
            std::wstring savedOutputName;
            LoadActiveSensingOutputName(savedOutputName);
            if (savedOutputName.empty())
                savedOutputName = L"Default App Loopback (A)";

            UINT numOutDevs = midiOutGetNumDevs();
            int selIdx = 0;
            for (UINT i = 0; i < numOutDevs; ++i)
            {
                MIDIOUTCAPS caps = {};
                if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
                {
                    int idx = static_cast<int>(SendMessageW(g_activeSensingOutputComboHwnd,
                        CB_ADDSTRING, 0, (LPARAM)caps.szPname));
                    if (std::wstring(caps.szPname) == savedOutputName)
                        selIdx = idx;
                }
            }
            SendMessageW(g_activeSensingOutputComboHwnd, CB_SETCURSEL, selIdx, 0);
        }

        SetTimer(hWnd, kLedTimerId, kLedTimerIntervalMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
    {
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kActiveSensingCheckId)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
                hCheck = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, kActiveSensingCheckId);
            bool enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveActiveSensingEnabled(enabled);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == kActiveSensingOutputComboId)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo) hCombo = g_activeSensingOutputComboHwnd;
            if (hCombo)
            {
                int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR)
                {
                    wchar_t buf[256] = {};
                    SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                    SaveActiveSensingOutputName(buf);
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kAutoCloseCheckId)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
            {
                hCheck = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kAutoCloseCheckId);
            }

            bool enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_closeSettingsOnDisconnect = enabled;
            SaveCloseSettingsOnDisconnect(enabled);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kShowConsoleCheckId)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
            {
                hCheck = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kShowConsoleCheckId);
            }

            bool show = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            HWND hConsoleWnd = GetConsoleWindow();
            if (hConsoleWnd)
            {
                ShowWindow(hConsoleWnd, show ? SW_SHOW : SW_HIDE);
            }
            SaveShowDebugConsole(show);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kCheckForUpdateOnStartCheckId)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
            {
                hCheck = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kCheckForUpdateOnStartCheckId);
            }

            bool enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveCheckForUpdateOnStart(enabled);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == kMainWindowBehaviorComboId)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kMainWindowBehaviorComboId);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            bool enabled = (selectedIndex == 1);
            g_closeSettingsOnDisconnect = enabled;
            SaveCloseSettingsOnDisconnect(enabled);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == kMainDebugConsoleComboId)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kMainDebugConsoleComboId);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            bool show = (selectedIndex != 1);
            HWND hConsoleWnd = GetConsoleWindow();
            if (hConsoleWnd)
            {
                ShowWindow(hConsoleWnd, show ? SW_SHOW : SW_HIDE);
            }
            SaveShowDebugConsole(show);
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == kMainUpdateModeComboId)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsInfoPageHwnd ? g_settingsInfoPageHwnd : hWnd, kMainUpdateModeComboId);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            SaveCheckForUpdateOnStart(selectedIndex != 1);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kMainCheckNowButtonId)
        {
            CheckForUpdateInteractive(hWnd);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kSdSendButtonId)
        {
            // Stop Stream Deck so we can safely write profile files
            system("taskkill /f /im StreamDeck.exe >nul 2>&1");
            Sleep(2000);

            bool ok = CreateStreamDeckProfileFromSettings();

            // Restart Stream Deck to pick up the new/updated profile
            Sleep(1000);
            ShellExecuteW(nullptr, L"open",
                L"C:\\Program Files\\Elgato\\StreamDeck\\StreamDeck.exe",
                nullptr, nullptr, SW_HIDE);

            if (ok)
                MessageBoxW(hWnd, L"Stream Deck profile created!\nStream Deck has been restarted.", L"Stream Deck", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxW(hWnd, L"Failed to create Stream Deck profile.\nCheck that organs are installed.", L"Stream Deck", MB_OK | MB_ICONERROR);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kSdPluginUpdateButtonId)
        {
            CheckForPluginUpdateInteractive(hWnd);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kHauptwerkSaveAudioAssignmentButtonId)
        {
            if (!g_hauptwerkOrganListHwnd || !g_hauptwerkAudioDeviceComboHwnd)
                return 0;

            int selectedOrgan = ListView_GetNextItem(g_hauptwerkOrganListHwnd, -1, LVNI_SELECTED);
            int selectedDevice = static_cast<int>(SendMessageW(g_hauptwerkAudioDeviceComboHwnd, CB_GETCURSEL, 0, 0));
            if (selectedOrgan < 0 || selectedOrgan >= static_cast<int>(g_hauptwerkOrgans.size()) ||
                selectedDevice == CB_ERR || selectedDevice >= static_cast<int>(g_hauptwerkAudioDevices.size()))
            {
                MessageBoxW(hWnd, L"Select an organ and an audio device first.", L"Hauptwerk", MB_OK | MB_ICONWARNING);
                return 0;
            }

            const auto& organ = g_hauptwerkOrgans[selectedOrgan];
            const auto& device = g_hauptwerkAudioDevices[selectedDevice];
            if (SaveInstalledOrganOutputDevice(organ.uniqueOrganId, device.id))
            {
                RefreshHauptwerkAudioAssignmentsUI();
                ListView_SetItemState(g_hauptwerkOrganListHwnd, selectedOrgan, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                SyncHauptwerkAudioComboToSelection();
            }
            else
            {
                MessageBoxW(hWnd, L"Failed to save the organ audio assignment.", L"Hauptwerk", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kBiduleBrowseButtonId)
        {
            std::wstring bidulePath;
            if (PickBiduleExecutable(hWnd, bidulePath))
            {
                SaveBidulePath(bidulePath);
                RefreshBiduleSettingsUI();
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kBiduleAutoDetectButtonId)
        {
            std::wstring bidulePath = DetectBiduleExePath();
            if (bidulePath.empty())
            {
                MessageBoxW(hWnd, L"Bidule was not found automatically. Use Browse to select Bidule.exe.", L"Bidule", MB_OK | MB_ICONINFORMATION);
            }
            RefreshBiduleSettingsUI();
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kBiduleCloseOnUnloadCheckId)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
                hCheck = g_biduleCloseOnUnloadCheckHwnd;
            bool enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveBiduleCloseOnUnload(enabled);
            return 0;
        }
        break;
    }
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED)
        {
            HandleMidiDeviceChange(hWnd);
        }
        return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<LPNMHDR>(lParam)->hwndFrom == g_hauptwerkOrganListHwnd
            && reinterpret_cast<LPNMHDR>(lParam)->code == LVN_ITEMCHANGED)
        {
            SyncHauptwerkAudioComboToSelection();
            return 0;
        }
        if (reinterpret_cast<LPNMHDR>(lParam)->code == TCN_SELCHANGE
            && reinterpret_cast<LPNMHDR>(lParam)->hwndFrom == g_settingsTabHwnd)
        {
            int sel = TabCtrl_GetCurSel(g_settingsTabHwnd);
            if (g_settingsMidiPageHwnd)
            {
                ShowWindow(g_settingsMidiPageHwnd, sel == 0 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsInfoPageHwnd)
            {
                ShowWindow(g_settingsInfoPageHwnd, sel == 1 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsOrganInfoPageHwnd)
            {
                ShowWindow(g_settingsOrganInfoPageHwnd, sel == 2 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsHauptwerkPageHwnd)
            {
                ShowWindow(g_settingsHauptwerkPageHwnd, sel == 3 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsBidulePageHwnd)
            {
                ShowWindow(g_settingsBidulePageHwnd, sel == 4 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsStreamDeckPageHwnd)
            {
                ShowWindow(g_settingsStreamDeckPageHwnd, sel == 5 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsAboutPageHwnd)
            {
                ShowWindow(g_settingsAboutPageHwnd, sel == 6 ? SW_SHOW : SW_HIDE);
            }
            if (sel == 2)
            {
                UpdateOrganInfoGroupTitle();
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kLedTimerId)
        {
            if (g_feLedHwnd)
            {
                InvalidateRect(g_feLedHwnd, nullptr, FALSE);
            }
            UpdateOrganInfoGroupTitle();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, kLedTimerId);
        g_feLedHwnd = nullptr;
        g_activeSensingOutputComboHwnd = nullptr;
        g_settingsTabHwnd = nullptr;
        g_settingsMidiPageHwnd = nullptr;
        g_settingsInfoPageHwnd = nullptr;
        g_settingsOrganInfoPageHwnd = nullptr;
        g_settingsHauptwerkPageHwnd = nullptr;
        g_settingsBidulePageHwnd = nullptr;
        g_settingsOrganInfoGroupHwnd = nullptr;
        g_settingsAboutPageHwnd = nullptr;
        g_settingsStreamDeckPageHwnd = nullptr;
        g_hauptwerkOrganListHwnd = nullptr;
        g_hauptwerkAudioDeviceComboHwnd = nullptr;
        g_bidulePathEditHwnd = nullptr;
        g_biduleCloseOnUnloadCheckHwnd = nullptr;
        g_settingsHwnd = nullptr;
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

bool CreateTrayIcon(HINSTANCE hInstance, HWND hWnd)
{
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"AHLBORN 250 SL (OFF)");

    if (Shell_NotifyIconW(NIM_ADD, &g_nid) == FALSE)
    {
        return false;
    }
    // Prefer per-user icons in %APPDATA%\AhlbornBridge\Icons\A_Disabled.png when available.
    wchar_t appdataBuf[MAX_PATH] = {};
    std::wstring iconPath;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdataBuf)))
    {
        iconPath = std::wstring(appdataBuf) + L"\\AhlbornBridge\\Icons\\A_Disabled.png";
    }

    if (!iconPath.empty())
    {
        UpdateTrayIconFromFile(iconPath.c_str());
    }
    else
    {
        // Fallback to legacy path if AppData not available.
        UpdateTrayIconFromFile(L"C:\\App\\icons\\A_Disabled.png");
    }
    return true;
}

void RemoveTrayIcon(HWND hWnd)
{
    if (g_nid.hWnd != nullptr)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_nid.hWnd = nullptr;
    }

    if (g_trayIcon)
    {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
}

bool UpdateTrayIconFromFile(const wchar_t* iconPath)
{
    if (!g_nid.hWnd || !iconPath)
    {
        return false;
    }

    if (!EnsureGdiplusStarted())
    {
        return false;
    }

    Gdiplus::Bitmap bitmap(iconPath);
    if (bitmap.GetLastStatus() != Gdiplus::Ok)
    {
        return false;
    }

    HICON hIcon = nullptr;
    if (bitmap.GetHICON(&hIcon) != Gdiplus::Ok || !hIcon)
    {
        return false;
    }

    if (g_trayIcon)
    {
        DestroyIcon(g_trayIcon);
    }

    g_trayIcon = hIcon;
    g_nid.hIcon = g_trayIcon;
    g_nid.uFlags |= NIF_ICON;
    return Shell_NotifyIconW(NIM_MODIFY, &g_nid) != FALSE;
}

void CloseSettingsWindowIfAutoClose()
{
    if (!g_closeSettingsOnDisconnect.load())
    {
        return;
    }

    HWND settingsHwnd = g_settingsHwnd;
    if (settingsHwnd && IsWindow(settingsHwnd))
    {
        PostMessageW(settingsHwnd, WM_CLOSE, 0, 0);
    }
}

bool UpdateTrayIconTooltip(const wchar_t* tooltip)
{
    if (!g_nid.hWnd || !tooltip)
    {
        return false;
    }

    lstrcpyW(g_nid.szTip, tooltip);
    g_nid.uFlags |= NIF_TIP;
    return Shell_NotifyIconW(NIM_MODIFY, &g_nid) != FALSE;
}

void NotifyOrganInfoTitleChanged()
{
    if (!g_settingsHwnd || !IsWindow(g_settingsHwnd))
    {
        return;
    }

    UpdateOrganInfoGroupTitle();
}

bool EnsureBiduleVisibleViaTrayToggle()
{
    if (IsBiduleWindowVisible())
        return true;

    HWND trayHwnd = g_nid.hWnd;
    if (!trayHwnd || !IsWindow(trayHwnd))
        return false;

    SendMessageW(trayHwnd, WM_COMMAND, ID_TRAY_TOGGLE_BIDULE, 0);
    return IsBiduleWindowVisible();
}

void ShowTrayMenu(HWND hWnd, POINT pt)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    bool showConsole = true;
    LoadShowDebugConsole(showConsole);

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MIDI_ASSIGN, L"Assign MIDI devices\u2026");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (g_activeSensingEnabled.load() ? MF_CHECKED : MF_UNCHECKED),
        ID_TRAY_TOGGLE_ACTIVE_SENSING, L"Active Sensing sender");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (IsProcessRunningByName(L"bidule.exe") || IsProcessRunningByName(L"Bidule.exe"))
    {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_BIDULE,
            IsBiduleWindowVisible() ? L"Bidule: Hide" : L"Bidule: Show");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_BIDULE_OSC_TEST, L"Bidule: OSC Test /open");
    }
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_UPDATE, L"Check for Updates");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_CONSOLE,
        showConsole ? L"Hide debug console" : L"Show debug console");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y, 0, hWnd, nullptr);

    DestroyMenu(hMenu);
}

// Forward declaration for helper implemented in Midi.cpp so we can
// close Hauptwerk cleanly when the tray application exits.
void CloseHauptwerkProcess();

// Unified window procedure for the hidden tray window. It handles tray
// icon clicks, settings/exit commands and graceful shutdown of
// MIDI/Hauptwerk resources.
LRESULT CALLBACK TrayIconWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_TRAYICON:
		// Left-click (single or double) on tray icon: always attempt to
		// launch (or bring to front) Hauptwerk using the same helper used
		// by the watchdog. Start the tray flashing animation before
		// launching so we visually indicate startup while Hauptwerk is
		// initialising, even if the Ahlborn console is disconnected.
		if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
		{
			TriggerTrayIconStartupFlash();
			bool started = LaunchHauptwerkAndDismissWelcome();
			if (!started && g_hauptwerkMainWindow == nullptr)
			{
				// Launch failed and no existing Hauptwerk window exists.
					// Stop the flash immediately and show the error icon.
					StopTrayIconFlashing();
					wchar_t appdata[MAX_PATH] = {};
					if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata)))
					{
						std::wstring errIcon = std::wstring(appdata) + L"\\AhlbornBridge\\Icons\\A_Error.png";
						UpdateTrayIconFromFile(errIcon.c_str());
					}
					UpdateTrayIconTooltip(L"Hauptwerk launch failed");
				g_trayIconImageStatus = TrayIconImageStatus::Error;
			}
			return 0;
		}

		// Right-click or context menu opens the tray menu.
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
		{
			POINT pt;
			GetCursorPos(&pt);
			ShowTrayMenu(hWnd, pt);
		}
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case ID_TRAY_SETTINGS:
			ShowSettingsWindow((HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), hWnd);
			return 0;
		case ID_TRAY_MIDI_ASSIGN:
			ShowMidiAssignmentWindow((HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), hWnd);
			return 0;
		case ID_TRAY_UPDATE:
			CheckForUpdateInteractive(hWnd);
			return 0;
		case ID_TRAY_TOGGLE_CONSOLE:
		{
			bool showConsole = true;
			LoadShowDebugConsole(showConsole);
			showConsole = !showConsole;
			HWND hConsoleWnd = GetConsoleWindow();
			if (hConsoleWnd)
			{
				ShowWindow(hConsoleWnd, showConsole ? SW_SHOW : SW_HIDE);
			}
			SaveShowDebugConsole(showConsole);
			return 0;
		}
		case ID_TRAY_TOGGLE_ACTIVE_SENSING:
		{
			bool enabled = !g_activeSensingEnabled.load();
			g_activeSensingEnabled = enabled;
			SaveActiveSensingEnabled(enabled);
			return 0;
		}
		case ID_TRAY_TOGGLE_BIDULE:
		{
			ToggleBiduleWindowVisibility();
			return 0;
		}
		case ID_TRAY_BIDULE_OSC_TEST:
		{
			std::wstring testPath = L"C:\\Users\\paolo\\AppData\\Roaming\\AhlbornBridge\\BiduleProfiles\\OSC_Test.bidule";
			if (SendBiduleOscOpenProfile(testPath))
				MessageBoxW(hWnd, L"OSC test sent: /open OSC_Test.bidule", L"Bidule OSC", MB_OK | MB_ICONINFORMATION);
			else
				MessageBoxW(hWnd, L"OSC test failed. Check Bidule OSC server/port and file path.", L"Bidule OSC", MB_OK | MB_ICONWARNING);
			return 0;
		}
		case ID_TRAY_EXIT:
			// User requested application exit from tray menu.
			DestroyWindow(hWnd);
			return 0;
		default:
			break;
		}
		break;

	case WM_CLOSE:
		// Hide the helper window instead of destroying it when receiving
		// a generic close request.
		ShowWindow(hWnd, SW_HIDE);
		return 0;

	case WM_DEVICECHANGE:
		printf("WM_DEVICECHANGE received: wParam=0x%llX\n", static_cast<unsigned long long>(wParam));
		if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED)
		{
			// Do NOT Sleep here — it blocks the message pump and causes
			// Windows to drop subsequent WM_DEVICECHANGE messages.
			// Instead, use a one-shot timer so the refresh runs after
			// the MIDI subsystem has had time to update its device list.
			SetTimer(hWnd, 9999, 600, nullptr);
		}
		return 0;

	case WM_TIMER:
		if (wParam == 9999)
		{
			KillTimer(hWnd, 9999);
			printf("Device change timer fired — refreshing.\n");
			RefreshMidiDeviceStatus();
			RefreshSettingsFile();
			printf("Settings XML refreshed after device change.\n");
		}
		return 0;

	case WM_HOTKEY:
		if (wParam == 1) // kHotkeyIdClearConsole
		{
			HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
			if (hConsole != INVALID_HANDLE_VALUE)
			{
				CONSOLE_SCREEN_BUFFER_INFO csbi = {};
				GetConsoleScreenBufferInfo(hConsole, &csbi);
				DWORD totalCells = csbi.dwSize.X * csbi.dwSize.Y;
				DWORD written = 0;
				COORD origin = { 0, 0 };
				FillConsoleOutputCharacterW(hConsole, L' ', totalCells, origin, &written);
				FillConsoleOutputAttribute(hConsole, csbi.wAttributes, totalCells, origin, &written);
				SetConsoleCursorPosition(hConsole, origin);
			}
			printf("Console cleared (F12).\n");
		}
		return 0;

	case WM_DESTROY:
		UnregisterHotKey(hWnd, 1);
		// Gracefully stop background MIDI processing and close Hauptwerk
		// before exiting.
		running = false;
		StopOrganFolderWatcher();
		StopStreamDeckPipeServer();
		if (hMidiIn)
		{
			midiInStop(hMidiIn);
			midiInClose(hMidiIn);
			hMidiIn = nullptr;
		}
		CloseHauptwerkProcess();
		RemoveTrayIcon(hWnd);
		CleanupMidiLocks();
		PostQuitMessage(0);
		return 0;

	default:
		break;
	}

	return DefWindowProcW(hWnd, message, wParam, lParam);
}

void ShowSettingsWindow(HINSTANCE hInstance, HWND hOwner)
{
    if (g_settingsHwnd && IsWindow(g_settingsHwnd))
    {
        SetForegroundWindow(g_settingsHwnd);
        return;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kSettingsClassName;

    RegisterClassW(&wc);
    EnsureLedStripClass(hInstance);
    EnsureSettingsPageClass(hInstance);

    g_settingsHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kSettingsClassName,
        L"General Preferences",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 610,
        hOwner,
        nullptr,
        hInstance,
        nullptr);

    if (g_settingsHwnd)
    {
        ShowWindow(g_settingsHwnd, SW_SHOW);
        UpdateWindow(g_settingsHwnd);
    }
}

// ---------------------------------------------------------------------------
// Ahlborn Splash (generic fade window)
// ---------------------------------------------------------------------------
namespace {
    static std::atomic<bool> g_splashActive{ false };
    static std::atomic<HWND> g_textSplashHwnd{ nullptr };
    static std::atomic<int> g_textSplashProgress{ 0 };
    static std::wstring g_textSplashLine1;
    static std::wstring g_textSplashLine2;

    static std::wstring BuildSplashImagePath(const wchar_t* filename)
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path != nullptr)
        {
            std::wstring result(path);
            CoTaskMemFree(path);
            result += L"\\AhlbornBridge\\Icons\\";
            result += filename;
            return result;
        }
        return {};
    }

    struct SplashThreadParam
    {
        HINSTANCE hInstance;
        std::wstring imagePath;
        int totalMs;
        bool bottomRight;
        int holdMs;
    };

    struct TextSplashThreadParam
    {
        HINSTANCE hInstance;
        std::wstring line1;
        std::wstring line2;
    };

    static LRESULT CALLBACK SplashWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    static DWORD WINAPI SplashThread(LPVOID param)
    {
        auto* p = reinterpret_cast<SplashThreadParam*>(param);
        HINSTANCE hInst = p->hInstance;
        std::wstring imgPath = std::move(p->imagePath);
        int totalMs = p->totalMs;
        bool bottomRight = p->bottomRight;
        int holdMs = p->holdMs;
        delete p;

        if (imgPath.empty())
        {
            g_splashActive = false;
            return 0;
        }

        // Load image with GDI+ to get its real size.
        Gdiplus::GdiplusStartupInput gsi;
        ULONG_PTR gdipToken = 0;
        Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(imgPath.c_str());
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
        {
            delete bmp;
            Gdiplus::GdiplusShutdown(gdipToken);
            g_splashActive = false;
            return 0;
        }

        int imgW = static_cast<int>(bmp->GetWidth())  / 2;
        int imgH = static_cast<int>(bmp->GetHeight()) / 2;

        // Register a minimal window class for the splash.
        constexpr wchar_t kSplashClass[] = L"AhlbornSplashWnd";
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = SplashWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kSplashClass;
        RegisterClassExW(&wc);

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = 0;
        int y = 0;
        if (bottomRight)
        {
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            constexpr int kToastMargin = 20;
            x = workArea.right - imgW - kToastMargin;
            y = workArea.bottom - imgH - kToastMargin;
            if (x < workArea.left) x = workArea.left;
            if (y < workArea.top) y = workArea.top;
        }
        else
        {
            // Centre on primary monitor.
            x = (screenW - imgW) / 2;
            y = (screenH - imgH) / 2;
        }

        HWND hWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kSplashClass, L"", WS_POPUP,
            x, y, imgW, imgH,
            nullptr, nullptr, hInst, nullptr);

        if (!hWnd)
        {
            delete bmp;
            Gdiplus::GdiplusShutdown(gdipToken);
            g_splashActive = false;
            return 0;
        }

        // Build a memory DC with the image pre-painted (ARGB).
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem    = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = imgW;
        bmi.bmiHeader.biHeight      = -imgH;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP hOldBmp = reinterpret_cast<HBITMAP>(SelectObject(hdcMem, hBitmap));

        {
            Gdiplus::Graphics g(hdcMem);
            g.DrawImage(bmp, 0, 0, imgW, imgH);
        }
        delete bmp;

        POINT ptSrc = { 0, 0 };
        SIZE  szWnd = { imgW, imgH };
        POINT ptDst = { x, y };

        // Max opacity: 220/255 (~86%) so the image appears slightly transparent at peak.
        constexpr int   kMaxAlpha    = 220;
        constexpr int   kSteps       = 100;
        int kSleepMs = totalMs / (kSteps * 2);
        if (kSleepMs < 5)
            kSleepMs = 5;

        // Fade-in.
        for (int i = 0; i <= kSteps; ++i)
        {
            BYTE alpha = static_cast<BYTE>((i * kMaxAlpha) / kSteps);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
            UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
            ShowWindow(hWnd, SW_SHOW);
            Sleep(kSleepMs);
        }

        // Hold at peak visibility.
        if (holdMs > 0)
            Sleep(holdMs);

        // Fade-out.
        for (int i = kSteps; i >= 0; --i)
        {
            BYTE alpha = static_cast<BYTE>((i * kMaxAlpha) / kSteps);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
            UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
            ShowWindow(hWnd, SW_SHOW);
            Sleep(kSleepMs);
        }

        DestroyWindow(hWnd);

        // Drain any messages posted to this thread.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {}

        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        Gdiplus::GdiplusShutdown(gdipToken);

        g_splashActive = false;
        return 0;
    }

    static void ShowSplash(const wchar_t* filename, int totalMs = 4000, bool bottomRight = false, int holdMs = 0)
    {
        bool expected = false;
        if (!g_splashActive.compare_exchange_strong(expected, true))
            return;

        auto* p = new SplashThreadParam{ GetModuleHandleW(nullptr), BuildSplashImagePath(filename), totalMs, bottomRight, holdMs };
        HANDLE h = CreateThread(nullptr, 0, SplashThread, p, 0, nullptr);
        if (h) CloseHandle(h);
        else
        {
            delete p;
            g_splashActive = false;
        }
    }

    static void PaintTextSplash(HWND hWnd)
    {
        HDC hdc = GetDC(hWnd);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 28, 34));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));

        HFONT hTitleFont = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hBodyFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        HFONT hOld = reinterpret_cast<HFONT>(SelectObject(hdc, hTitleFont));
        RECT titleRc{ 24, 18, rc.right - 24, 54 };
        DrawTextW(hdc, g_textSplashLine1.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, hBodyFont);
        RECT bodyRc{ 24, 56, rc.right - 24, 92 };
        DrawTextW(hdc, g_textSplashLine2.c_str(), -1, &bodyRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT barOuter{ 24, 104, rc.right - 24, 124 };
        HBRUSH barBg = CreateSolidBrush(RGB(52, 58, 66));
        FillRect(hdc, &barOuter, barBg);
        DeleteObject(barBg);

        int progress = g_textSplashProgress.load();
        RECT barFill = barOuter;
        barFill.right = barOuter.left + ((barOuter.right - barOuter.left) * progress / 100);
        HBRUSH barFillBrush = CreateSolidBrush(RGB(0, 170, 255));
        FillRect(hdc, &barFill, barFillBrush);
        DeleteObject(barFillBrush);

        wchar_t pct[16] = {};
        wsprintfW(pct, L"%d%%", progress);
        SetTextColor(hdc, RGB(220, 220, 220));
        RECT pctRc{ barOuter.right - 52, 126, barOuter.right, 146 };
        DrawTextW(hdc, pct, -1, &pctRc, DT_RIGHT | DT_SINGLELINE);

        SelectObject(hdc, hOld);
        DeleteObject(hTitleFont);
        DeleteObject(hBodyFont);
        ReleaseDC(hWnd, hdc);
    }

    static DWORD WINAPI TextSplashThread(LPVOID param)
    {
        auto* p = reinterpret_cast<TextSplashThreadParam*>(param);
        HINSTANCE hInst = p->hInstance;
        g_textSplashLine1 = std::move(p->line1);
        g_textSplashLine2 = std::move(p->line2);
        g_textSplashProgress = 10;
        delete p;

        constexpr wchar_t kSplashClass[] = L"AhlbornTextSplashWnd";
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
        {
            switch (msg)
            {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps{};
                BeginPaint(hWnd, &ps);
                EndPaint(hWnd, &ps);
                PaintTextSplash(hWnd);
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(hWnd);
                return 0;
            case WM_DESTROY:
                g_textSplashHwnd = nullptr;
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            }
        };
        wc.hInstance = hInst;
        wc.hbrBackground = CreateSolidBrush(RGB(24, 28, 34));
        wc.lpszClassName = kSplashClass;
        RegisterClassExW(&wc);

        const int wndW = 520;
        const int wndH = 150;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - wndW) / 2;
        int y = (screenH - wndH) / 2;

        HWND hWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kSplashClass, L"", WS_POPUP,
            x, y, wndW, wndH,
            nullptr, nullptr, hInst, nullptr);
        if (!hWnd)
        {
            g_splashActive = false;
            return 0;
        }

        g_textSplashHwnd = hWnd;

        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hWnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_splashActive = false;
        return 0;
    }
} // namespace

void ShowAhlbornStartedSplash()
{
    ShowSplash(L"ahlborn_started.png");
}

void ShowAhlbornClosedSplash()
{
    ShowSplash(L"ahlborn_closed.png");
}

void ShowVstLinkModeSplash()
{
    ShowSplash(L"VstLinkMode1.png", 1200, true, 1400);
}

void ShowHauptwerkRestartSplash(const wchar_t* deviceName)
{
    bool expected = false;
    if (!g_splashActive.compare_exchange_strong(expected, true))
        return;

    std::wstring deviceLine = L"Audio device: ";
    deviceLine += (deviceName && *deviceName) ? deviceName : L"(unknown)";
    auto* p = new TextSplashThreadParam{ GetModuleHandleW(nullptr), L"Restarting Hauptwerk...", std::move(deviceLine) };
    HANDLE h = CreateThread(nullptr, 0, TextSplashThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else
    {
        delete p;
        g_splashActive = false;
    }
}

void UpdateHauptwerkRestartSplashProgress(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_textSplashProgress = percent;
    HWND hWnd = g_textSplashHwnd.load();
    if (hWnd && IsWindow(hWnd))
        InvalidateRect(hWnd, nullptr, TRUE);
}

void CloseHauptwerkRestartSplash()
{
    HWND hWnd = g_textSplashHwnd.load();
    if (hWnd && IsWindow(hWnd))
        PostMessageW(hWnd, WM_CLOSE, 0, 0);
    else
        g_splashActive = false;
}
