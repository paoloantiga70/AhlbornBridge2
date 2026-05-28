// AhlbornBridgeSD.exe  —  Native Stream Deck plugin for AhlbornBridge
//
// Launched by Stream Deck with:
//   --port <port> --pluginUUID <uuid> --registerEvent <event> --info <json>
//
// Connects:
//   1. WebSocket to Stream Deck (ws://127.0.0.1:<port>)
//   2. Named pipe to AhlbornBridge (\\.\pipe\AhlbornBridge)
//
// Proxies organ list, state changes, button presses between the two.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <share.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

static const int PI_HTTP_PORT = 28781;

// ─── File logger

static FILE* g_logFile = nullptr;

static void LogInit()
{
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    std::wstring logPath = std::wstring(tmpPath) + L"AhlbornBridgeSD.log";
    g_logFile = _wfsopen(logPath.c_str(), L"a", _SH_DENYNO);
    if (g_logFile) setvbuf(g_logFile, NULL, _IOLBF, 4096);
}

static void Log(const char* fmt, ...)
{
    if (!g_logFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d  ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fprintf(g_logFile, "\n");
}

// ─── Globals ────────────────────────────────────────────────────────────

static int         g_port = 0;
static std::string g_pluginUUID;
static std::string g_registerEvent;

static HINTERNET   g_hSession = nullptr;
static HINTERNET   g_hConnect = nullptr;
static HINTERNET   g_hRequest = nullptr;
static HINTERNET   g_hWebSocket = nullptr;
static std::mutex  g_wsMutex;

static HANDLE      g_pipe = INVALID_HANDLE_VALUE;
static std::mutex  g_pipeMutex;
static std::atomic<bool> g_pipeConnected{ false };

static std::atomic<bool> g_running{ true };

// Track visible buttons for both actions
struct ButtonInfo
{
    std::string context;
    std::string action;
    int organIndex = 0;
    std::string organName;
    int switchIndex = 0;
    bool switchIsOn = false;
    // Cache to avoid redundant WS messages
    std::string lastSvg;
    int lastState = -1;
};
static std::mutex g_buttonsMutex;
static std::map<std::string, ButtonInfo> g_visibleButtons; // context -> info

// Current organ state
static std::mutex g_stateMutex;
static int g_loadedOrganIndex = 0;
static bool g_loadedOrganUsesVstLink = false;
static bool g_consoleConnected = false;
static bool g_hauptwerkRunning = false;
static bool g_biduleSplashActive = false;
static std::atomic<bool> g_activeSensingEnabledState{ true };

// Organ and switch lists from AhlbornBridge
struct OrganEntry
{
    int index;
    std::string name;
};
struct SwitchEntry
{
    int index = 0;
    std::string name;
    int channel = 1;
    int cc = 0;
    int valueOn = 127;
    int valueOff = 0;
    bool momentary = false;
};
static std::mutex g_organsMutex;
static std::vector<OrganEntry> g_organs;
static std::mutex g_switchesMutex;
static std::vector<SwitchEntry> g_switches;

// Remember switch ON/OFF state mirrored from bridge authoritative messages.
static std::mutex g_switchStateMutex;
static std::map<int, bool> g_currentSwitchStateByIndex;
static std::atomic<bool> g_freezeSwitchStateDuringUnload{ false };

static void PipeSend(const std::string& json);
static void UpdateAllButtons();

// Background images (base64-encoded PNG data, loaded at startup)
static std::string g_bgImageOff;
static std::string g_bgImageOn;

// Delayed refresh after willAppear burst (profile load)
static std::atomic<ULONGLONG> g_lastWillAppearTick{0};
static std::atomic<bool> g_refreshPending{false};

// Organ loading progress (real from host when available, simulated fallback)
static std::mutex g_loadingMutex;
static std::map<int, int> g_loadingProgressByOrgan;
static std::map<int, ULONGLONG> g_requestByOrgan;
static std::atomic<bool> g_loadingWorkerRunning{ false };
static std::atomic<bool> g_requestWorkerRunning{ false };
static std::atomic<int> g_realLoadingIndex{ 0 };
static std::atomic<int> g_realLoadingProgress{ -1 };
static std::atomic<bool> g_realLoadingVisible{ false };
static std::atomic<ULONGLONG> g_realLoadingLastTick{ 0 };
static bool IsOrganLoading(int organIndex, int* progressOut = nullptr);
static bool IsOrganRequesting(int organIndex);
static void StartOrganLoadingProgress(int organIndex);
static void CompleteOrganLoadingProgress(int organIndex);
static void StartOrganRequest(int organIndex);
static void StopOrganRequest(int organIndex);
static void ClearOrganRequests();

// Hold detection for loaded-organ button action (short press unload, long press toggle Bidule window)
struct KeyHoldInfo
{
    ULONGLONG downTick = 0;
    int organIndex = 0;
    bool longTriggered = false;
};
static std::mutex g_keyDownMutex;
static std::map<std::string, KeyHoldInfo> g_keyDownTickByContext;

// ─── Simple JSON helpers ────────────────────────────────────────────────

static std::string JsonExtractString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static int JsonExtractInt(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    return atoi(json.c_str() + pos);
}

static bool JsonExtractBool(const std::string& json, const std::string& key, bool defaultValue = false)
{
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
    {
        search = "\"" + key + "\"";
        pos = json.find(search);
        if (pos == std::string::npos)
            return defaultValue;
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        if (pos >= json.size() || json[pos] != ':')
            return defaultValue;
        ++pos;
    }
    else
    {
        pos += search.size();
    }

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size())
        return defaultValue;

    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    if (json[pos] == '1') return true;
    if (json[pos] == '0') return false;

    return defaultValue;
}

static std::string JsonExtractObject(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return "";

    char open = json[pos];
    char close = (open == '{') ? '}' : (open == '[') ? ']' : 0;
    if (close == 0) return "";

    int depth = 1;
    size_t start = pos;
    ++pos;
    while (pos < json.size() && depth > 0)
    {
        if (json[pos] == open) ++depth;
        else if (json[pos] == close) --depth;
        else if (json[pos] == '"')
        {
            ++pos;
            while (pos < json.size() && json[pos] != '"')
            {
                if (json[pos] == '\\') ++pos;
                ++pos;
            }
        }
        ++pos;
    }
    return json.substr(start, pos - start);
}

static std::string EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

// ─── WebSocket send ─────────────────────────────────────────────────────

static bool WsSend(const std::string& json)
{
    std::lock_guard<std::mutex> lk(g_wsMutex);
    if (!g_hWebSocket) { Log("[WS] Send: no websocket handle"); return false; }
    DWORD err = WinHttpWebSocketSend(g_hWebSocket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)json.c_str(), (DWORD)json.size());
    if (err != NO_ERROR)
    {
        Log("[WS] Send FAILED: %lu  json=%s", err, json.c_str());
        return false;
    }
    Log("[WS] Send OK (%d bytes): %.200s", (int)json.size(), json.c_str());
    return true;
}

// ─── SD API commands ────────────────────────────────────────────────────

static void SdSetTitle(const std::string& context, const std::string& title, int state = -1)
{
    std::string json = "{\"event\":\"setTitle\",\"context\":\"" + context +
        "\",\"payload\":{\"title\":\"" + EscapeJson(title) + "\",\"target\":0";
    if (state >= 0)
        json += ",\"state\":" + std::to_string(state);
    json += "}}";
    WsSend(json);
}

static void SdSetState(const std::string& context, int state)
{
    std::string json = "{\"event\":\"setState\",\"context\":\"" + context +
        "\",\"payload\":{\"state\":" + std::to_string(state) + "}}";
    WsSend(json);
}

static void SdSendToPropertyInspector(const std::string& context, const std::string& action, const std::string& payloadJson)
{
    std::string json = "{\"event\":\"sendToPropertyInspector\",\"context\":\"" + context +
        "\",\"action\":\"" + action + "\",\"payload\":" + payloadJson + "}";
    WsSend(json);
}

static void SdSetSettings(const std::string& context, const std::string& settingsJson)
{
    std::string json = "{\"event\":\"setSettings\",\"context\":\"" + context +
        "\",\"payload\":" + settingsJson + "}";
    WsSend(json);
}

static void SdGetSettings(const std::string& context)
{
    std::string json = "{\"event\":\"getSettings\",\"context\":\"" + context + "\"}";
    WsSend(json);
}

static std::string BuildOrgansPayload()
{
    std::lock_guard<std::mutex> lk(g_organsMutex);
    std::string json = "{\"connected\":";
    json += g_pipeConnected.load() ? "true" : "false";
    json += ",\"organs\":[";
    for (size_t i = 0; i < g_organs.size(); ++i)
    {
        if (i > 0) json += ",";
        json += "{\"index\":" + std::to_string(g_organs[i].index) +
            ",\"name\":\"" + EscapeJson(g_organs[i].name) + "\"}";
    }
    json += "]}";
    return json;
}

static std::string BuildSwitchesPayload()
{
    std::lock_guard<std::mutex> lk(g_switchesMutex);
    std::string json = "{\"connected\":";
    json += g_pipeConnected.load() ? "true" : "false";
    json += ",\"switches\":[";
    for (size_t i = 0; i < g_switches.size(); ++i)
    {
        if (i > 0) json += ",";
        json += "{\"index\":" + std::to_string(g_switches[i].index) +
            ",\"name\":\"" + EscapeJson(g_switches[i].name) + "\"" +
            ",\"channel\":" + std::to_string(g_switches[i].channel) +
            ",\"cc\":" + std::to_string(g_switches[i].cc) +
            ",\"valueOn\":" + std::to_string(g_switches[i].valueOn) +
            ",\"valueOff\":" + std::to_string(g_switches[i].valueOff) + "}";
    }
    json += "]}";
    return json;
}

// Forward declarations (defined below)
static std::string Base64Encode(const std::string& input);
static void UpdateAllButtons();

// ─── Background image loading ───────────────────────────────────────────

static std::wstring GetPluginDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring dir(path);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        dir.resize(pos + 1);
    return dir;
}

static std::string LoadFileAsBase64(const std::wstring& filePath)
{
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    DWORD size = GetFileSize(hFile, nullptr);
    if (size == 0 || size == INVALID_FILE_SIZE)
    {
        CloseHandle(hFile);
        return "";
    }
    std::string data(size, '\0');
    DWORD bytesRead = 0;
    ReadFile(hFile, &data[0], size, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (bytesRead != size) return "";
    return Base64Encode(data);
}

static void LoadBackgroundImages()
{
    std::wstring dir = GetPluginDir() + L"images\\";
    g_bgImageOff = LoadFileAsBase64(dir + L"organ-off.png");
    g_bgImageOn  = LoadFileAsBase64(dir + L"organ-on.png");
    Log("[Images] Background images: off=%s on=%s",
        g_bgImageOff.empty() ? "not found" : "loaded",
        g_bgImageOn.empty() ? "not found" : "loaded");
}

// ─── XML/SVG helpers ────────────────────────────────────────────────────

static std::string XmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c) {
        case '&':  out += "&amp;"; break;
        case '<':  out += "&lt;"; break;
        case '>':  out += "&gt;"; break;
        case '\'': out += "&apos;"; break;
        case '"':  out += "&quot;"; break;
        default:   out += c;
        }
    }
    return out;
}

