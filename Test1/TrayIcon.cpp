#include "TrayIcon.h"
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
static HWND g_settingsOrganInfoGroupHwnd = nullptr;
static HWND g_settingsAboutPageHwnd = nullptr;
static HWND g_settingsStreamDeckPageHwnd = nullptr;
static HWND g_inputLedStripHwnd = nullptr;
static HWND g_outputLedStripHwnd = nullptr;
static HWND g_feLedHwnd = nullptr;
static HWND g_inputStatusLedHwnd = nullptr;
static HWND g_inputStatus2LedHwnd = nullptr;
static HWND g_outputStatusLedHwnd = nullptr;
static HWND g_outputStatus2LedHwnd = nullptr;
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

namespace
{
    void HandleMidiDeviceChange(HWND hWnd);
    void UpdateOrganInfoGroupTitle();

    DWORD g_inputRefreshUntil = 0;

    bool g_inputDeviceError   = false;
    bool g_input2DeviceError  = false;
    bool g_outputDeviceError  = false;
    bool g_output2DeviceError = false;

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
            case WM_LBUTTONUP:
            {
                LONG_PTR controlId = GetWindowLongPtrW(hWnd, GWLP_ID);
                HWND parent = GetParent(hWnd);
                if (controlId == 107) // Input 01
                {
                    if (IsMidiInputDeviceOpen())
                    {
                        CloseMidiInputDeviceOnly();
                        g_inputDeviceError = false;
                    }
                    else
                    {
                        g_inputRefreshUntil = GetTickCount() + kInputRefreshDurationMs;
                        UINT deviceId = 0;
                        LoadSelectedDeviceId(deviceId);
                        bool ok = SwitchMidiInputDevice(deviceId);
                        g_inputDeviceError = !ok;
                    }
                    HandleMidiDeviceChange(parent);
                    {
                        bool nowOpen = IsMidiInputDeviceOpen();
                        SaveMidiInput1DeviceEnabled(nowOpen);
                        if (g_settingsMidiPageHwnd)
                            EnableWindow(GetDlgItem(g_settingsMidiPageHwnd, 101), nowOpen ? TRUE : FALSE);
                    }
                    return 0;
                }
                if (controlId == 110) // Input 02
                {
                    if (IsMidiInput2DeviceOpen())
                    {
                        CloseMidiInput2DeviceOnly();
                        g_input2DeviceError = false;
                    }
                    else
                    {
                        UINT deviceId = 0;
                        LoadSelectedInput2DeviceId(deviceId);
                        bool ok = SwitchMidiInput2Device(deviceId);
                        g_input2DeviceError = !ok;
                    }
                    HandleMidiDeviceChange(parent);
                    {
                        bool nowOpen = IsMidiInput2DeviceOpen();
                        SaveMidiInput2DeviceEnabled(nowOpen);
                        if (g_settingsMidiPageHwnd)
                            EnableWindow(GetDlgItem(g_settingsMidiPageHwnd, 109), nowOpen ? TRUE : FALSE);
                    }
                    return 0;
                }
                if (controlId == 108) // Output 01
                {
                    if (IsMidiOutputDeviceOpen())
                    {
                        CloseMidiOutputDeviceOnly();
                        g_outputDeviceError = false;
                    }
                    else
                    {
                        UINT deviceId = 0;
                        LoadSelectedOutputDeviceId(deviceId);
                        bool ok = SwitchMidiOutputDevice(deviceId);
                        g_outputDeviceError = !ok;
                    }
                    HandleMidiDeviceChange(parent);
                    {
                        bool nowOpen = IsMidiOutputDeviceOpen();
                        SaveMidiOutput1DeviceEnabled(nowOpen);
                        if (g_settingsMidiPageHwnd)
                            EnableWindow(GetDlgItem(g_settingsMidiPageHwnd, 102), nowOpen ? TRUE : FALSE);
                    }
                    return 0;
                }
                if (controlId == 112) // Output 02
                {
                    if (IsMidiOutput2DeviceOpen())
                    {
                        CloseMidiOutput2DeviceOnly();
                        g_output2DeviceError = false;
                    }
                    else
                    {
                        UINT deviceId = 0;
                        LoadSelectedOutput2DeviceId(deviceId);
                        bool ok = SwitchMidiOutput2Device(deviceId);
                        g_output2DeviceError = !ok;
                    }
                    HandleMidiDeviceChange(parent);
                    {
                        bool nowOpen = IsMidiOutput2DeviceOpen();
                        SaveMidiOutput2DeviceEnabled(nowOpen);
                        if (g_settingsMidiPageHwnd)
                            EnableWindow(GetDlgItem(g_settingsMidiPageHwnd, 111), nowOpen ? TRUE : FALSE);
                    }
                    return 0;
                }
                break;
            }
            case WM_SETCURSOR:
            {
                LONG_PTR controlId = GetWindowLongPtrW(hWnd, GWLP_ID);
                if (controlId == 107 || controlId == 108 || controlId == 110 || controlId == 112)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            }
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
                bool isOutput = controlId == 105;
                bool isFe = controlId == 106;
                bool isInputStatus = controlId == 107;
                bool isOutputStatus = controlId == 108;
                bool isInput2Status = controlId == 110;
                bool isOutput2Status = controlId == 112;

