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
    if (g_logFile) setvbuf(g_logFile, NULL, _IONBF, 0);
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

// Track visible buttons: context string -> organIndex (from settings)
struct ButtonInfo
{
    std::string context;
    int organIndex = 0;
    std::string organName; // cached from settings, used before pipe connects
};
static std::mutex g_buttonsMutex;
static std::map<std::string, ButtonInfo> g_visibleButtons; // context -> info

// Current organ state
static std::mutex g_stateMutex;
static int g_loadedOrganIndex = 0;

// Organ list from AhlbornBridge
struct OrganEntry
{
    int index;
    std::string name;
};
static std::mutex g_organsMutex;
static std::vector<OrganEntry> g_organs;

// Background images (base64-encoded PNG data, loaded at startup)
static std::string g_bgImageOff;
static std::string g_bgImageOn;

// Delayed refresh after willAppear burst (profile load)
static std::atomic<ULONGLONG> g_lastWillAppearTick{0};
static std::atomic<bool> g_refreshPending{false};

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

// Forward declaration (defined below)
static std::string Base64Encode(const std::string& input);

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

// Build an SVG image for a button with improved visual states and title layout
static std::string BuildButtonSvg(const std::string& title, bool loaded)
{
    const bool isEmpty = (title == "Empty");
    const bool isOffline = (title == "Disconnected.");
    const bool isReady = !loaded && !isEmpty && !isOffline;

    const std::string& bgImage = loaded ? g_bgImageOn : g_bgImageOff;
    std::string frameColor = loaded ? "#39d353" : isOffline ? "#ffb347" : isEmpty ? "#7d8590" : "#58a6ff";
    std::string badgeText = loaded ? "LOADED" : isOffline ? "OFFLINE" : isEmpty ? "EMPTY" : "READY";
    std::string badgeFill = loaded ? "#123c1f" : isOffline ? "#5c3b00" : isEmpty ? "#30363d" : "#0d2d4f";
    std::string titleColor = loaded ? "#eaffea" : isOffline ? "#fff4db" : isEmpty ? "#e6edf3" : "#ffffff";
    std::string shadowColor = loaded ? "#0b1f10" : "#111111";

    std::string svg = "<svg width='144' height='144' xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>";
    svg += "<rect width='144' height='144' rx='14' fill='#0f1115'/>";

    if (!bgImage.empty() && !isOffline && !isEmpty)
    {
        svg += "<image width='144' height='144' preserveAspectRatio='xMidYMid slice' href='data:image/png;base64," + bgImage + "' xlink:href='data:image/png;base64," + bgImage + "' opacity='0.96'/>";
        svg += "<rect width='144' height='144' rx='14' fill='#000000' fill-opacity='0.18'/>";
    }
    else
    {
        std::string bg = loaded ? "#16321d" : isOffline ? "#3a2714" : isEmpty ? "#20262d" : "#162235";
        svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='" + bg + "'/>";
    }

    svg += "<rect x='4' y='4' width='136' height='136' rx='12' fill='none' stroke='" + frameColor + "' stroke-width='3'/>";
    svg += "<rect x='14' y='12' width='116' height='24' rx='8' fill='" + badgeFill + "' stroke='" + frameColor + "' stroke-width='1.5'/>";
    svg += "<text x='72' y='29' font-family='Arial' font-size='12' fill='" + frameColor + "' text-anchor='middle' font-weight='bold' letter-spacing='1'>" + badgeText + "</text>";

    auto lines = WrapTitleLines(title, 12, 4);
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

    if (isReady)
    {
        svg += "<circle cx='123' cy='122' r='7' fill='#58a6ff' stroke='#dbeafe' stroke-width='1.5'/>";
    }
    else if (loaded)
    {
        svg += "<circle cx='123' cy='122' r='7' fill='#39d353' stroke='#dcffe4' stroke-width='1.5'/>";
    }
    else if (isOffline)
    {
        svg += "<circle cx='123' cy='122' r='7' fill='#ffb347' stroke='#fff1d6' stroke-width='1.5'/>";
    }
    else
    {
        svg += "<circle cx='123' cy='122' r='7' fill='#7d8590' stroke='#e6edf3' stroke-width='1.5'/>";
    }

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

static std::string ResolveButtonTitle(int organIndex, const std::string& cachedOrganName)
{
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
    if (organIndex == 0)
        return "Empty";
    if (!g_pipeConnected.load())
        return "Disconnected.";
    return "ORGAN " + std::to_string(organIndex);
}

// ─── Update all visible buttons ─────────────────────────────────────────

static void UpdateAllButtons()
{
    std::lock_guard<std::mutex> lk(g_buttonsMutex);
    int loaded = 0;
    {
        std::lock_guard<std::mutex> sl(g_stateMutex);
        loaded = g_loadedOrganIndex;
    }

    Log("[UpdateAllButtons] visibleButtons=%d  loadedOrganIndex=%d",
        (int)g_visibleButtons.size(), loaded);

    for (auto& [ctx, info] : g_visibleButtons)
    {
        std::string title = ResolveButtonTitle(info.organIndex, info.organName);

        // If we resolved a better name than what's cached, persist it to settings
        if (title != info.organName && title != "Empty" && title != "Disconnected."
            && title.rfind("ORGAN ", 0) != 0)
        {
            info.organName = title;
            std::string settingsJson = "{\"organIndex\":" + std::to_string(info.organIndex)
                + ",\"organName\":\"" + EscapeJson(title) + "\"}";
            SdSetSettings(ctx, settingsJson);
        }

        int st = (info.organIndex == loaded && loaded > 0) ? 1 : 0;
        Log("  button ctx=%.30s organIndex=%d title=%s state=%d",
            ctx.c_str(), info.organIndex, title.c_str(), st);
        std::string svg = BuildButtonSvg(title, st == 1);
        SdSetImage(ctx, svg, 0);
        SdSetImage(ctx, svg, 1);
        SdSetTitle(ctx, "", 0);
        SdSetTitle(ctx, "", 1);
        SdSetState(ctx, st);
    }
}

// ─── Update a single button by context

static void UpdateButtonForContext(const std::string& context, int organIndex)
{
    std::string cachedName;
    {
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        auto it = g_visibleButtons.find(context);
        if (it != g_visibleButtons.end())
            cachedName = it->second.organName;
    }
    std::string title = ResolveButtonTitle(organIndex, cachedName);

    int loaded;
    {
        std::lock_guard<std::mutex> sl(g_stateMutex);
        loaded = g_loadedOrganIndex;
    }
    bool isLoaded = (organIndex == loaded && loaded > 0);
    std::string svg = BuildButtonSvg(title, isLoaded);
    SdSetImage(context, svg, 0);
    SdSetImage(context, svg, 1);
    SdSetTitle(context, "", 0);
    SdSetTitle(context, "", 1);
    SdSetState(context, isLoaded ? 1 : 0);
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

static void HandlePipeMessage(const std::string& msg)
{
    Log("[Pipe] HandlePipeMessage: %.200s", msg.c_str());
    if (msg.find("\"organs\"") != std::string::npos && msg.find("\"list\"") != std::string::npos)
    {
        ParseOrganList(msg);
    }
    else if (msg.find("\"state\"") != std::string::npos)
    {
        int loaded = JsonExtractInt(msg, "loadedIndex");
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            g_loadedOrganIndex = loaded;
        }
        Log("[Pipe] State update: loadedIndex=%d", loaded);
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
            Log("[Pipe] Requesting initial organ list and state");
            PipeSend("{\"type\":\"getOrgans\"}");
            PipeSend("{\"type\":\"getState\"}");
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
            int organIndex = atoi(organStr.c_str());

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

        Log("[SD] willAppear: context=%.40s organIndex=%d organName=%s device=%.60s action=%s",
            context.c_str(), organIndex, organName.c_str(), device.c_str(), action.c_str());

        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            g_visibleButtons[context] = { context, organIndex, organName };
        }

        std::string title = ResolveButtonTitle(organIndex, organName);

        int loaded;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
        }
        bool isLoaded = (organIndex == loaded && loaded > 0);
        std::string svg = BuildButtonSvg(title, isLoaded);
        SdSetImage(context, svg, 0);
        SdSetImage(context, svg, 1);
        SdSetTitle(context, "", 0);
        SdSetTitle(context, "", 1);
        SdSetState(context, isLoaded ? 1 : 0);

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
        std::lock_guard<std::mutex> lk(g_buttonsMutex);
        g_visibleButtons.erase(context);
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

        if (organIndex > 0 && organIndex == loaded)
        {
            // This organ is currently loaded → unload
            Log("[SD] keyDown: unloading organ %d", organIndex);
            PipeSend("{\"type\":\"unload\"}");
        }
        else if (organIndex > 0)
        {
            // Not loaded → load this organ
            Log("[SD] keyDown: loading organ %d", organIndex);
            PipeSend("{\"type\":\"load\",\"index\":" + std::to_string(organIndex) + "}");
        }
        else
        {
            Log("[SD] keyDown: organIndex=0, no action (button not configured)");
        }
    }
    else if (event == "propertyInspectorDidAppear")
    {
        Log("[SD] propertyInspectorDidAppear: context=%.40s action=%s",
            context.c_str(), action.c_str());
        SdSendToPropertyInspector(context, action, BuildOrgansPayload());
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
            {
                std::lock_guard<std::mutex> sl(g_stateMutex);
                loaded = g_loadedOrganIndex;
            }
            bool isLoaded = (organIndex == loaded && loaded > 0);
            std::string svg = BuildButtonSvg(title, isLoaded);
            SdSetImage(context, svg, 0);
            SdSetImage(context, svg, 1);
            SdSetTitle(context, "", 0);
            SdSetTitle(context, "", 1);
            SdSetState(context, isLoaded ? 1 : 0);
        }
    }
    else if (event == "didReceiveSettings")
    {
        std::string payload = JsonExtractObject(json, "payload");
        std::string settings = JsonExtractObject(payload, "settings");
        int organIndex = JsonExtractInt(settings, "organIndex");
        std::string organName = JsonExtractString(settings, "organName");

        Log("[SD] didReceiveSettings: context=%.40s organIndex=%d organName=%s",
            context.c_str(), organIndex, organName.c_str());

        {
            std::lock_guard<std::mutex> lk(g_buttonsMutex);
            auto it = g_visibleButtons.find(context);
            if (it != g_visibleButtons.end())
            {
                it->second.organIndex = organIndex;
                if (!organName.empty())
                    it->second.organName = organName;
            }
        }

        std::string title = ResolveButtonTitle(organIndex, organName);

        int loaded;
        {
            std::lock_guard<std::mutex> sl(g_stateMutex);
            loaded = g_loadedOrganIndex;
        }
        bool isLoaded = (organIndex == loaded && loaded > 0);
        std::string svg = BuildButtonSvg(title, isLoaded);
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