static std::vector<std::string> WrapTitleLines(const std::string& text, int maxChars = 12, int maxLines = 4)
{
    std::vector<std::string> lines;
    std::string current;
    std::string word;

    auto flushWord = [&]()
    {
        if (word.empty())
            return;

        if (current.empty())
        {
            current = word;
        }
        else if ((int)(current.size() + 1 + word.size()) <= maxChars)
        {
            current += ' ';
            current += word;
        }
        else
        {
            lines.push_back(current);
            current = word;
        }

        word.clear();
    };

    for (size_t i = 0; i <= text.size(); ++i)
    {
        char c = (i < text.size()) ? text[i] : '\0';
        if (c == ' ' || c == '\0')
        {
            flushWord();
            if ((int)lines.size() >= maxLines)
                break;
        }
        else
        {
            word += c;
        }
    }

    if (!current.empty() && (int)lines.size() < maxLines)
        lines.push_back(current);

    if (lines.empty())
        lines.push_back(text.empty() ? std::string("Empty") : text.substr(0, maxChars));

    if ((int)lines.size() > maxLines)
        lines.resize(maxLines);

    if ((int)lines.size() == maxLines && (int)text.size() > (maxChars * maxLines))
    {
        std::string& last = lines.back();
        if ((int)last.size() > maxChars - 1)
            last.resize(maxChars - 1);
        last += "…";
    }

    return lines;
}

static std::string BuildSwitchButtonSvg(const std::string& title, bool isOn, int midiChannel)
{
    const bool isEmpty = (title == "Empty");
    const bool isOffline = (title == "Disconnected.");

    // Per-channel accent colours
    // ch1=PEDALE: blue  ch2=G.O.: gold  ch3=RECIT.: green  other: grey
    struct ChAccent { const char* hi; const char* lo; const char* bodyOn; const char* bodyOff; const char* insetOn; const char* insetOff; const char* badgeOn; const char* textOn; const char* shadowOn; };
    ChAccent accent;
    if      (midiChannel == 1) accent = { "#4a9eff", "#1a4a8a", "#c8e0ff", "#2f343a", "#a8ccf5", "#3a4048", "#1a3a6a", "#ddeeff", "#90b8e0" }; // blue
    else if (midiChannel == 2) accent = { "#d4b86a", "#5a4720", "#f2e8c9", "#2f343a", "#e6d8a8", "#3a4048", "#5a4720", "#ffe08a", "#c9bb90" }; // gold (original)
    else if (midiChannel == 3) accent = { "#39d353", "#123c1f", "#d4f5da", "#2f343a", "#b8ecbf", "#3a4048", "#0f3d18", "#c8f5d0", "#80cc90" }; // green
    else                       accent = { "#8b949e", "#2a2f36", "#e0e4e8", "#2f343a", "#c8cdd2", "#3a4048", "#20262d", "#c9d1d9", "#606870" }; // grey

    std::string accentHi  = isOffline ? "#ff4d4f" : accent.hi;
    std::string accentLo  = isOffline ? "#4f1012" : accent.lo;

    std::string outerBorder    = isOffline ? "#ff4d4f" : accentHi;
    std::string bodyFill       = isOffline ? "#351012" : isOn ? accent.bodyOn  : accent.bodyOff;
    std::string bodyInset      = isOffline ? "#4f1012" : isOn ? accent.insetOn : accent.insetOff;
    std::string badgeFill      = isOffline ? "#4f1012" : isOn ? accent.badgeOn : "#20262d";
    std::string badgeStroke    = isOffline ? "#ff4d4f" : accentHi;
    std::string badgeText      = isOffline ? "OFFLINE" : (midiChannel == 1 ? "PEDALE" : midiChannel == 2 ? "G.O." : midiChannel == 3 ? "RECIT." : "SWITCH");
    std::string badgeTextColor = isOffline ? "#ffd6d6" : isOn ? accent.textOn : "#c9d1d9";
    std::string titleColor     = isOffline ? "#ffd6d6" : isOn ? accent.bodyOff : "#f0f6fc";
    std::string titleShadow    = isOn ? accent.shadowOn : "#111111";
    std::string ledFill        = isOffline ? "#ff4d4f" : isOn ? accentHi : "#5a6169";
    std::string ledStroke      = isOffline ? "#ffd6d6" : isOn ? accent.textOn : "#c9d1d9";

    std::string svg = "<svg width='144' height='144' xmlns='http://www.w3.org/2000/svg'>";
    svg += "<rect width='144' height='144' rx='14' fill='#0f1115'/>";
    svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='" + bodyFill + "' stroke='" + outerBorder + "' stroke-width='3'/>";
    svg += "<rect x='10' y='10' width='124' height='124' rx='10' fill='" + bodyInset + "' opacity='0.75'/>";
    svg += "<rect x='14' y='12' width='116' height='24' rx='8' fill='" + badgeFill + "' stroke='" + badgeStroke + "' stroke-width='1.5'/>";
    svg += "<text x='72' y='29' font-family='Arial' font-size='12' fill='" + badgeTextColor + "' text-anchor='middle' font-weight='bold' letter-spacing='1'>" + badgeText + "</text>";

    std::string titleForRender = isOffline ? "" : title;
    if (!titleForRender.empty())
    {
        auto lines = WrapTitleLines(titleForRender, 11, 4);
        int fontSize = (int)lines.size() >= 4 ? 15 : (int)lines.size() == 3 ? 17 : 19;
        int lineHeight = fontSize + 4;
        int totalHeight = (int)lines.size() * lineHeight;
        int startY = 58 - (totalHeight / 2) + fontSize;
        for (int i = 0; i < (int)lines.size(); ++i)
        {
            int y = startY + i * lineHeight;
            svg += "<text x='72' y='" + std::to_string(y + 1) + "' font-family='Arial' font-size='" + std::to_string(fontSize) + "' fill='" + titleShadow + "' text-anchor='middle' font-weight='bold'>" + XmlEscape(lines[i]) + "</text>";
            svg += "<text x='72' y='" + std::to_string(y) + "' font-family='Arial' font-size='" + std::to_string(fontSize) + "' fill='" + titleColor + "' text-anchor='middle' font-weight='bold'>" + XmlEscape(lines[i]) + "</text>";
        }
    }

    svg += "<rect x='26' y='116' width='92' height='16' rx='8' fill='#11161c' stroke='" + outerBorder + "' stroke-width='1'/>";
    svg += "<circle cx='42' cy='124' r='6' fill='" + ledFill + "' stroke='" + ledStroke + "' stroke-width='1.8'/>";
    svg += "<text x='78' y='129' font-family='Arial' font-size='12' fill='" + badgeTextColor + "' text-anchor='middle' font-weight='bold'>" + std::string(isOn ? "ON" : isOffline ? "OFF" : "OFF") + "</text>";
    svg += "</svg>";
    return svg;
}

// Build an SVG image for the Console Power (Active Sensing toggle) button
static std::string BuildConsolePowerButtonSvg(const std::string& title, bool isOn)
{
    const bool isOffline = (title == "Disconnected.");

    std::string outerBorder   = isOffline ? "#ff4d4f" : isOn ? "#39d353" : "#8b949e";
    std::string bodyFill      = isOffline ? "#351012" : isOn ? "#0b2e12" : "#1a1f26";
    std::string bodyInset     = isOffline ? "#4f1012" : isOn ? "#0f3d18" : "#2a3040";
    std::string badgeFill     = isOffline ? "#4f1012" : isOn ? "#123c1f" : "#20262d";
    std::string badgeStroke   = isOffline ? "#ff4d4f" : isOn ? "#39d353" : "#8b949e";
    std::string badgeTextCol  = isOffline ? "#ffd6d6" : isOn ? "#85e89d" : "#c9d1d9";
    std::string titleColor    = isOffline ? "#ffd6d6" : isOn ? "#e6f4e9" : "#c9d1d9";
    std::string ledFill       = isOffline ? "#ff4d4f" : isOn ? "#39d353" : "#5a6169";
    std::string ledStroke     = isOffline ? "#ffd6d6" : isOn ? "#c8f5d0" : "#c9d1d9";
    std::string onOffText     = isOffline ? "---" : isOn ? "ON" : "OFF";
    std::string onOffColor    = isOffline ? "#ffd6d6" : isOn ? "#85e89d" : "#c9d1d9";

    std::string svg = "<svg width='144' height='144' xmlns='http://www.w3.org/2000/svg'>";
    svg += "<rect width='144' height='144' rx='14' fill='#0f1115'/>";
    svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='" + bodyFill + "' stroke='" + outerBorder + "' stroke-width='3'/>";
    svg += "<rect x='10' y='10' width='124' height='124' rx='10' fill='" + bodyInset + "' opacity='0.65'/>";
    svg += "<rect x='14' y='12' width='116' height='24' rx='8' fill='" + badgeFill + "' stroke='" + badgeStroke + "' stroke-width='1.5'/>";
    svg += "<text x='72' y='29' font-family='Arial' font-size='10' fill='" + badgeTextCol + "' text-anchor='middle' font-weight='bold' letter-spacing='1'>CONSOLE</text>";

    // Title
    if (!isOffline && !title.empty())
    {
        auto lines = WrapTitleLines(title, 11, 2);
        int fontSize = 18;
        int lineHeight = fontSize + 4;
        int totalHeight = (int)lines.size() * lineHeight;
        int startY = 72 - (totalHeight / 2) + fontSize;
        for (int i = 0; i < (int)lines.size(); ++i)
        {
            int y = startY + i * lineHeight;
            svg += "<text x='72' y='" + std::to_string(y) + "' font-family='Arial' font-size='" + std::to_string(fontSize) + "' fill='" + titleColor + "' text-anchor='middle' font-weight='bold'>" + XmlEscape(lines[i]) + "</text>";
        }
    }

    // ON/OFF indicator bar
    svg += "<rect x='26' y='103' width='92' height='22' rx='11' fill='#11161c' stroke='" + outerBorder + "' stroke-width='1.5'/>";
    svg += "<circle cx='44' cy='114' r='7' fill='" + ledFill + "' stroke='" + ledStroke + "' stroke-width='2'/>";
    svg += "<text x='82' y='119' font-family='Arial' font-size='14' fill='" + onOffColor + "' text-anchor='middle' font-weight='bold'>" + onOffText + "</text>";

    svg += "</svg>";
    return svg;
}

