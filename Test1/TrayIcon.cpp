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
static HWND g_settingsOrganInfoGroupHwnd = nullptr;
static HWND g_settingsAboutPageHwnd = nullptr;
static HWND g_settingsStreamDeckPageHwnd = nullptr;
static HWND g_feLedHwnd = nullptr;
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
constexpr int kMainUpdateModeComboId = 403;
constexpr int kMainCheckNowButtonId = 404;

namespace
{
    void HandleMidiDeviceChange(HWND hWnd)
    {
        RefreshMidiDeviceStatus();
        RefreshSettingsFile();
        if (g_feLedHwnd)
        {
            InvalidateRect(g_feLedHwnd, nullptr, FALSE);
        }
    }

    void UpdateOrganInfoGroupTitle();

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
        tabItem.pszText = const_cast<wchar_t*>(L"Stream Deck");
        TabCtrl_InsertItem(g_settingsTabHwnd, 4, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"About");
        TabCtrl_InsertItem(g_settingsTabHwnd, 5, &tabItem);

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
                10, 8, 700, 180, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Hauptwerk preferences",
                WS_CHILD | WS_VISIBLE,
                24, 32, 220, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC",
                L"This tab is reserved for Hauptwerk-specific options and routing behavior.",
                WS_CHILD | WS_VISIBLE,
                24, 52, 620, 18, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                24, 78, 660, 2, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Status:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                30, 96, 120, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Ready for additional Hauptwerk settings.",
                WS_CHILD | WS_VISIBLE,
                164, 96, 420, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Tip:",
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                30, 126, 120, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Next step: add model, ports and profile mapping options here.",
                WS_CHILD | WS_VISIBLE,
                164, 126, 500, 20, g_settingsHauptwerkPageHwnd, nullptr, nullptr, nullptr);
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
            10, 10, 280, 50, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        g_feLedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            22, 30, 20, 20, g_settingsMidiPageHwnd, (HMENU)106, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"FE", WS_CHILD | WS_VISIBLE,
            48, 32, 120, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        SetTimer(hWnd, kLedTimerId, kLedTimerIntervalMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
    {
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
        break;
    }
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED)
        {
            HandleMidiDeviceChange(hWnd);
        }
        return 0;
    case WM_NOTIFY:
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
            if (g_settingsStreamDeckPageHwnd)
            {
                ShowWindow(g_settingsStreamDeckPageHwnd, sel == 4 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsAboutPageHwnd)
            {
                ShowWindow(g_settingsAboutPageHwnd, sel == 5 ? SW_SHOW : SW_HIDE);
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
        g_settingsTabHwnd = nullptr;
        g_settingsMidiPageHwnd = nullptr;
        g_settingsInfoPageHwnd = nullptr;
        g_settingsOrganInfoPageHwnd = nullptr;
        g_settingsHauptwerkPageHwnd = nullptr;
        g_settingsOrganInfoGroupHwnd = nullptr;
        g_settingsAboutPageHwnd = nullptr;
        g_settingsStreamDeckPageHwnd = nullptr;
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

void ShowTrayMenu(HWND hWnd, POINT pt)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    bool showConsole = true;
    LoadShowDebugConsole(showConsole);

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MIDI_ASSIGN, L"Assign MIDI devices\u2026");
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