                if (isInputStatus || isOutputStatus || isInput2Status || isOutput2Status)
                {
                    RECT ledRect{ margin, y, margin + ledSize, y + ledSize };
                    bool isOpen = isInputStatus ? IsMidiInputDeviceOpen()
                                : isInput2Status ? IsMidiInput2DeviceOpen()
                                : isOutput2Status ? IsMidiOutput2DeviceOpen()
                                : IsMidiOutputDeviceOpen();
                    bool hasError = isInputStatus   ? g_inputDeviceError
                                  : isInput2Status  ? g_input2DeviceError
                                  : isOutput2Status ? g_output2DeviceError
                                  : g_outputDeviceError;
                    COLORREF color = isOpen    ? RGB(0, 200, 0)   // verde  = aperto
                                   : hasError  ? RGB(220, 0, 0)   // rosso  = errore
                                   : RGB(80, 80, 80);             // grigio = chiuso
                    if (isInputStatus && g_inputRefreshUntil != 0)
                    {
                        DWORD tick = GetTickCount();
                        if (tick >= g_inputRefreshUntil)
                        {
                            g_inputRefreshUntil = 0;
                        }
                        else
                        {
                            bool showAlt = (tick / kInputRefreshBlinkIntervalMs) % 2 == 0;
                            COLORREF altColor = isOpen ? RGB(255, 200, 0) : RGB(255, 120, 0);
                            color = showAlt ? altColor : color;
                        }
                    }
                    HBRUSH brush = CreateSolidBrush(color);
                    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(memDc, brush));
                    Ellipse(memDc, ledRect.left, ledRect.top, ledRect.right, ledRect.bottom);
                    SelectObject(memDc, oldBrush);
                    DeleteObject(brush);
                }
                else if (isFe)
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
                else
                {

                    for (int ch = 0; ch < kChannels; ++ch)
                    {
                        int x = margin + ch * (ledSize + spacing);
                        RECT ledRect{ x, y, x + ledSize, y + ledSize };
                        bool active = isOutput ? IsOutputChannelActive(ch) : IsChannelActive(ch);
                        COLORREF color = active ? RGB(0, 200, 0) : RGB(0, 0, 0);
                        HBRUSH brush = CreateSolidBrush(color);
                        HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(memDc, brush));
                        Ellipse(memDc, ledRect.left, ledRect.top, ledRect.right, ledRect.bottom);
                        SelectObject(memDc, oldBrush);
                        DeleteObject(brush);
                    }
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

    void HandleMidiDeviceChange(HWND hWnd)
    {
        RefreshMidiDeviceStatus();
        RefreshMidiComboSelections(hWnd);
        RefreshSettingsFile();

        if (g_inputStatusLedHwnd)
        {
            InvalidateRect(g_inputStatusLedHwnd, nullptr, FALSE);
        }
        if (g_inputStatus2LedHwnd)
        {
            InvalidateRect(g_inputStatus2LedHwnd, nullptr, FALSE);
        }
        if (g_outputStatusLedHwnd)
        {
            InvalidateRect(g_outputStatusLedHwnd, nullptr, FALSE);
        }
        if (g_outputStatus2LedHwnd)
        {
            InvalidateRect(g_outputStatus2LedHwnd, nullptr, FALSE);
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
        tabItem.pszText = const_cast<wchar_t*>(L"Options");
        TabCtrl_InsertItem(g_settingsTabHwnd, 1, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Organ Info...");
        TabCtrl_InsertItem(g_settingsTabHwnd, 2, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"Stream Deck");
        TabCtrl_InsertItem(g_settingsTabHwnd, 3, &tabItem);
        tabItem.pszText = const_cast<wchar_t*>(L"About");
        TabCtrl_InsertItem(g_settingsTabHwnd, 4, &tabItem);

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

        CreateWindowW(L"BUTTON", L"Settings window", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 8, 300, 60, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

        HWND hAutoCloseCheck = CreateWindowW(L"BUTTON",
            L"AUTO close when ALL is disconnected.",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            22, 30, 280, 20, g_settingsInfoPageHwnd, (HMENU)kAutoCloseCheckId, nullptr, nullptr);

        bool closeOnDisconnect = false;
        if (LoadCloseSettingsOnDisconnect(closeOnDisconnect) && closeOnDisconnect)
        {
            SendMessageW(hAutoCloseCheck, BM_SETCHECK, BST_CHECKED, 0);
        }
        g_closeSettingsOnDisconnect = closeOnDisconnect;

        CreateWindowW(L"BUTTON", L"Debug", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 76, 300, 60, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

        HWND hShowConsoleCheck = CreateWindowW(L"BUTTON",
            L"Show debug console",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            22, 98, 280, 20, g_settingsInfoPageHwnd, (HMENU)kShowConsoleCheckId, nullptr, nullptr);

        bool showConsole = true;
        if (LoadShowDebugConsole(showConsole) && showConsole)
        {
            SendMessageW(hShowConsoleCheck, BM_SETCHECK, BST_CHECKED, 0);
        }
        else
        {
            SendMessageW(hShowConsoleCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        }

        CreateWindowW(L"BUTTON", L"Updates", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 144, 300, 50, g_settingsInfoPageHwnd, nullptr, nullptr, nullptr);

        HWND hCheckForUpdateOnStartCheck = CreateWindowW(L"BUTTON",
            L"Check for updates on start",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            22, 166, 280, 20, g_settingsInfoPageHwnd, (HMENU)kCheckForUpdateOnStartCheckId, nullptr, nullptr);

        bool checkForUpdateOnStart = true;
        LoadCheckForUpdateOnStart(checkForUpdateOnStart);
        SendMessageW(hCheckForUpdateOnStartCheck, BM_SETCHECK, checkForUpdateOnStart ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowW(L"BUTTON", L"MIDI Input devices...", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 8, 280, 120, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Device 01", WS_CHILD | WS_VISIBLE,
            22, 30, 80, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        HWND hCombo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            22, 48, 232, 200, g_settingsMidiPageHwnd, (HMENU)101, nullptr, nullptr);

        g_inputStatusLedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            260, 50, 16, 16, g_settingsMidiPageHwnd, (HMENU)107, nullptr, nullptr);

        PopulateMidiInputs(hCombo);
        UINT savedDeviceId = 0;
        int count = static_cast<int>(SendMessageW(hCombo, CB_GETCOUNT, 0, 0));
        if (count > 0 && LoadSelectedDeviceId(savedDeviceId) && savedDeviceId < static_cast<UINT>(count))
        {
            SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(savedDeviceId), 0);
        }
        {
            bool input1Enabled = true;
            LoadMidiInput1DeviceEnabled(input1Enabled);
            EnableWindow(hCombo, input1Enabled ? TRUE : FALSE);
        }

        CreateWindowW(L"STATIC", L"Device 02", WS_CHILD | WS_VISIBLE,
            22, 80, 80, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        HWND hCombo2 = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            22, 98, 232, 200, g_settingsMidiPageHwnd, (HMENU)109, nullptr, nullptr);

        g_inputStatus2LedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            260, 100, 16, 16, g_settingsMidiPageHwnd, (HMENU)110, nullptr, nullptr);

        PopulateMidiInputs(hCombo2);
        UINT savedDeviceId2 = 0;
        int count2 = static_cast<int>(SendMessageW(hCombo2, CB_GETCOUNT, 0, 0));
        if (count2 > 0 && LoadSelectedInput2DeviceId(savedDeviceId2) && savedDeviceId2 < static_cast<UINT>(count2))
        {
            SendMessageW(hCombo2, CB_SETCURSEL, static_cast<WPARAM>(savedDeviceId2), 0);
        }
        {
            bool input2Enabled = true;
            LoadMidiInput2DeviceEnabled(input2Enabled);
            EnableWindow(hCombo2, input2Enabled ? TRUE : FALSE);
        }

        CreateWindowW(L"BUTTON", L"MIDI Output devices...", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 141, 280, 120, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Device 01", WS_CHILD | WS_VISIBLE,
            22, 163, 80, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        HWND hOutputCombo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            22, 181, 232, 200, g_settingsMidiPageHwnd, (HMENU)102, nullptr, nullptr);

        g_outputStatusLedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            260, 183, 16, 16, g_settingsMidiPageHwnd, (HMENU)108, nullptr, nullptr);

        PopulateMidiOutputs(hOutputCombo);
        UINT savedOutputDeviceId = 0;
        int outputCount = static_cast<int>(SendMessageW(hOutputCombo, CB_GETCOUNT, 0, 0));
        if (outputCount > 0 && LoadSelectedOutputDeviceId(savedOutputDeviceId)
            && savedOutputDeviceId < static_cast<UINT>(outputCount))
        {
            SendMessageW(hOutputCombo, CB_SETCURSEL, static_cast<WPARAM>(savedOutputDeviceId), 0);
        }
        {
            bool output1Enabled = true;
            LoadMidiOutput1DeviceEnabled(output1Enabled);
            EnableWindow(hOutputCombo, output1Enabled ? TRUE : FALSE);
        }

        CreateWindowW(L"STATIC", L"Device 02", WS_CHILD | WS_VISIBLE,
            22, 213, 80, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        HWND hOutput2Combo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            22, 231, 232, 200, g_settingsMidiPageHwnd, (HMENU)111, nullptr, nullptr);

        g_outputStatus2LedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            260, 233, 16, 16, g_settingsMidiPageHwnd, (HMENU)112, nullptr, nullptr);

        PopulateMidiOutputs(hOutput2Combo);
        UINT savedOutput2DeviceId = 0;
        int output2Count = static_cast<int>(SendMessageW(hOutput2Combo, CB_GETCOUNT, 0, 0));
        if (output2Count > 0 && LoadSelectedOutput2DeviceId(savedOutput2DeviceId)
            && savedOutput2DeviceId < static_cast<UINT>(output2Count))
        {
            SendMessageW(hOutput2Combo, CB_SETCURSEL, static_cast<WPARAM>(savedOutput2DeviceId), 0);
        }
        {
            bool output2Enabled = true;
            LoadMidiOutput2DeviceEnabled(output2Enabled);
            EnableWindow(hOutput2Combo, output2Enabled ? TRUE : FALSE);
        }

        CreateWindowW(L"BUTTON", L"Router", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 269, 280, 50, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        HWND hRouterCheck = CreateWindowW(L"BUTTON", L"Enable MIDI routing", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            22, 289, 240, 20, g_settingsMidiPageHwnd, (HMENU)103, nullptr, nullptr);

        bool routerEnabled = false;
        if (LoadMidiRouterEnabled(routerEnabled) && routerEnabled)
        {
            SendMessageW(hRouterCheck, BM_SETCHECK, BST_CHECKED, 0);
        }
        g_midiRouterEnabled = routerEnabled;

        CreateWindowW(L"BUTTON", L"Ahlborn 250 SL (PORT)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 327, 280, 60, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        g_inputLedStripHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            22, 349, 256, 20, g_settingsMidiPageHwnd, (HMENU)104, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Hauptwerk midi input (PORT)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 393, 280, 60, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        g_outputLedStripHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            22, 415, 256, 20, g_settingsMidiPageHwnd, (HMENU)105, nullptr, nullptr, reinterpret_cast<LPVOID>(1));

        CreateWindowW(L"BUTTON", L"FE (Active sensing)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 459, 280, 50, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        g_feLedHwnd = CreateWindowW(kLedStripClassName, nullptr, WS_CHILD | WS_VISIBLE,
            22, 479, 20, 20, g_settingsMidiPageHwnd, (HMENU)106, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"FE", WS_CHILD | WS_VISIBLE,
            48, 481, 120, 16, g_settingsMidiPageHwnd, nullptr, nullptr, nullptr);

        SetTimer(hWnd, kLedTimerId, kLedTimerIntervalMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
    {
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == 101)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, 101);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            if (selectedIndex >= 0)
            {
                UINT numDevs = midiInGetNumDevs();
                if (static_cast<UINT>(selectedIndex) < numDevs)
                {
                    UINT deviceId = static_cast<UINT>(selectedIndex);
                    SaveSelectedDeviceId(deviceId);
                    SwitchMidiInputDevice(deviceId);
                    if (g_inputStatusLedHwnd)
                    {
                        InvalidateRect(g_inputStatusLedHwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == 109)
        {
            HWND hCombo2 = reinterpret_cast<HWND>(lParam);
            if (!hCombo2)
            {
                hCombo2 = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, 109);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo2, CB_GETCURSEL, 0, 0));
            if (selectedIndex >= 0)
            {
                UINT numDevs = midiInGetNumDevs();
                if (static_cast<UINT>(selectedIndex) < numDevs)
                {
                    UINT deviceId = static_cast<UINT>(selectedIndex);
                    SaveSelectedInput2DeviceId(deviceId);
                    SwitchMidiInput2Device(deviceId);
                    if (g_inputStatus2LedHwnd)
                    {
                        InvalidateRect(g_inputStatus2LedHwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == 102)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, 102);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            if (selectedIndex >= 0)
            {
                UINT numDevs = midiOutGetNumDevs();
                if (static_cast<UINT>(selectedIndex) < numDevs)
                {
                    UINT deviceId = static_cast<UINT>(selectedIndex);
                    SaveSelectedOutputDeviceId(deviceId);
                    SwitchMidiOutputDevice(deviceId);
                    if (g_outputStatusLedHwnd)
                    {
                        InvalidateRect(g_outputStatusLedHwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == 111)
        {
            HWND hCombo = reinterpret_cast<HWND>(lParam);
            if (!hCombo)
            {
                hCombo = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, 111);
            }

            int selectedIndex = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
            if (selectedIndex >= 0)
            {
                UINT numDevs = midiOutGetNumDevs();
                if (static_cast<UINT>(selectedIndex) < numDevs)
                {
                    UINT deviceId = static_cast<UINT>(selectedIndex);
                    SaveSelectedOutput2DeviceId(deviceId);
                    SwitchMidiOutput2Device(deviceId);
                    if (g_outputStatus2LedHwnd)
                    {
                        InvalidateRect(g_outputStatus2LedHwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 103)
        {
            HWND hCheck = reinterpret_cast<HWND>(lParam);
            if (!hCheck)
            {
                hCheck = GetDlgItem(g_settingsMidiPageHwnd ? g_settingsMidiPageHwnd : hWnd, 103);
            }

            bool enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_midiRouterEnabled = enabled;
            if (!enabled)
            {
                ClearOutputNotes();
            }
            SaveMidiRouterEnabled(enabled);
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
            if (g_settingsStreamDeckPageHwnd)
            {
                ShowWindow(g_settingsStreamDeckPageHwnd, sel == 3 ? SW_SHOW : SW_HIDE);
            }
            if (g_settingsAboutPageHwnd)
            {
                ShowWindow(g_settingsAboutPageHwnd, sel == 4 ? SW_SHOW : SW_HIDE);
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
            if (g_inputLedStripHwnd)
            {
                InvalidateRect(g_inputLedStripHwnd, nullptr, FALSE);
            }
            if (g_outputLedStripHwnd)
            {
                InvalidateRect(g_outputLedStripHwnd, nullptr, FALSE);
            }
            if (g_feLedHwnd)
            {
                InvalidateRect(g_feLedHwnd, nullptr, FALSE);
            }
            if (g_inputStatusLedHwnd)
            {
                InvalidateRect(g_inputStatusLedHwnd, nullptr, FALSE);
            }
            if (g_inputStatus2LedHwnd)
            {
                InvalidateRect(g_inputStatus2LedHwnd, nullptr, FALSE);
            }
            if (g_outputStatusLedHwnd)
            {
                InvalidateRect(g_outputStatusLedHwnd, nullptr, FALSE);
            }
            if (g_outputStatus2LedHwnd)
            {
                InvalidateRect(g_outputStatus2LedHwnd, nullptr, FALSE);
            }
            UpdateOrganInfoGroupTitle();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, kLedTimerId);
        g_inputLedStripHwnd = nullptr;
        g_outputLedStripHwnd = nullptr;
        g_feLedHwnd = nullptr;
        g_inputStatusLedHwnd = nullptr;
        g_inputStatus2LedHwnd = nullptr;
        g_outputStatusLedHwnd = nullptr;
        g_outputStatus2LedHwnd = nullptr;
        g_settingsTabHwnd = nullptr;
        g_settingsMidiPageHwnd = nullptr;
        g_settingsInfoPageHwnd = nullptr;
        g_settingsOrganInfoPageHwnd = nullptr;
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

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_UPDATE, L"Check for Updates");
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
		case ID_TRAY_UPDATE:
			CheckForUpdateInteractive(hWnd);
			return 0;
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
        L"Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 601,
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