// Build an SVG image for a button with improved visual states and title layout
static std::string BuildButtonSvg(const std::string& title, bool loaded, bool vstLink, bool consoleConnected, bool hauptwerkRunning,
                                  bool loadingActive = false, int loadingProgress = 0, bool requestActive = false, bool requestBlinkOn = true)
{
    const bool isEmpty = (title == "Empty");
    const bool isOffline = (title == "Disconnected.");
    const bool isRequest = requestActive && !loadingActive && !loaded && !isEmpty && !isOffline;
    const bool isLoading = loadingActive && !isEmpty && !isOffline;
    const bool isReady = !loaded && !isEmpty && !isOffline && !isLoading && !isRequest;
    const bool isVstLinkLoaded = loaded && vstLink;
    const bool isReadyYellow = isReady && hauptwerkRunning;
    const bool isConnectedIdle = isReady && !hauptwerkRunning;

    const std::string& bgImage = loaded ? g_bgImageOn : g_bgImageOff;
    std::string frameColor = isRequest ? (requestBlinkOn ? "#ff4d4f" : "#ff9b9c") : isLoading ? "#ffd33d" : isVstLinkLoaded ? "#a371f7" : loaded ? "#39d353" : isOffline ? "#ff4d4f" : isEmpty ? "#7d8590" : isConnectedIdle && consoleConnected ? "#ff7b72" : isConnectedIdle ? "#9aa4b2" : isReadyYellow ? "#ffd33d" : "#58a6ff";
    std::string outerBorderColor = isRequest ? (requestBlinkOn ? "#ff4d4f" : "#7d8590") : isLoading && consoleConnected ? "#ff4d4f" : isLoading ? "#ffd33d" : loaded ? "#39d353" : isOffline ? "#ff4d4f" : isEmpty ? "#7d8590" : isConnectedIdle && consoleConnected ? "#ff4d4f" : isConnectedIdle ? "#7d8590" : isReadyYellow && consoleConnected ? "#ff4d4f" : isReadyYellow ? "#ffd33d" : "#58a6ff";
    std::string badgeBorderColor = ((loaded || isConnectedIdle) && consoleConnected) ? "#ff4d4f" : outerBorderColor;
    std::string badgeText = isRequest ? "REQUEST" : isLoading ? "LOADING" : isVstLinkLoaded ? "VST LINK" : loaded ? "LOADED" : isOffline ? "OFFLINE" : isEmpty ? "EMPTY" : "READY";
    std::string badgeFill = isRequest ? (requestBlinkOn ? "#4f1012" : "#30363d") : isLoading ? "#4a3b0a" : isVstLinkLoaded ? "#2f1a47" : loaded ? "#123c1f" : isOffline ? "#4f1012" : isEmpty ? "#30363d" : isConnectedIdle ? "#2d333b" : isReadyYellow ? "#4a3b0a" : "#0d2d4f";
    std::string titleColor = isVstLinkLoaded ? "#a371f7" : loaded ? "#39d353" : isOffline ? "#ffd6d6" : isEmpty ? "#e6edf3" : isConnectedIdle ? "#e6edf3" : isReadyYellow || isLoading || isRequest ? "#fff1b8" : "#ffffff";
    std::string shadowColor = loaded ? "#0b1f10" : "#111111";
    if (loadingProgress < 0) loadingProgress = 0;
    if (loadingProgress > 100) loadingProgress = 100;

    std::string svg = "<svg width='144' height='144' xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>";
    svg += "<rect width='144' height='144' rx='14' fill='#0f1115'/>";

    if (!bgImage.empty() && !isOffline && !isEmpty && !isConnectedIdle)
    {
        svg += "<image width='144' height='144' preserveAspectRatio='xMidYMid slice' href='data:image/png;base64," + bgImage + "' xlink:href='data:image/png;base64," + bgImage + "' opacity='0.96'/>";
        svg += "<rect width='144' height='144' rx='14' fill='#000000' fill-opacity='0.18'/>";
    }
    else
    {
        std::string bg = loaded ? "#16321d" : isOffline ? "#351012" : isEmpty ? "#20262d" : isConnectedIdle ? "#2d333b" : "#162235";
        svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='" + bg + "'/>";
    }

    svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='none' stroke='" + outerBorderColor + "' stroke-width='3'/>";
    svg += "<rect x='14' y='12' width='116' height='24' rx='8' fill='" + badgeFill + "' stroke='" + badgeBorderColor + "' stroke-width='1.5'/>";
    svg += "<text x='72' y='29' font-family='Arial' font-size='12' fill='" + frameColor + "' text-anchor='middle' font-weight='bold' letter-spacing='1'>" + badgeText + "</text>";
    if (isLoading)
    {
        int progressWidth = static_cast<int>((104.0 * loadingProgress) / 100.0);
        if (loadingProgress > 0 && progressWidth < 8)
            progressWidth = 8;
        svg += "<rect x='20' y='30' width='104' height='5' rx='2' fill='#1f2328' stroke='#c9d1d9' stroke-width='1'/>";
        if (progressWidth > 0)
            svg += "<rect x='20' y='30' width='" + std::to_string(progressWidth) + "' height='5' rx='2' fill='#ffd33d'/>";
    }

    std::string titleForRender = isOffline ? "" : title;
    if (!titleForRender.empty())
    {
        auto lines = WrapTitleLines(titleForRender, 12, 4);
        int fontSize = (int)lines.size() >= 4 ? 16 : (int)lines.size() == 3 ? 18 : 20;
        int lineHeight = fontSize + 4;
        int totalHeight = (int)lines.size() * lineHeight;
        int startY = 78 - (totalHeight / 2) + fontSize;

        for (int i = 0; i < (int)lines.size(); ++i)
        {
            int y = startY + i * lineHeight;
            svg += "<text x='72' y='" + std::to_string(y + 1) + "' font-family='Arial' font-size='" + std::to_string(fontSize) + "' fill='" + shadowColor + "' text-anchor='middle' font-weight='bold'>" + XmlEscape(lines[i]) + "</text>";
            svg += "<text x='72' y='" + std::to_string(y) + "' font-family='Arial' font-size='" + std::to_string(fontSize) + "' fill='" + titleColor + "' text-anchor='middle' font-weight='bold'>" + XmlEscape(lines[i]) + "</text>";
        }
    }

    std::string consoleDotFill = "#7d8590";
    std::string consoleDotStroke = "#e6edf3";
    std::string consoleDotStrokeWidth = "1.5";
    if (isOffline)
    {
        consoleDotFill = "#ff4d4f";
        consoleDotStroke = "#ffd6d6";
        consoleDotStrokeWidth = "2.5";
    }
    else if (isConnectedIdle && consoleConnected)
    {
        consoleDotFill = "#7d8590";
        consoleDotStroke = "#ff4d4f";
        consoleDotStrokeWidth = "2.5";
    }
    else if (hauptwerkRunning)
    {
        const bool hasLoadedOrgan = loaded;
        consoleDotFill = hasLoadedOrgan ? "#39d353" : "#ffd33d";
        consoleDotStroke = consoleConnected ? "#ff4d4f" : "#dcffe4";
        consoleDotStrokeWidth = consoleConnected ? "2.5" : "1.5";
    }
    svg += "<circle cx='123' cy='122' r='7' fill='" + consoleDotFill + "' stroke='" + consoleDotStroke + "' stroke-width='" + consoleDotStrokeWidth + "'/>";

    svg += "</svg>";
    return svg;
}

static std::string Base64Encode(const std::string& input)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < input.size()) {
        size_t remaining = input.size() - i;
        uint32_t a = (uint8_t)input[i++];
        uint32_t b = (remaining > 1) ? (uint8_t)input[i++] : 0;
        uint32_t c = (remaining > 2) ? (uint8_t)input[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out += table[(triple >> 18) & 0x3F];
        out += table[(triple >> 12) & 0x3F];
        out += (remaining > 1) ? table[(triple >> 6) & 0x3F] : '=';
        out += (remaining > 2) ? table[triple & 0x3F] : '=';
    }
    return out;
}

static void SdSetImage(const std::string& context, const std::string& svg, int state = -1)
{
    std::string b64 = Base64Encode(svg);
    std::string payload = "data:image/svg+xml;base64," + b64;
    std::string json = "{\"event\":\"setImage\",\"context\":\"" + context +
        "\",\"payload\":{\"image\":\"" + payload + "\",\"target\":0";
    if (state >= 0)
        json += ",\"state\":" + std::to_string(state);
    json += "}}";
    WsSend(json);
}

// ─── Resolve display title for a button ─────────────────────────────────

static bool IsOrganLoading(int organIndex, int* progressOut)
{
    int simProgress = -1;
    {
        std::lock_guard<std::mutex> lk(g_loadingMutex);
        auto it = g_loadingProgressByOrgan.find(organIndex);
        if (it != g_loadingProgressByOrgan.end())
            simProgress = it->second;
    }

    const int realIdx = g_realLoadingIndex.load();
    const int realProgress = g_realLoadingProgress.load();
    const bool realVisible = g_realLoadingVisible.load();
    const ULONGLONG realTick = g_realLoadingLastTick.load();
    const ULONGLONG now = GetTickCount64();
    if (organIndex > 0 && realIdx == organIndex && realProgress >= 0 && realVisible && (now - realTick) <= 3000)
    {
        int merged = realProgress;
        if (simProgress > merged)
            merged = simProgress;
        if (progressOut)
            *progressOut = merged;
        return true;
    }

    if (simProgress >= 0)
    {
        if (progressOut)
            *progressOut = simProgress;
        return true;
    }

    return false;
}

static bool IsOrganRequesting(int organIndex)
{
    if (organIndex <= 0)
        return false;
    std::lock_guard<std::mutex> lk(g_loadingMutex);
    return g_requestByOrgan.find(organIndex) != g_requestByOrgan.end();
}

static void RequestBlinkThread()
{
    while (g_running)
    {
        Sleep(350);
        {
            std::lock_guard<std::mutex> lk(g_loadingMutex);
            if (g_requestByOrgan.empty())
            {
                g_requestWorkerRunning = false;
                return;
            }
        }
        UpdateAllButtons();
    }
    g_requestWorkerRunning = false;
}

static void StartOrganRequest(int organIndex)
{
    if (organIndex <= 0)
        return;
    {
        std::lock_guard<std::mutex> lk(g_loadingMutex);
        g_requestByOrgan[organIndex] = GetTickCount64();
    }
    UpdateAllButtons();
    bool expected = false;
    if (g_requestWorkerRunning.compare_exchange_strong(expected, true))
        std::thread(RequestBlinkThread).detach();
}

static void StopOrganRequest(int organIndex)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_loadingMutex);
        auto it = g_requestByOrgan.find(organIndex);
        if (it != g_requestByOrgan.end())
        {
            g_requestByOrgan.erase(it);
            changed = true;
        }
    }
    if (changed)
        UpdateAllButtons();
}

static void ClearOrganRequests()
{
    std::lock_guard<std::mutex> lk(g_loadingMutex);
    g_requestByOrgan.clear();
}

static void LoadingProgressThread()
{
    while (g_running)
    {
        Sleep(1000);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_loadingMutex);
            if (g_loadingProgressByOrgan.empty())
            {
                g_loadingWorkerRunning = false;
                return;
            }
            for (auto& kv : g_loadingProgressByOrgan)
            {
                if (kv.second < 99)
                {
                    kv.second += 10;
                    if (kv.second > 99)
                        kv.second = 99;
                    changed = true;
                }
            }
        }
        if (changed)
            UpdateAllButtons();
    }
    g_loadingWorkerRunning = false;
}

static void StartOrganLoadingProgress(int organIndex)
{
    if (organIndex <= 0)
        return;
    StopOrganRequest(organIndex);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_loadingMutex);
        if (g_loadingProgressByOrgan.find(organIndex) == g_loadingProgressByOrgan.end())
        {
            g_loadingProgressByOrgan[organIndex] = 0;
            changed = true;
        }
    }
    if (changed)
        UpdateAllButtons();
    bool expected = false;
    if (g_loadingWorkerRunning.compare_exchange_strong(expected, true))
        std::thread(LoadingProgressThread).detach();
}

static void CompleteOrganLoadingProgress(int organIndex)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_loadingMutex);
        auto it = g_loadingProgressByOrgan.find(organIndex);
        if (it != g_loadingProgressByOrgan.end())
        {
            g_loadingProgressByOrgan.erase(it);
            changed = true;
        }
    }
    if (changed)
        UpdateAllButtons();
}

static std::string ResolveButtonTitle(int organIndex, const std::string& cachedOrganName)
{
    if (organIndex == 0)
        return "Empty";
    if (!g_pipeConnected.load())
        return "Disconnected.";

    {
        std::lock_guard<std::mutex> ol(g_organsMutex);
        for (auto& o : g_organs)
        {
            if (o.index == organIndex)
                return o.name;
        }
    }
    if (!cachedOrganName.empty())
        return cachedOrganName;
    return "ORGAN " + std::to_string(organIndex);
}

static std::string ResolveSwitchTitle(int switchIndex, const std::string& cachedName)
{
    if (switchIndex == 0)
        return "Empty";
    if (!g_pipeConnected.load())
        return "Disconnected.";

    {
        std::lock_guard<std::mutex> lk(g_switchesMutex);
        for (const auto& s : g_switches)
        {
            if (s.index == switchIndex)
                return s.name;
        }
    }
    if (!cachedName.empty())
        return cachedName;
    return "SWITCH " + std::to_string(switchIndex);
}

static std::string ResolveActiveSensingTitle()
{
    return "CONSOLE";
}

// ─── Update all visible buttons ─────────────────────────────────────────

static void UpdateAllButtons()
{
    std::lock_guard<std::mutex> lk(g_buttonsMutex);
    int loaded = 0;
    bool loadedUsesVstLink = false;
    bool consoleConnected = false;
    bool hauptwerkRunning = false;
    {
        std::lock_guard<std::mutex> sl(g_stateMutex);
        loaded = g_loadedOrganIndex;
        loadedUsesVstLink = g_loadedOrganUsesVstLink;
        consoleConnected = g_consoleConnected;
        hauptwerkRunning = g_hauptwerkRunning;
    }

    Log("[UpdateAllButtons] visibleButtons=%d  loadedOrganIndex=%d",
        (int)g_visibleButtons.size(), loaded);

    for (auto& [ctx, info] : g_visibleButtons)
    {
        bool isSwitchAction = (info.action == "com.ahlbornbridge.switch");
        bool isActiveSensingAction = (info.action == "com.ahlbornbridge.activesensing");
        int switchChannel = 0;
        if (isSwitchAction)
        {
            std::lock_guard<std::mutex> sk(g_switchesMutex);
            for (const auto& s : g_switches)
            {
                if (s.index == info.switchIndex)
                {
                    switchChannel = s.channel;
                    break;
                }
            }
        }
        std::string title = isActiveSensingAction
            ? ResolveActiveSensingTitle()
            : (isSwitchAction ? ResolveSwitchTitle(info.switchIndex, info.organName)
                              : ResolveButtonTitle(info.organIndex, info.organName));

        if (!isActiveSensingAction && title != info.organName && title != "Empty" && title != "Disconnected."
            && title.rfind(isSwitchAction ? "SWITCH " : "ORGAN ", 0) != 0)
        {
            info.organName = title;
            std::string settingsJson = isSwitchAction
                ? ("{\"switchIndex\":" + std::to_string(info.switchIndex) + ",\"switchName\":\"" + EscapeJson(title) + "\"}")
                : ("{\"organIndex\":" + std::to_string(info.organIndex) + ",\"organName\":\"" + EscapeJson(title) + "\"}");
            SdSetSettings(ctx, settingsJson);
        }

        int st = isActiveSensingAction
            ? (g_activeSensingEnabledState.load() ? 1 : 0)
            : (isSwitchAction
                ? (info.switchIsOn ? 1 : 0)
                : ((info.organIndex == loaded && loaded > 0) ? 1 : 0));
        int loadingProgress = 0;
        bool isLoading = (!isSwitchAction && !isActiveSensingAction) && (st == 0) && IsOrganLoading(info.organIndex, &loadingProgress);
        bool isRequest = (!isSwitchAction && !isActiveSensingAction) && (st == 0) && !isLoading && IsOrganRequesting(info.organIndex);
        bool requestBlinkOn = ((GetTickCount64() / 350ULL) % 2ULL) == 0ULL;
        bool biduleBlinkPhase = ((GetTickCount64() / 350ULL) % 2ULL) == 0ULL;
        bool forceRequestVisual = (!isSwitchAction && !isActiveSensingAction) && (st == 1) && loadedUsesVstLink && g_biduleSplashActive && biduleBlinkPhase;
        Log("  button ctx=%.30s organIndex=%d title=%s state=%d request=%d loading=%d progress=%d biduleSplash=%d",
            ctx.c_str(), info.organIndex, title.c_str(), st, (int)isRequest, (int)isLoading, loadingProgress, (int)g_biduleSplashActive);
        std::string svg = isActiveSensingAction
            ? BuildConsolePowerButtonSvg(title, st == 1)
            : (isSwitchAction
                ? BuildSwitchButtonSvg(title, st == 1, switchChannel)
                : BuildButtonSvg(title, st == 1, st == 1 && loadedUsesVstLink, consoleConnected, hauptwerkRunning, isLoading, loadingProgress, isRequest || forceRequestVisual, requestBlinkOn));
        if (svg != info.lastSvg)
        {
            SdSetImage(ctx, svg, 0);
            SdSetImage(ctx, svg, 1);
            SdSetTitle(ctx, "", 0);
            SdSetTitle(ctx, "", 1);
            info.lastSvg = svg;
        }
        if (st != info.lastState)
        {
            SdSetState(ctx, st);
            info.lastState = st;
        }
    }
}

// ─── Update a single button by context

static void UpdateButtonForContext(const std::string& context, int organIndex)
{
    std::string cachedName;
    std::string action;
    int switchIndex = 0;
    bool switchIsOn = false;
    {
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        auto it = g_visibleButtons.find(context);
        if (it != g_visibleButtons.end())
        {
            cachedName = it->second.organName;
            action = it->second.action;
            switchIndex = it->second.switchIndex;
            switchIsOn = it->second.switchIsOn;
        }
    }
    bool isSwitchAction = (action == "com.ahlbornbridge.switch");
    bool isActiveSensingAction = (action == "com.ahlbornbridge.activesensing");
    int switchChannel = 0;
    if (isSwitchAction && switchIndex > 0)
    {
        std::lock_guard<std::mutex> sk(g_switchStateMutex);
        auto stIt = g_currentSwitchStateByIndex.find(switchIndex);
        if (stIt != g_currentSwitchStateByIndex.end())
            switchIsOn = stIt->second;
    }
    if (isSwitchAction)
    {
        std::lock_guard<std::mutex> sk(g_switchesMutex);
        for (const auto& s : g_switches)
        {
            if (s.index == switchIndex)
            {
                switchChannel = s.channel;
                break;
            }
        }
    }
    std::string title = isActiveSensingAction
        ? ResolveActiveSensingTitle()
        : (isSwitchAction ? ResolveSwitchTitle(switchIndex, cachedName)
                          : ResolveButtonTitle(organIndex, cachedName));

    int loaded;
    bool loadedUsesVstLink;
    bool consoleConnected;
    bool hauptwerkRunning;
    {
        std::lock_guard<std::mutex> sl(g_stateMutex);
        loaded = g_loadedOrganIndex;
        loadedUsesVstLink = g_loadedOrganUsesVstLink;
        consoleConnected = g_consoleConnected;
        hauptwerkRunning = g_hauptwerkRunning;
    }
    bool isLoaded = isActiveSensingAction
        ? g_activeSensingEnabledState.load()
        : (isSwitchAction ? switchIsOn : (organIndex == loaded && loaded > 0));
    int loadingProgress = 0;
    bool isLoading = !isSwitchAction && !isActiveSensingAction && !isLoaded && IsOrganLoading(organIndex, &loadingProgress);
    bool isRequest = !isSwitchAction && !isActiveSensingAction && !isLoaded && !isLoading && IsOrganRequesting(organIndex);
    bool requestBlinkOn = ((GetTickCount64() / 350ULL) % 2ULL) == 0ULL;
    bool biduleBlinkPhase = ((GetTickCount64() / 350ULL) % 2ULL) == 0ULL;
    bool forceRequestVisual = !isSwitchAction && !isActiveSensingAction && isLoaded && loadedUsesVstLink && g_biduleSplashActive && biduleBlinkPhase;
    bool requestBlinkState = forceRequestVisual ? biduleBlinkPhase : requestBlinkOn;
    std::string svg = isActiveSensingAction
        ? BuildConsolePowerButtonSvg(title, isLoaded)
        : (isSwitchAction
            ? BuildSwitchButtonSvg(title, isLoaded, switchChannel)
            : BuildButtonSvg(title, isLoaded, isLoaded && loadedUsesVstLink, consoleConnected, hauptwerkRunning, isLoading, loadingProgress, isRequest || forceRequestVisual, requestBlinkState));
    int newState = isLoaded ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        auto it = g_visibleButtons.find(context);
        if (it != g_visibleButtons.end())
        {
            if (svg != it->second.lastSvg)
            {
                SdSetImage(context, svg, 0);
                SdSetImage(context, svg, 1);
                SdSetTitle(context, "", 0);
                SdSetTitle(context, "", 1);
                it->second.lastSvg = svg;
            }
            if (newState != it->second.lastState)
            {
                SdSetState(context, newState);
                it->second.lastState = newState;
            }
            return;
        }
    }
    SdSetImage(context, svg, 0);
    SdSetImage(context, svg, 1);
    SdSetTitle(context, "", 0);
    SdSetTitle(context, "", 1);
    SdSetState(context, newState);
}

// ─── Named pipe communication

static void PipeSend(const std::string& json)
{
    std::lock_guard<std::mutex> lk(g_pipeMutex);
    if (g_pipe == INVALID_HANDLE_VALUE)
    {
        Log("[Pipe] PipeSend: pipe not connected, dropping: %.200s", json.c_str());
        return;
    }
    std::string msg = json + "\n";
    DWORD written = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = WriteFile(g_pipe, msg.c_str(), (DWORD)msg.size(), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        DWORD wait = WaitForSingleObject(ov.hEvent, 5000);
        if (wait == WAIT_OBJECT_0)
            ok = GetOverlappedResult(g_pipe, &ov, &written, FALSE);
        else
        {
            CancelIo(g_pipe);
            Log("[Pipe] PipeSend TIMEOUT: %.200s", json.c_str());
            CloseHandle(ov.hEvent);
            return;
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok)
        Log("[Pipe] PipeSend FAILED (err=%lu written=%lu): %.200s", GetLastError(), written, json.c_str());
    else
        Log("[Pipe] PipeSend OK (%lu bytes): %.200s", written, json.c_str());
}

static bool ConnectPipe()
{
    std::lock_guard<std::mutex> lk(g_pipeMutex);
    if (g_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }

    g_pipe = CreateFileW(
        L"\\\\.\\pipe\\AhlbornBridge",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if (g_pipe == INVALID_HANDLE_VALUE)
    {
        // Only log occasionally to avoid flooding
        static int failCount = 0;
        if (++failCount % 15 == 1)
            Log("[Pipe] ConnectPipe failed (attempt %d): %lu", failCount, GetLastError());
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(g_pipe, &mode, nullptr, nullptr);
    g_pipeConnected = true;
    Log("[Pipe] Connected to AhlbornBridge");
    return true;
}

// Parse organ list: {"type":"organs","list":[{"index":1,"name":"..."},...]
static void ParseOrganList(const std::string& json)
{
    std::string listStr = JsonExtractObject(json, "list");
    if (listStr.empty()) return;

    std::vector<OrganEntry> organs;
    size_t pos = 0;
    while (pos < listStr.size())
    {
        size_t objStart = listStr.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = listStr.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = listStr.substr(objStart, objEnd - objStart + 1);

        OrganEntry e;
        e.index = JsonExtractInt(obj, "index");
        e.name = JsonExtractString(obj, "name");
        if (e.index > 0)
            organs.push_back(e);

        pos = objEnd + 1;
    }

    int count = (int)organs.size();
    {
        std::lock_guard<std::mutex> lk(g_organsMutex);
        g_organs = std::move(organs);
    }

    Log("[Pipe] ParseOrganList: received %d organs", count);
    UpdateAllButtons();
}

static void ParseSwitchList(const std::string& json)
{
    std::string listStr = JsonExtractObject(json, "switches");
    if (listStr.empty()) return;

    std::vector<SwitchEntry> switches;
    size_t pos = 0;
    while (pos < listStr.size())
    {
        size_t objStart = listStr.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = listStr.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = listStr.substr(objStart, objEnd - objStart + 1);

        SwitchEntry e;
        e.index = JsonExtractInt(obj, "index");
        e.name = JsonExtractString(obj, "name");
        e.channel = JsonExtractInt(obj, "channel");
        e.cc = JsonExtractInt(obj, "cc");
        e.valueOn = JsonExtractInt(obj, "valueOn");
        e.valueOff = JsonExtractInt(obj, "valueOff");
        e.momentary = (JsonExtractInt(obj, "momentary") != 0);
        if (e.index > 0)
            switches.push_back(e);

        pos = objEnd + 1;
    }

    int count = (int)switches.size();
    {
        std::lock_guard<std::mutex> lk(g_switchesMutex);
        g_switches = std::move(switches);
    }
    {
        std::lock_guard<std::mutex> lk(g_switchStateMutex);
        for (const auto& s : g_switches)
        {
            if (g_currentSwitchStateByIndex.find(s.index) == g_currentSwitchStateByIndex.end())
                g_currentSwitchStateByIndex[s.index] = false;
        }
    }

    Log("[Pipe] ParseSwitchList: received %d switches", count);

    std::vector<std::pair<std::string, std::string>> targets;
    {
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        for (const auto& [ctx, info] : g_visibleButtons)
        {
            if (info.action == "com.ahlbornbridge.switch")
                targets.push_back({ ctx, info.action });
        }
    }

    std::string payload = BuildSwitchesPayload();
    for (const auto& target : targets)
        SdSendToPropertyInspector(target.first, target.second, payload);

    // Persiste il flag momentary nelle settings di ogni pulsante switch visibile
    {
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        for (const auto& [ctx, info] : g_visibleButtons)
        {
            if (info.action != "com.ahlbornbridge.switch") continue;
            int idx = 0;
            {
                // recupera switchIndex dalle settings correnti tramite g_switches
                std::lock_guard<std::mutex> sk(g_switchesMutex);
                for (const auto& s : g_switches)
                {
                    // cerca per nome nel buttonInfo non disponibile qui;
                    // usiamo un secondo loop separato sotto
                    (void)s;
                }
            }
        }
    }
    // Aggiorna settings momentary per tutti i pulsanti switch visibili
    {
        std::vector<std::pair<std::string,int>> ctxSwitchIndex;
        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            for (const auto& [ctx, info] : g_visibleButtons)
            {
                if (info.action == "com.ahlbornbridge.switch" && info.switchIndex > 0)
                    ctxSwitchIndex.push_back({ctx, info.switchIndex});
            }
        }
        std::lock_guard<std::mutex> sk(g_switchesMutex);
        for (const auto& [ctx, sidx] : ctxSwitchIndex)
        {
            for (const auto& s : g_switches)
            {
                if (s.index == sidx)
                {
                    std::string sj = "{\"switchIndex\":" + std::to_string(s.index)
                        + ",\"switchName\":\"" + EscapeJson(s.name) + "\""
                        + ",\"switchIsOn\":false"
                        + ",\"momentary\":" + (s.momentary ? "true" : "false") + "}";
                    SdSetSettings(ctx, sj);
                    break;
                }
            }
        }
    }
}

static void HandlePipeMessage(const std::string& msg)
{
    Log("[Pipe] HandlePipeMessage: %.200s", msg.c_str());
    if (msg.find("\"organs\"") != std::string::npos && msg.find("\"list\"") != std::string::npos)
    {
        ParseOrganList(msg);
    }
    else if (msg.find("\"switches\"") != std::string::npos)
    {
        ParseSwitchList(msg);
    }
    else if (msg.find("\"switchState\"") != std::string::npos)
    {
        int switchIndex = JsonExtractInt(msg, "index");
        bool isOn = JsonExtractBool(msg, "isOn", false);
        if (switchIndex > 0)
        {
            int loadedNow = 0;
            {
                std::lock_guard<std::mutex> sl(g_stateMutex);
                loadedNow = g_loadedOrganIndex;
            }

            bool ignoreUnloadFreeze = g_freezeSwitchStateDuringUnload.load() && loadedNow == 0;
            if (ignoreUnloadFreeze)
            {
                Log("[Pipe] Ignored switchState during unload freeze: index=%d isOn=%d", switchIndex, (int)isOn);
            }
            else
            {
                {
                    std::lock_guard<std::mutex> sk(g_switchStateMutex);
                    g_currentSwitchStateByIndex[switchIndex] = isOn;
                }

                std::vector<std::string> targets;
                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    for (auto& [ctx, info] : g_visibleButtons)
                    {
                        if (info.action == "com.ahlbornbridge.switch" && info.switchIndex == switchIndex)
                        {
                            info.switchIsOn = isOn;
                            targets.push_back(ctx);
                        }
                    }
                }

                for (const auto& ctx : targets)
                    UpdateButtonForContext(ctx, 0);

                Log("[Pipe] Switch state sync: index=%d isOn=%d targets=%d", switchIndex, (int)isOn, (int)targets.size());
            }
        }
    }
    else if (msg.find("\"activeSensingState\"") != std::string::npos)
    {
        bool enabled = JsonExtractBool(msg, "enabled", true);
        g_activeSensingEnabledState.store(enabled);
        UpdateAllButtons();
    }
    else if (msg.find("\"state\"") != std::string::npos)
    {
        int loaded = JsonExtractInt(msg, "loadedIndex");
        int loadingIndex = JsonExtractInt(msg, "loadingIndex");
        int loadingProgress = JsonExtractInt(msg, "loadingProgress");
        bool loadingVisible = (msg.find("\"loadingVisible\":true") != std::string::npos);
        if (loadingProgress < 0) loadingProgress = 0;
        if (loadingProgress > 100) loadingProgress = 100;
        bool vstLink = (msg.find("\"vstLink\":true") != std::string::npos);
        bool consoleConnected = (msg.find("\"consoleConnected\":true") != std::string::npos);
        bool hauptwerkRunning = (msg.find("\"hauptwerkRunning\":true") != std::string::npos);
        bool biduleSplashActive = (msg.find("\"biduleSplashActive\":true") != std::string::npos);
        int previousLoaded = 0;
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            previousLoaded = g_loadedOrganIndex;
            g_loadedOrganIndex = loaded;
            g_loadedOrganUsesVstLink = (loaded > 0) && vstLink;
            g_consoleConnected = consoleConnected;
            g_hauptwerkRunning = hauptwerkRunning;
            g_biduleSplashActive = biduleSplashActive;
        }
        if (loaded > 0)
        {
            g_realLoadingIndex = 0;
            g_realLoadingProgress = -1;
            g_realLoadingVisible = false;
            StopOrganRequest(loaded);
            CompleteOrganLoadingProgress(loaded);

            g_freezeSwitchStateDuringUnload.store(false);
        }
        else if (loadingIndex > 0 && loadingVisible)
        {
            g_realLoadingIndex = loadingIndex;
            g_realLoadingProgress = loadingProgress;
            g_realLoadingVisible = true;
            g_realLoadingLastTick = GetTickCount64();
            StopOrganRequest(loadingIndex);

            // Keep the latest real progress so the loading bar is visible immediately.
            {
                std::lock_guard<std::mutex> lk(g_loadingMutex);
                auto& slot = g_loadingProgressByOrgan[loadingIndex];
                if (loadingProgress > slot)
                    slot = loadingProgress;
            }
            StartOrganLoadingProgress(loadingIndex);
        }
        else
        {
            int prevLoading = g_realLoadingIndex.load();
            g_realLoadingIndex = 0;
            g_realLoadingProgress = -1;
            g_realLoadingVisible = false;
            if (prevLoading > 0)
                CompleteOrganLoadingProgress(prevLoading);
        }

        if (previousLoaded > 0 && previousLoaded != loaded)
            CompleteOrganLoadingProgress(previousLoaded);

        if (loaded > 0 && previousLoaded != loaded)
            UpdateAllButtons();
        Log("[Pipe] State update: loadedIndex=%d loadingIndex=%d loadingProgress=%d loadingVisible=%d biduleSplashActive=%d vstLink=%d consoleConnected=%d hauptwerkRunning=%d",
            loaded, loadingIndex, loadingProgress, (int)loadingVisible, (int)biduleSplashActive, (int)((loaded > 0) && vstLink), (int)consoleConnected, (int)hauptwerkRunning);
        UpdateAllButtons();
    }
}

static void PipeReaderThread()
{
    Log("[Pipe] Reader thread started");
    while (g_running)
    {
        if (!g_pipeConnected)
        {
            if (!ConnectPipe())
            {
                Sleep(2000);
                continue;
            }

            // Request initial data
            Log("[Pipe] Requesting initial organ list, switch list and state");
            PipeSend("{\"type\":\"getOrgans\"}");
            PipeSend("{\"type\":\"getSwitches\"}");
            PipeSend("{\"type\":\"getState\"}");
            PipeSend("{\"type\":\"getActiveSensing\"}");
        }

        char buffer[4096];
        DWORD bytesRead = 0;
        HANDLE pipe;
        {
            std::lock_guard<std::mutex> lk(g_pipeMutex);
            pipe = g_pipe;
        }

        OVERLAPPED readOv = {};
        readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, &readOv);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            // Wait with periodic checks so we can exit cleanly
            while (g_running)
            {
                DWORD wait = WaitForSingleObject(readOv.hEvent, 500);
                if (wait == WAIT_OBJECT_0)
                {
                    ok = GetOverlappedResult(pipe, &readOv, &bytesRead, FALSE);
                    break;
                }
                // Timeout — loop back to check g_running
            }
            if (!g_running)
            {
                CancelIo(pipe);
                CloseHandle(readOv.hEvent);
                break;
            }
        }
        CloseHandle(readOv.hEvent);

        if (!ok || bytesRead == 0)
        {
            Log("[Pipe] Disconnected from AhlbornBridge (ok=%d bytes=%lu err=%lu)",
                (int)ok, bytesRead, GetLastError());
            {
                std::lock_guard<std::mutex> lk(g_pipeMutex);
                if (g_pipe != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(g_pipe);
                    g_pipe = INVALID_HANDLE_VALUE;
                }
            }
            g_pipeConnected = false;
            {
                std::lock_guard<std::mutex> ol(g_organsMutex);
                g_organs.clear();
            }
            {
                std::lock_guard<std::mutex> sl(g_stateMutex);
                g_loadedOrganIndex = 0;
                g_loadedOrganUsesVstLink = false;
                g_consoleConnected = false;
                g_hauptwerkRunning = false;
            }
            g_realLoadingIndex = 0;
            g_realLoadingProgress = -1;
            g_realLoadingVisible = false;
            ClearOrganRequests();
            {
                std::lock_guard<std::mutex> ll(g_loadingMutex);
                g_loadingProgressByOrgan.clear();
            }
            UpdateAllButtons();
            Sleep(2000);
            continue;
        }

        buffer[bytesRead] = '\0';
        static std::string partial;
        partial += buffer;

        size_t pos;
        while ((pos = partial.find('\n')) != std::string::npos)
        {
            std::string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);
            if (!line.empty())
                HandlePipeMessage(line);
        }
    }
    Log("[Pipe] Reader thread stopped");
}

// ─── HTTP listener for PI → Plugin direct communication ─────────────────

static SOCKET g_httpListener = INVALID_SOCKET;

static std::string UrlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            int hi = 0, lo = 0;
            char c1 = s[i + 1], c2 = s[i + 2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            out += (char)((hi << 4) | lo);
            i += 2;
        }
        else if (s[i] == '+')
            out += ' ';
        else
            out += s[i];
    }
    return out;
}

static std::string ExtractQueryParam(const std::string& request, const std::string& param)
{
    std::string search = param + "=";
    size_t pos = request.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = request.find_first_of("& \r\n", pos);
    if (end == std::string::npos) end = request.size();
    return UrlDecode(request.substr(pos, end - pos));
}

static void HttpListenerThread()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Log("[HTTP] WSAStartup failed");
        return;
    }

    g_httpListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_httpListener == INVALID_SOCKET)
    {
        Log("[HTTP] socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return;
    }

    int optval = 1;
    setsockopt(g_httpListener, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PI_HTTP_PORT);

    if (bind(g_httpListener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        Log("[HTTP] bind(%d) failed: %d", PI_HTTP_PORT, WSAGetLastError());
        closesocket(g_httpListener);
        g_httpListener = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    listen(g_httpListener, 5);
    Log("[HTTP] Listening on port %d", PI_HTTP_PORT);

    while (g_running)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_httpListener, &readfds);
        timeval tv = { 1, 0 };
        int sel = select(0, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        SOCKET client = accept(g_httpListener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        char buf[4096];
        int n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            buf[n] = '\0';
            std::string request(buf, n);
            Log("[HTTP] Request: %.200s", request.c_str());

            std::string ctx = ExtractQueryParam(request, "ctx");
            std::string organStr = ExtractQueryParam(request, "organ");
            std::string switchStr = ExtractQueryParam(request, "switch");
            int organIndex = atoi(organStr.c_str());
            int switchIndex = atoi(switchStr.c_str());

            if (!ctx.empty() && organIndex > 0)
            {
                Log("[HTTP] SetOrgan: ctx=%.40s organIndex=%d", ctx.c_str(), organIndex);

                // Look up organ name for settings persistence
                std::string organName;
                {
                    std::lock_guard<std::mutex> ol(g_organsMutex);
                    for (auto& o : g_organs)
                    {
                        if (o.index == organIndex)
                        {
                            organName = o.name;
                            break;
                        }
                    }
                }

                // Persist settings via SD API (plugin-side setSettings works)
                std::string settingsJson = "{\"organIndex\":" + std::to_string(organIndex);
                if (!organName.empty())
                    settingsJson += ",\"organName\":\"" + EscapeJson(organName) + "\"";
                settingsJson += "}";
                SdSetSettings(ctx, settingsJson);

                // Update internal state
                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    auto it = g_visibleButtons.find(ctx);
                    if (it != g_visibleButtons.end())
                    {
                        it->second.organIndex = organIndex;
                        if (!organName.empty())
                            it->second.organName = organName;
                    }
                }

                // Update button display
                UpdateButtonForContext(ctx, organIndex);
            }
            else if (!ctx.empty() && switchIndex > 0)
            {
                Log("[HTTP] SetSwitch: ctx=%.40s switchIndex=%d", ctx.c_str(), switchIndex);

                std::string switchName;
                {
                    std::lock_guard<std::mutex> sk(g_switchesMutex);
                    for (const auto& s : g_switches)
                    {
                        if (s.index == switchIndex)
                        {
                            switchName = s.name;
                            break;
                        }
                    }
                }

                std::string settingsJson = "{\"switchIndex\":" + std::to_string(switchIndex)
                    + ",\"switchName\":\"" + EscapeJson(switchName) + "\""
                    + ",\"switchIsOn\":false}";
                SdSetSettings(ctx, settingsJson);

                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    auto it = g_visibleButtons.find(ctx);
                    if (it != g_visibleButtons.end())
                    {
                        it->second.switchIndex = switchIndex;
                        it->second.switchIsOn = false;
                        if (!switchName.empty())
                            it->second.organName = switchName;
                    }
                }

                UpdateButtonForContext(ctx, 0);
            }
        }

        // Send CORS-friendly response
        const char* resp = "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n\r\nOK";
        send(client, resp, (int)strlen(resp), 0);
        closesocket(client);
    }

    closesocket(g_httpListener);
    g_httpListener = INVALID_SOCKET;
    WSACleanup();
    Log("[HTTP] Listener stopped");
}

// ─── WebSocket event handling ───────────────────────────────────────────

static void HandleSdEvent(const std::string& json)
{
    std::string event = JsonExtractString(json, "event");
    std::string context = JsonExtractString(json, "context");
    std::string action = JsonExtractString(json, "action");

    Log("[SD] Event: %s  context=%.40s", event.c_str(), context.c_str());

    if (event == "willAppear")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string settings = JsonExtractObject(payload, "settings");
        int organIndex = JsonExtractInt(settings, "organIndex");
        std::string organName = JsonExtractString(settings, "organName");
        std::string device = JsonExtractString(json, "device");

        int switchIndex = JsonExtractInt(settings, "switchIndex");
        bool switchIsOn = false;
        std::string switchName = JsonExtractString(settings, "switchName");
        {
            std::lock_guard<std::mutex> sk(g_switchStateMutex);
            auto sit = g_currentSwitchStateByIndex.find(switchIndex);
            if (sit != g_currentSwitchStateByIndex.end())
                switchIsOn = sit->second;
        }
        Log("[SD] willAppear: context=%.40s organIndex=%d switchIndex=%d organName=%s device=%.60s action=%s",
            context.c_str(), organIndex, switchIndex, organName.c_str(), device.c_str(), action.c_str());

        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            ButtonInfo info;
            info.context = context;
            info.action = action;
            info.organIndex = organIndex;
            info.organName = (action == "com.ahlbornbridge.switch") ? switchName : organName;
            info.switchIndex = switchIndex;
            info.switchIsOn = switchIsOn;
            g_visibleButtons[context] = info;
        }

        std::string title = (action == "com.ahlbornbridge.activesensing") ? ResolveActiveSensingTitle()
                           : ((action == "com.ahlbornbridge.switch") ? ResolveSwitchTitle(switchIndex, switchName)
                                                                       : ResolveButtonTitle(organIndex, organName));

        int loaded;
        bool loadedUsesVstLink;
        bool consoleConnected;
        bool hauptwerkRunning;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
            loadedUsesVstLink = g_loadedOrganUsesVstLink;
            consoleConnected = g_consoleConnected;
            hauptwerkRunning = g_hauptwerkRunning;
        }
        bool isSwitchAction = (action == "com.ahlbornbridge.switch");
        bool isActiveSensingAction = (action == "com.ahlbornbridge.activesensing");
        int switchChannel = 0;
        if (isSwitchAction)
        {
            std::lock_guard<std::mutex> sk(g_switchesMutex);
            for (const auto& s : g_switches)
                if (s.index == switchIndex) { switchChannel = s.channel; break; }
        }
        bool isLoaded = isActiveSensingAction
            ? g_activeSensingEnabledState.load()
            : (isSwitchAction ? switchIsOn : (organIndex == loaded && loaded > 0));
        std::string svg = isActiveSensingAction
            ? BuildConsolePowerButtonSvg(title, isLoaded)
            : (isSwitchAction
                ? BuildSwitchButtonSvg(title, isLoaded, switchChannel)
                : BuildButtonSvg(title, isLoaded, isLoaded && loadedUsesVstLink, consoleConnected, hauptwerkRunning));
        SdSetImage(context, svg, 0);
        SdSetImage(context, svg, 1);
        SdSetTitle(context, "", 0);
        SdSetTitle(context, "", 1);
        SdSetState(context, isLoaded ? 1 : 0);
        // Seed the cache so UpdateAllButtons skips redundant sends for this button
        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            auto it = g_visibleButtons.find(context);
            if (it != g_visibleButtons.end())
            {
                it->second.lastSvg = svg;
                it->second.lastState = isLoaded ? 1 : 0;
            }
        }

        // Schedule delayed refresh to handle bulk willAppear during profile load
        g_lastWillAppearTick = GetTickCount64();
        if (!g_refreshPending.exchange(true))
        {
            std::thread([]() {
                while (true) {
                    Sleep(800);
                    ULONGLONG elapsed = GetTickCount64() - g_lastWillAppearTick;
                    if (elapsed >= 800) break;
                }
                g_refreshPending = false;
                Log("[SD] Delayed refresh after willAppear burst");
                UpdateAllButtons();
            }).detach();
        }
    }
    else if (event == "willDisappear")
    {
        Log("[SD] willDisappear: context=%.40s", context.c_str());
        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            g_visibleButtons.erase(context);
        }
        {
            std::lock_guard<std::mutex> kl(g_keyDownMutex);
            g_keyDownTickByContext.erase(context);
        }
    }
    else if (event == "keyDown")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string settings = JsonExtractObject(payload, "settings");
        int organIndex = JsonExtractInt(settings, "organIndex");

        int loaded;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
        }

        Log("[SD] keyDown: organIndex=%d loadedOrganIndex=%d", organIndex, loaded);

        if (action == "com.ahlbornbridge.switch")
        {
            SwitchEntry selected{};
            bool found = false;
            int switchIndex = JsonExtractInt(settings, "switchIndex");
            {
                std::lock_guard<std::mutex> sk(g_switchesMutex);
                for (const auto& s : g_switches)
                {
                    if (s.index == switchIndex)
                    {
                        selected = s;
                        found = true;
                        break;
                    }
                }
            }
            bool momentaryFromSettings = (JsonExtractInt(settings, "momentary") != 0) ||
                                           (JsonExtractString(settings, "momentary") == "true");
            Log("[SD] keyDown switch: switchIndex=%d found=%d momentary=%d momentarySettings=%d loadedNow=%d", switchIndex, (int)found, found ? (int)selected.momentary : -1, (int)momentaryFromSettings, loaded);
            bool isMomentary = (found && selected.momentary) || momentaryFromSettings;
            if (isMomentary)
            {
                // Momentary: keyDown invia ON, keyUp invia OFF (nessun controllo organo caricato)
                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    auto it = g_visibleButtons.find(context);
                    if (it != g_visibleButtons.end())
                    {
                        it->second.switchIsOn = true;
                        if (it->second.organName.empty())
                            it->second.organName = selected.name;
                    }
                }
                {
                    std::lock_guard<std::mutex> sk(g_switchStateMutex);
                    g_currentSwitchStateByIndex[selected.index] = true;
                }
                if (found)
                {
                    PipeSend("{\"type\":\"switchCc\",\"channel\":" + std::to_string(selected.channel) +
                             ",\"cc\":" + std::to_string(selected.cc) + ",\"value\":" + std::to_string(selected.valueOn) + "}");
                }
                UpdateButtonForContext(context, organIndex);
                return;
            }

            int loadedNow = 0;
            {
                std::lock_guard<std::mutex> sl(g_stateMutex);
                loadedNow = g_loadedOrganIndex;
            }
            if (loadedNow <= 0)
            {
                Log("[SD] keyDown: switch ignored because no organ is loaded");
                UpdateButtonForContext(context, organIndex);
                return;
            }
            if (found)
            {
                // Toggle: ogni pressione inverte lo stato
                bool currentOn = false;
                {
                    std::lock_guard<std::mutex> sk(g_switchStateMutex);
                    auto it = g_currentSwitchStateByIndex.find(selected.index);
                    if (it != g_currentSwitchStateByIndex.end())
                        currentOn = it->second;
                }
                bool nextOn = !currentOn;
                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    auto it = g_visibleButtons.find(context);
                    if (it != g_visibleButtons.end())
                    {
                        it->second.switchIsOn = nextOn;
                        if (it->second.organName.empty())
                            it->second.organName = selected.name;
                    }
                }
                {
                    std::lock_guard<std::mutex> sk(g_switchStateMutex);
                    g_currentSwitchStateByIndex[selected.index] = nextOn;
                }
                int value = nextOn ? selected.valueOn : selected.valueOff;
                PipeSend("{\"type\":\"switchCc\",\"channel\":" + std::to_string(selected.channel) +
                         ",\"cc\":" + std::to_string(selected.cc) + ",\"value\":" + std::to_string(value) + "}");
                std::string settingsJson = "{\"switchIndex\":" + std::to_string(selected.index) +
                    + ",\"switchName\":\"" + EscapeJson(selected.name) + "\""
                    + ",\"switchIsOn\":false}";
                SdSetSettings(context, settingsJson);
                UpdateButtonForContext(context, organIndex);
            }
            else
            {
                Log("[SD] keyDown: switchIndex not found for action");
            }
        }
        else if (action == "com.ahlbornbridge.activesensing")
        {
            bool currentlyOn = g_activeSensingEnabledState.load();
            if (!currentlyOn)
            {
                // OFF → ON: pressione immediata
                g_activeSensingEnabledState.store(true);
                PipeSend("{\"type\":\"toggleActiveSensing\"}");
                UpdateButtonForContext(context, 0);
                Log("[SD] keyDown: activeSensing ON (immediate)");
            }
            else
            {
                // ON → OFF: richiede hold 1 secondo
                ULONGLONG downTick = GetTickCount64();
                {
                    std::lock_guard<std::mutex> kl(g_keyDownMutex);
                    g_keyDownTickByContext[context] = { downTick, -1, false };
                }
                std::string holdCtx = context;
                std::thread([holdCtx, downTick]() {
                    Sleep(1000);
                    bool shouldTrigger = false;
                    {
                        std::lock_guard<std::mutex> kl(g_keyDownMutex);
                        auto it = g_keyDownTickByContext.find(holdCtx);
                        if (it != g_keyDownTickByContext.end()
                            && it->second.downTick == downTick
                            && !it->second.longTriggered)
                        {
                            it->second.longTriggered = true;
                            shouldTrigger = true;
                        }
                    }
                    if (!shouldTrigger) return;
                    Log("[SD] hold 1000ms: activeSensing OFF");
                    g_activeSensingEnabledState.store(false);
                    PipeSend("{\"type\":\"toggleActiveSensing\"}");
                    UpdateButtonForContext(holdCtx, 0);
                }).detach();
            }
        }
        else if (organIndex > 0 && organIndex == loaded)
        {
            ULONGLONG downTick = GetTickCount64();
            {
                std::lock_guard<std::mutex> kl(g_keyDownMutex);
                g_keyDownTickByContext[context] = { downTick, organIndex, false };
            }
            Log("[SD] keyDown: loaded organ button hold tracking started (ctx=%.40s)", context.c_str());

            std::string holdContext = context;
            std::thread([holdContext, organIndex, downTick]() {
                Sleep(2000);

                bool shouldTrigger = false;
                {
                    std::lock_guard<std::mutex> kl(g_keyDownMutex);
                    auto it = g_keyDownTickByContext.find(holdContext);
                    if (it != g_keyDownTickByContext.end()
                        && it->second.downTick == downTick
                        && it->second.organIndex == organIndex
                        && !it->second.longTriggered)
                    {
                        it->second.longTriggered = true;
                        shouldTrigger = true;
                    }
                }

                if (!shouldTrigger)
                    return;

                int loadedNow;
                bool loadedUsesVstLinkNow;
                {
                    std::lock_guard<std::mutex> sl(g_stateMutex);
                    loadedNow = g_loadedOrganIndex;
                    loadedUsesVstLinkNow = g_loadedOrganUsesVstLink;
                }

                if (organIndex <= 0 || organIndex != loadedNow || !loadedUsesVstLinkNow)
                {
                    Log("[SD] hold threshold reached, action ignored (organIndex=%d loaded=%d vst=%d)",
                        organIndex, loadedNow, (int)loadedUsesVstLinkNow);
                    return;
                }

                Log("[SD] hold threshold reached (2000 ms) on loaded organ %d -> toggle Bidule window", organIndex);
                PipeSend("{\"type\":\"toggleBiduleWindow\"}");
            }).detach();
        }
        else if (organIndex > 0)
        {
            // Not loaded → request load and wait for loading dialog visibility
            Log("[SD] keyDown: loading organ %d", organIndex);
            StartOrganRequest(organIndex);
            PipeSend("{\"type\":\"load\",\"index\":" + std::to_string(organIndex) + "}");
        }
        else
        {
            Log("[SD] keyDown: organIndex=0, no action (button not configured)");
        }
    }
    else if (event == "keyUp")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string settings = JsonExtractObject(payload, "settings");
        int organIndex = JsonExtractInt(settings, "organIndex");

        // Gestione momentary per switch
        if (action == "com.ahlbornbridge.switch")
        {
            int switchIndex = JsonExtractInt(settings, "switchIndex");
            SwitchEntry selected{};
            bool found = false;
            {
                std::lock_guard<std::mutex> sk(g_switchesMutex);
                for (const auto& s : g_switches)
                {
                    if (s.index == switchIndex)
                    {
                        selected = s;
                        found = true;
                        break;
                    }
                }
            }
            bool momentaryFromSettings = (JsonExtractInt(settings, "momentary") != 0) ||
                                           (JsonExtractString(settings, "momentary") == "true");
            Log("[SD] keyUp switch: switchIndex=%d found=%d momentary=%d momentarySettings=%d", switchIndex, (int)found, found ? (int)selected.momentary : -1, (int)momentaryFromSettings);
            bool isMomentary = (found && selected.momentary) || momentaryFromSettings;
            if (isMomentary)
            {
                {
                    std::lock_guard<std::mutex> lk(g_buttonsMutex);
                    auto it = g_visibleButtons.find(context);
                    if (it != g_visibleButtons.end())
                        it->second.switchIsOn = false;
                }
                {
                    std::lock_guard<std::mutex> sk(g_switchStateMutex);
                    g_currentSwitchStateByIndex[selected.index] = false;
                }
                if (found)
                {
                    PipeSend("{\"type\":\"switchCc\",\"channel\":" + std::to_string(selected.channel) +
                             ",\"cc\":" + std::to_string(selected.cc) + ",\"value\":" + std::to_string(selected.valueOff) + "}");
                }
                UpdateButtonForContext(context, organIndex);
            }
            return;
        }

        KeyHoldInfo hold;
        bool hasHold = false;
        {
            std::lock_guard<std::mutex> kl(g_keyDownMutex);
            auto it = g_keyDownTickByContext.find(context);
            if (it != g_keyDownTickByContext.end())
            {
                hold = it->second;
                g_keyDownTickByContext.erase(it);
                hasHold = true;
            }
        }

        if (!hasHold)
            return;

        ULONGLONG heldMs = GetTickCount64() - hold.downTick;

        if (hold.longTriggered)
        {
            Log("[SD] keyUp: hold action already triggered at threshold (held=%llu ms)", (unsigned long long)heldMs);
            return;
        }

        int loaded;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
        }

        if (organIndex <= 0 || organIndex != loaded)
        {
            Log("[SD] keyUp: ignoring short press action, organ changed (organIndex=%d loaded=%d held=%llu)",
                organIndex, loaded, (unsigned long long)heldMs);
            return;
        }

        // Short press on loaded organ keeps existing behavior
        Log("[SD] keyUp: short press (%llu ms) on loaded organ %d -> unload", (unsigned long long)heldMs, organIndex);
        g_freezeSwitchStateDuringUnload.store(true);
        PipeSend("{\"type\":\"unload\"}");
    }
    else if (event == "propertyInspectorDidAppear")
    {
        Log("[SD] propertyInspectorDidAppear: context=%.40s action=%s",
            context.c_str(), action.c_str());
        if (action == "com.ahlbornbridge.switch")
        {
            if (g_pipeConnected.load())
                PipeSend("{\"type\":\"getSwitches\"}");
            SdSendToPropertyInspector(context, action, BuildSwitchesPayload());
        }
        else if (action == "com.ahlbornbridge.activesensing")
        {
            SdSendToPropertyInspector(context, action, "{\"activeSensing\":true}");
        }
        else
        {
            SdSendToPropertyInspector(context, action, BuildOrgansPayload());
        }
    }
    else if (event == "propertyInspectorDidDisappear")
    {
        Log("[SD] propertyInspectorDidDisappear: context=%.40s", context.c_str());
        // PI closed — fetch settings in case PI saved changes via setSettings
        // SD doesn't relay didReceiveSettings from PI, so we request explicitly
        SdGetSettings(context);
    }
    else if (event == "sendToPlugin")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string piEvent = JsonExtractString(payload, "piEvent");
        Log("[SD] sendToPlugin: piEvent=%s context=%.40s payload=%.200s",
            piEvent.c_str(), context.c_str(), payload.c_str());

        if (piEvent == "requestOrgans")
        {
            SdSendToPropertyInspector(context, action, BuildOrgansPayload());
        }
        else if (piEvent == "requestSwitches")
        {
            if (g_pipeConnected.load())
                PipeSend("{\"type\":\"getSwitches\"}");
            SdSendToPropertyInspector(context, action, BuildSwitchesPayload());
        }
        else if (piEvent == "setOrganIndex")
        {
            int organIndex = JsonExtractInt(payload, "organIndex");
            Log("[SD] sendToPlugin/setOrganIndex: organIndex=%d context=%.40s",
                organIndex, context.c_str());

            // Look up organ name to persist in settings
            std::string title;
            {
                std::lock_guard<std::mutex> ol(g_organsMutex);
                for (auto& o : g_organs)
                {
                    if (o.index == organIndex)
                    {
                        title = o.name;
                        break;
                    }
                }
            }
            if (title.empty())
                title = (organIndex == 0) ? "Empty" : "ORGAN " + std::to_string(organIndex);

            // Persist settings with organName so it survives plugin restart
            std::string nameEsc = EscapeJson(title);
            std::string settingsJson = "{\"organIndex\":" + std::to_string(organIndex)
                + ",\"organName\":\"" + nameEsc + "\"}";
            SdSetSettings(context, settingsJson);

            // Update internal state
            {
                std::lock_guard<std::mutex> lk(g_buttonsMutex);
                auto it = g_visibleButtons.find(context);
                if (it != g_visibleButtons.end())
                {
                    it->second.organIndex = organIndex;
                    it->second.organName = title;
                }
            }

            int loaded;
            bool loadedUsesVstLink;
            bool consoleConnected;
            bool hauptwerkRunning;
            {
                std::lock_guard<std::mutex> sl(g_stateMutex);
                loaded = g_loadedOrganIndex;
                loadedUsesVstLink = g_loadedOrganUsesVstLink;
                consoleConnected = g_consoleConnected;
                hauptwerkRunning = g_hauptwerkRunning;
            }
            bool isLoaded = (organIndex == loaded && loaded > 0);
            std::string svg = BuildButtonSvg(title, isLoaded, isLoaded && loadedUsesVstLink, consoleConnected, hauptwerkRunning);
            SdSetImage(context, svg, 0);
            SdSetImage(context, svg, 1);
            SdSetTitle(context, "", 0);
            SdSetTitle(context, "", 1);
            SdSetState(context, isLoaded ? 1 : 0);
        }
        else if (piEvent == "setSwitchIndex")
        {
            int switchIndex = JsonExtractInt(payload, "switchIndex");
            std::string switchName = JsonExtractString(payload, "switchName");
            bool isMomentary = false;
            {
                std::lock_guard<std::mutex> sk(g_switchesMutex);
                for (const auto& s : g_switches)
                {
                    if (s.index == switchIndex)
                    {
                        if (switchName.empty())
                            switchName = s.name;
                        isMomentary = s.momentary;
                        break;
                    }
                }
            }
            std::string settingsJson = "{\"switchIndex\":" + std::to_string(switchIndex)
                + ",\"switchName\":\"" + EscapeJson(switchName) + "\""
                + ",\"switchIsOn\":false"
                + ",\"momentary\":" + (isMomentary ? "true" : "false") + "}";
            SdSetSettings(context, settingsJson);
            {
                std::lock_guard<std::mutex> lk(g_buttonsMutex);
                auto it = g_visibleButtons.find(context);
                if (it != g_visibleButtons.end())
                {
                    it->second.switchIndex = switchIndex;
                    it->second.organName = switchName;
                    it->second.switchIsOn = false;
                }
            }
            UpdateButtonForContext(context, 0);
        }
    }
    else if (event == "didReceiveSettings")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string settings = JsonExtractObject(payload, "settings");
        int organIndex = JsonExtractInt(settings, "organIndex");
        int switchIndex = JsonExtractInt(settings, "switchIndex");
        bool switchIsOn = false;
        {
            std::lock_guard<std::mutex> sk(g_switchStateMutex);
            auto sit = g_currentSwitchStateByIndex.find(switchIndex);
            if (sit != g_currentSwitchStateByIndex.end())
                switchIsOn = sit->second;
        }
        std::string organName = JsonExtractString(settings, "organName");
        std::string switchName = JsonExtractString(settings, "switchName");

        Log("[SD] didReceiveSettings: context=%.40s organIndex=%d switchIndex=%d organName=%s switchName=%s",
            context.c_str(), organIndex, switchIndex, organName.c_str(), switchName.c_str());

        std::string actionName;
        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            auto it = g_visibleButtons.find(context);
            if (it != g_visibleButtons.end())
            {
                actionName = it->second.action;
                it->second.organIndex = organIndex;
                it->second.switchIndex = switchIndex;
                it->second.switchIsOn = switchIsOn;
                if (!organName.empty())
                    it->second.organName = organName;
                if (!switchName.empty())
                    it->second.organName = switchName;
            }
        }

        std::string title = (actionName == "com.ahlbornbridge.activesensing") ? ResolveActiveSensingTitle()
                           : ((actionName == "com.ahlbornbridge.switch") ? ResolveSwitchTitle(switchIndex, switchName)
                                                                            : ResolveButtonTitle(organIndex, organName));

        int loaded;
        bool loadedUsesVstLink;
        bool consoleConnected;
        bool hauptwerkRunning;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
            loadedUsesVstLink = g_loadedOrganUsesVstLink;
            consoleConnected = g_consoleConnected;
            hauptwerkRunning = g_hauptwerkRunning;
        }
        bool isSwitchAction = (actionName == "com.ahlbornbridge.switch");
        bool isActiveSensingAction = (actionName == "com.ahlbornbridge.activesensing");
        int switchChannel = 0;
        if (isSwitchAction)
        {
            std::lock_guard<std::mutex> sk(g_switchesMutex);
            for (const auto& s : g_switches)
                if (s.index == switchIndex) { switchChannel = s.channel; break; }
        }
        bool isLoaded = isActiveSensingAction
            ? g_activeSensingEnabledState.load()
            : (isSwitchAction ? switchIsOn : (organIndex == loaded && loaded > 0));
        std::string svg = isActiveSensingAction
            ? BuildConsolePowerButtonSvg(title, isLoaded)
            : (isSwitchAction
                ? BuildSwitchButtonSvg(title, isLoaded, switchChannel)
                : BuildButtonSvg(title, isLoaded, isLoaded && loadedUsesVstLink, consoleConnected, hauptwerkRunning));
        SdSetImage(context, svg, 0);
        SdSetImage(context, svg, 1);
        SdSetTitle(context, "", 0);
        SdSetTitle(context, "", 1);
        SdSetState(context, isLoaded ? 1 : 0);
    }
}

// ─── WebSocket connection ───────────────────────────────────────────────

static bool ConnectWebSocket()
{
    g_hSession = WinHttpOpen(L"AhlbornBridgeSD/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession)
    {
        Log("[WS] WinHttpOpen failed: %lu", GetLastError());
        return false;
    }

    g_hConnect = WinHttpConnect(g_hSession, L"127.0.0.1", (INTERNET_PORT)g_port, 0);
    if (!g_hConnect)
    {
        Log("[WS] WinHttpConnect failed: %lu", GetLastError());
        return false;
    }

    g_hRequest = WinHttpOpenRequest(g_hConnect, L"GET", L"/",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!g_hRequest)
    {
        Log("[WS] WinHttpOpenRequest failed: %lu", GetLastError());
        return false;
    }

    // Request WebSocket upgrade
    BOOL bResult = WinHttpSetOption(g_hRequest,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!bResult)
    {
        Log("[WS] SetOption failed: %lu", GetLastError());
        return false;
    }

    bResult = WinHttpSendRequest(g_hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (!bResult)
    {
        Log("[WS] SendRequest failed: %lu", GetLastError());
        return false;
    }

    bResult = WinHttpReceiveResponse(g_hRequest, nullptr);
    if (!bResult)
    {
        Log("[WS] ReceiveResponse failed: %lu", GetLastError());
        return false;
    }

    g_hWebSocket = WinHttpWebSocketCompleteUpgrade(g_hRequest, 0);
    if (!g_hWebSocket)
    {
        Log("[WS] CompleteUpgrade failed: %lu", GetLastError());
        return false;
    }

    // Close the request handle - no longer needed after upgrade
    WinHttpCloseHandle(g_hRequest);
    g_hRequest = nullptr;

    Log("[WS] WebSocket connected to port %d", g_port);
    return true;
}

// ─── Command-line parsing ───────────────────────────────────────────────

static bool ParseArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-port" || arg == "--port")
        {
            if (++i < argc) g_port = atoi(argv[i]);
        }
        else if (arg == "-pluginUUID" || arg == "--pluginUUID")
        {
            if (++i < argc) g_pluginUUID = argv[i];
        }
        else if (arg == "-registerEvent" || arg == "--registerEvent")
        {
            if (++i < argc) g_registerEvent = argv[i];
        }
        else if (arg == "-info" || arg == "--info")
        {
            if (++i < argc) { /* info JSON — not needed currently */ }
        }
    }

    if (g_port == 0 || g_pluginUUID.empty() || g_registerEvent.empty())
    {
        Log("Usage: AhlbornBridgeSD.exe -port <port> -pluginUUID <uuid> -registerEvent <event> -info <json>");
        return false;
    }
    return true;
}

// ─── Main ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    LogInit();
    Log("======== AhlbornBridgeSD plugin starting ========");

    if (!ParseArgs(argc, argv))
        return 1;

    // Load background images (optional PNG files in images/ folder)
    LoadBackgroundImages();

    Log("[Main] port=%d pluginUUID=%s registerEvent=%s",
        g_port, g_pluginUUID.c_str(), g_registerEvent.c_str());

    // Connect WebSocket to Stream Deck
    if (!ConnectWebSocket())
    {
        Log("[Main] Failed to connect WebSocket");
        return 1;
    }

    // Register with Stream Deck
    std::string regMsg = "{\"event\":\"" + g_registerEvent +
        "\",\"uuid\":\"" + g_pluginUUID + "\"}";
    if (!WsSend(regMsg))
    {
        Log("[Main] Failed to send registration");
        return 1;
    }
    Log("[Main] Registered with Stream Deck");

    // Start pipe reader thread (connects to AhlbornBridge)
    std::thread pipeThread(PipeReaderThread);

    // Start HTTP listener for direct PI → Plugin communication
    std::thread httpThread(HttpListenerThread);

    // WebSocket receive loop (main thread)
    char wsBuffer[65536];
    std::string wsAccum;  // accumulate fragments
    while (g_running)
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

        // NOTE: No mutex here — WinHTTP supports concurrent send/receive
        // on the same WebSocket handle. Locking here would deadlock with
        // WsSend() called from the pipe reader thread.
        DWORD err = WinHttpWebSocketReceive(g_hWebSocket,
            wsBuffer, sizeof(wsBuffer) - 1, &bytesRead, &bufferType);

        if (err != NO_ERROR)
        {
            Log("[WS] Receive failed: %lu", err);
            g_running = false;
            break;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            Log("[WS] Server closed connection");
            g_running = false;
            break;
        }

        // Handle WebSocket fragments
        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
        {
            wsAccum.append(wsBuffer, bytesRead);
            Log("[WS] Fragment received (%lu bytes, total=%d)", bytesRead, (int)wsAccum.size());
            continue;
        }

        // Complete message (possibly preceded by fragments)
        std::string json;
        if (!wsAccum.empty())
        {
            wsAccum.append(wsBuffer, bytesRead);
            json = std::move(wsAccum);
            wsAccum.clear();
            Log("[WS] Recv assembled (%d bytes): %.300s", (int)json.size(), json.c_str());
        }
        else
        {
            json.assign(wsBuffer, bytesRead);
            Log("[WS] Recv (%d bytes): %.300s", (int)json.size(), json.c_str());
        }

        HandleSdEvent(json);
    }

    // Cleanup
    Log("[Main] Shutting down...");
    g_running = false;

    // Close pipe to unblock reader thread
    {
        std::lock_guard<std::mutex> lk(g_pipeMutex);
        if (g_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
    }

    // Close HTTP listener to unblock accept
    if (g_httpListener != INVALID_SOCKET)
    {
        closesocket(g_httpListener);
        g_httpListener = INVALID_SOCKET;
    }

    if (pipeThread.joinable())
        pipeThread.join();
    if (httpThread.joinable())
        httpThread.join();

    {
        std::lock_guard<std::mutex> lk(g_wsMutex);
        if (g_hWebSocket)
        {
            WinHttpWebSocketClose(g_hWebSocket,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(g_hWebSocket);
        }
    }
    if (g_hConnect) WinHttpCloseHandle(g_hConnect);
    if (g_hSession) WinHttpCloseHandle(g_hSession);

    Log("[Main] Plugin stopped");
    if (g_logFile) fclose(g_logFile);
    return 0;
}
