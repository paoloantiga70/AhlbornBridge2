#include "StreamDeck.h"
#include "Midi.h"
#include "Xml.h"
#include "Hauptwerk.h"
#include "StreamDeck_profiler.h"
#include <algorithm>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>

void SendUnloadOrganMidiMessage(bool resetSwitches)
{
    // BF 50 00 = CC ch16, CC#80, value 0
    DWORD msg50 = CC_ch16 | (BF_0x50 << 8) | (0x00 << 16);
    // BF 51 00 = CC ch16, CC#81, value 0
    DWORD msg51 = CC_ch16 | (BF_0x51 << 8) | (0x00 << 16);

    bool ok50 = EnqueueMidiOutMessage(msg50);
    bool ok51 = EnqueueMidiOutMessage(msg51);

    printf("\nSendUnloadOrganMidiMessage: BF 50 00 %s, BF 51 00 %s.\n\n",
        ok50 ? "enqueued" : "FAILED",
        ok51 ? "enqueued" : "FAILED");

    NotifyStreamDeckOrganUnloaded(resetSwitches);
}

// ─── Named pipe server for Stream Deck plugin IPC ───────────────────────

static const wchar_t* kPipeName = L"\\\\.\\pipe\\AhlbornBridge";
static HANDLE g_pipeServerThread = nullptr;
static HANDLE g_pipeStopEvent = nullptr;
static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_pipeLock;
static bool g_pipeLockInitialized = false;
static std::atomic<bool> g_pipeConnected{ false };

static void PipeSend(const std::string& json)
{
    if (!g_pipeLockInitialized) return;
    EnterCriticalSection(&g_pipeLock);
    if (g_pipeHandle != INVALID_HANDLE_VALUE && g_pipeConnected)
    {
        std::string msg = json + "\n";
        DWORD written = 0;
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = WriteFile(g_pipeHandle, msg.c_str(), (DWORD)msg.size(), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            WaitForSingleObject(ov.hEvent, 5000);
            ok = GetOverlappedResult(g_pipeHandle, &ov, &written, FALSE);
        }
        CloseHandle(ov.hEvent);
        if (!ok)
            printf("[PipeServer] PipeSend FAILED: err=%lu\n", GetLastError());
    }
    LeaveCriticalSection(&g_pipeLock);
}

static std::string EscapeJsonString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static std::string BuildOrganListJson()
{
    wchar_t* appDataPath = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath) != S_OK)
        return "{\"type\":\"organs\",\"list\":[]}";
    std::wstring settingsW = std::wstring(appDataPath) + L"\\AhlbornBridge2\\Settings.xml";
    CoTaskMemFree(appDataPath);

    std::string settingsPath(settingsW.begin(), settingsW.end());
    std::vector<OrganInfo> organs = LoadInstalledOrgans(settingsPath);

    std::string json = "{\"type\":\"organs\",\"list\":[";
    for (int i = 0; i < (int)organs.size(); ++i)
    {
        if (i > 0) json += ",";
        std::string name = organs[i].displayName.empty()
            ? ("ORGAN " + std::to_string(i + 1))
            : organs[i].displayName;
        json += "{\"index\":" + std::to_string(i + 1) +
                ",\"name\":\"" + EscapeJsonString(name) + "\"}";
    }
    json += "]}";
    return json;
}

static int JsonExtractInt(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
        return 0;
    pos += search.size();
    return atoi(json.c_str() + pos);
}

static std::string BuildSwitchesPayload()
{
    std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
    std::string json = "{\"type\":\"switches\",\"connected\":";
    json += g_pipeConnected.load() ? "true" : "false";
    json += ",\"switches\":[";
    for (size_t i = 0; i < switches.size(); ++i)
    {
        if (i > 0) json += ",";
        std::string name(switches[i].name.begin(), switches[i].name.end());
        json += "{\"index\":" + std::to_string(i + 1) +
                ",\"name\":\"" + EscapeJsonString(name) + "\"" +
                ",\"channel\":" + std::to_string(switches[i].channel) +
                ",\"cc\":" + std::to_string(switches[i].controlChange) +
                ",\"valueOn\":" + std::to_string(switches[i].valueOn) +
                ",\"valueOff\":" + std::to_string(switches[i].valueOff) +
                ",\"momentary\":" + (switches[i].momentary ? "1" : "0") + "}";
    }
    json += "]}";
    return json;
}

static bool IsVstLinkEngineForInstalledOrgan(int installedOrganIndex)
{
    if (installedOrganIndex <= 0)
        return false;

    std::vector<InstalledOrganInfo> organs = LoadInstalledOrganInfos();
    if (installedOrganIndex > static_cast<int>(organs.size()))
        return false;

    const std::wstring& outputDeviceId = organs[installedOrganIndex - 1].outputDeviceId;
    if (outputDeviceId.empty())
        return false;

    std::vector<AudioDeviceInfo> devices = LoadAudioOutputDevices();
    for (const auto& device : devices)
    {
        if (device.id == outputDeviceId)
            return _wcsicmp(device.name.c_str(), L"Hauptwerk VST Link") == 0;
    }

    return false;
}

static std::wstring ResolveCurrentLoadedOrganUniqueIdForPersistence()
{
    std::vector<InstalledOrganInfo> organs = LoadInstalledOrganInfos();
    if (organs.empty())
        return {};

    int loadedInstalledIndex = g_currentLoadedInstalledOrganIndex.load();
    if (loadedInstalledIndex > 0 && loadedInstalledIndex <= static_cast<int>(organs.size()))
    {
        const InstalledOrganInfo& info = organs[loadedInstalledIndex - 1];
        if (!info.uniqueOrganId.empty())
            return info.uniqueOrganId;
        // Fallback: use the positional id attribute when UniqueOrganID is absent
        if (!info.id.empty())
            return info.id;
    }

    if (!g_hauptwerkOrganTitle.empty())
    {
        for (const auto& organ : organs)
        {
            if ((!organ.name.empty() && _wcsicmp(organ.name.c_str(), g_hauptwerkOrganTitle.c_str()) == 0) ||
                (!organ.displayName.empty() && _wcsicmp(organ.displayName.c_str(), g_hauptwerkOrganTitle.c_str()) == 0))
            {
                if (!organ.uniqueOrganId.empty())
                    return organ.uniqueOrganId;
                if (!organ.id.empty())
                    return organ.id;
            }
        }
    }

    return {};
}

static std::string BuildStateJson(int loadedIndex)
{
    bool consoleConnected = (deviceState.load() == DeviceState::Connected);
    bool hauptwerkRunning = IsProcessRunningByName(L"Hauptwerk.exe");
    bool loadingVisible = g_isLoadingOrgan.load();

    int loadingIndex = g_streamDeckLoadingInstalledOrganIndex.load();
    int loadingProgress = std::clamp(g_streamDeckLoadingProgress.load(), 0, 100);
    bool biduleSplashActive = g_streamDeckBiduleSplashActive.load();
    if (loadingVisible && loadingIndex <= 0)
        loadingIndex = loadedIndex;

    int effectiveLoadedIndex = loadingVisible ? 0 : loadedIndex;
    bool vstLink = IsVstLinkEngineForInstalledOrgan(effectiveLoadedIndex);

    return "{\"type\":\"state\",\"loadedIndex\":" + std::to_string(effectiveLoadedIndex) +
           ",\"vstLink\":" + std::string(vstLink ? "true" : "false") +
           ",\"consoleConnected\":" + std::string(consoleConnected ? "true" : "false") +
           ",\"hauptwerkRunning\":" + std::string(hauptwerkRunning ? "true" : "false") +
           ",\"loadingIndex\":" + std::to_string(loadingIndex) +
           ",\"loadingProgress\":" + std::to_string(loadingProgress) +
           ",\"loadingVisible\":" + std::string(loadingVisible ? "true" : "false") +
           ",\"biduleSplashActive\":" + std::string(biduleSplashActive ? "true" : "false") + "}";
}

static void HandlePipeMessage(const std::string& msg)
{
    printf("[PipeServer] HandlePipeMessage: %s\n", msg.c_str());
    if (msg.find("\"getOrgans\"") != std::string::npos)
    {
        printf("[PipeServer] Client requested organ list\n");
        PipeSend(BuildOrganListJson());
    }
    else if (msg.find("\"getState\"") != std::string::npos)
    {
        printf("[PipeServer] Client requested state\n");
        int loadedIdx = g_currentLoadedInstalledOrganIndex.load();
        PipeSend(BuildStateJson(loadedIdx));
        // Also push current switch states so the plugin can mirror them immediately
        if (loadedIdx > 0)
            NotifyStreamDeckSwitchesForOrgan(loadedIdx);
    }
    else if (msg.find("\"load\"") != std::string::npos)
    {
        size_t pos = msg.find("\"index\":");
        if (pos != std::string::npos)
        {
            int idx = atoi(msg.c_str() + pos + 8);
            if (idx >= 1 && idx <= 127)
            {
                printf("[PipeServer] Load organ %d requested — calling EnqueueLoadInstalledOrgan\n", idx);
                EnqueueLoadInstalledOrgan(idx);
                // Send acknowledgement back to plugin
                PipeSend("{\"type\":\"ack\",\"command\":\"load\",\"index\":" + std::to_string(idx) + "}");
            }
            else
            {
                printf("[PipeServer] Load organ REJECTED: index %d out of range\n", idx);
            }
        }
        else
        {
            printf("[PipeServer] Load organ REJECTED: no index found in message\n");
        }
    }
    else if (msg.find("\"unload\"") != std::string::npos)
    {
        printf("[PipeServer] Unload organ requested\n");
        EnqueueUnloadOrgan();
        PipeSend("{\"type\":\"ack\",\"command\":\"unload\"}");
    }
    else if (msg.find("\"toggleBiduleWindow\"") != std::string::npos)
    {
        printf("[PipeServer] Toggle Bidule window requested\n");
        bool toggled = ToggleBiduleWindowVisibility();
        PipeSend(std::string("{\"type\":\"ack\",\"command\":\"toggleBiduleWindow\",\"ok\":") + (toggled ? "true" : "false") + "}");
    }
    else if (msg.find("\"toggleActiveSensing\"") != std::string::npos)
    {
        bool newState = !g_activeSensingEnabled.load();
        g_activeSensingEnabled.store(newState);
        SaveActiveSensingEnabled(newState);
        NotifyStreamDeckActiveSensingState(newState);
        PipeSend(std::string("{\"type\":\"ack\",\"command\":\"toggleActiveSensing\",\"enabled\":") +
            (newState ? "true" : "false") + "}");
    }
    else if (msg.find("\"getActiveSensing\"") != std::string::npos)
    {
        PipeSend(std::string("{\"type\":\"activeSensingState\",\"enabled\":") +
            (g_activeSensingEnabled.load() ? "true" : "false") + "}");
    }
    else if (msg.find("\"getSwitches\"") != std::string::npos)
    {
        printf("[PipeServer] Client requested switch list\n");
        PipeSend(BuildSwitchesPayload());
    }
    else if (msg.find("\"switchCc\"") != std::string::npos)
    {
        int channel = JsonExtractInt(msg, "channel");
        int cc = JsonExtractInt(msg, "cc");
        int value = JsonExtractInt(msg, "value");
        printf("[PipeServer] Switch CC requested: ch=%d cc=%d value=%d\n", channel, cc, value);
        SendAhlbornSwitchControlChange(channel, cc, value);

        // Fissatore press (CC 71, value 70 = ON) toggles Bidule window visibility
        if (cc == AHLBORN_FISSATORE_CC && value == AHLBORN_FISSATORE_DN)
        {
            printf("[PipeServer] Fissatore press from Stream Deck -> EnqueueToggleBidule\n");
            EnqueueToggleBidule();
        }

        std::wstring uniqueOrganId = ResolveCurrentLoadedOrganUniqueIdForPersistence();
        printf("[PipeServer] switchCc persist: loadedIndex=%d uniqueOrganId=%S\n",
            g_currentLoadedInstalledOrganIndex.load(),
            uniqueOrganId.empty() ? L"(empty)" : uniqueOrganId.c_str());
        if (!uniqueOrganId.empty())
        {
            std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
            bool matched = false;
            for (size_t i = 0; i < switches.size(); ++i)
            {
                const AhlbornSwitchInfo& s = switches[i];
                if (s.channel == channel && s.controlChange == cc &&
                    (value == s.valueOn || value == s.valueOff))
                {
                    int switchIndex = static_cast<int>(i) + 1;
                    bool isOn = (value == s.valueOn);
                    printf("[PipeServer] switchCc matched switch #%d (valueOn=%d valueOff=%d) -> isOn=%d\n",
                        switchIndex, s.valueOn, s.valueOff, (int)isOn);
                    SaveOrganSwitchState(uniqueOrganId, switchIndex, isOn);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                printf("[PipeServer] switchCc: no switch matched ch=%d cc=%d value=%d in %zu switches\n",
                    channel, cc, value, switches.size());
        }
        else
        {
            printf("[PipeServer] switchCc: uniqueOrganId empty, state NOT saved\n");
        }

        PipeSend("{\"type\":\"ack\",\"command\":\"switchCc\",\"ok\":true}");
    }
    else
    {
        printf("[PipeServer] Unknown message: %s\n", msg.c_str());
    }
}

static DWORD WINAPI PipeServerThread(LPVOID)
{
    printf("[PipeServer] Thread started\n");

    while (WaitForSingleObject(g_pipeStopEvent, 0) != WAIT_OBJECT_0)
    {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            printf("[PipeServer] CreateNamedPipe failed: %lu\n", GetLastError());
            Sleep(2000);
            continue;
        }

        // Wait for a client connection (overlapped so we can be interrupted)
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(pipe, &ov);

        DWORD lastErr = GetLastError();
        if (lastErr == ERROR_PIPE_CONNECTED)
        {
            // Client already connected before ConnectNamedPipe returned
            SetEvent(ov.hEvent);
        }
        else if (lastErr != ERROR_IO_PENDING)
        {
            printf("[PipeServer] ConnectNamedPipe failed: %lu\n", lastErr);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }

        HANDLE waitHandles[] = { ov.hEvent, g_pipeStopEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        CloseHandle(ov.hEvent);

        if (waitResult != WAIT_OBJECT_0)
        {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        printf("[PipeServer] Client connected\n");

        EnterCriticalSection(&g_pipeLock);
        g_pipeHandle = pipe;
        g_pipeConnected = true;
        LeaveCriticalSection(&g_pipeLock);

        // Send initial data to the plugin
        PipeSend(BuildOrganListJson());
        PipeSend(BuildStateJson(g_currentLoadedInstalledOrganIndex.load()));

        // Read loop
        char buffer[4096];
        std::string partial;

        while (WaitForSingleObject(g_pipeStopEvent, 0) != WAIT_OBJECT_0)
        {
            OVERLAPPED readOv = {};
            readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD bytesRead = 0;

            BOOL ok = ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, &readOv);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                HANDLE rh[] = { readOv.hEvent, g_pipeStopEvent };
                DWORD rw = WaitForMultipleObjects(2, rh, FALSE, INFINITE);
                if (rw == WAIT_OBJECT_0)
                {
                    if (!GetOverlappedResult(pipe, &readOv, &bytesRead, FALSE))
                    {
                        CloseHandle(readOv.hEvent);
                        break; // pipe broken
                    }
                }
                else
                {
                    CancelIo(pipe);
                    CloseHandle(readOv.hEvent);
                    break; // stop event
                }
            }
            else if (!ok)
            {
                CloseHandle(readOv.hEvent);
                break; // client disconnected
            }

            CloseHandle(readOv.hEvent);

            if (bytesRead == 0)
                break;

            buffer[bytesRead] = '\0';
            partial += buffer;

            // Process complete messages (newline-delimited JSON)
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos)
            {
                std::string line = partial.substr(0, pos);
                partial.erase(0, pos + 1);
                if (!line.empty())
                    HandlePipeMessage(line);
            }
        }

        printf("[PipeServer] Client disconnected\n");

        EnterCriticalSection(&g_pipeLock);
        g_pipeHandle = INVALID_HANDLE_VALUE;
        g_pipeConnected = false;
        LeaveCriticalSection(&g_pipeLock);

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    printf("[PipeServer] Thread stopped\n");
    return 0;
}

void StartStreamDeckPipeServer()
{
    InitializeCriticalSection(&g_pipeLock);
    g_pipeLockInitialized = true;
    g_pipeStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pipeServerThread = CreateThread(nullptr, 0, PipeServerThread, nullptr, 0, nullptr);
    std::thread([]() { UpdateStreamDeckProfileTitles(); }).detach();
    printf("[Startup] StreamDeck pipe server started.\n");
}

void StopStreamDeckPipeServer()
{
    if (g_pipeStopEvent)
    {
        SetEvent(g_pipeStopEvent);
    }
    if (g_pipeServerThread)
    {
        WaitForSingleObject(g_pipeServerThread, 3000);
        CloseHandle(g_pipeServerThread);
        g_pipeServerThread = nullptr;
    }
    if (g_pipeStopEvent)
    {
        CloseHandle(g_pipeStopEvent);
        g_pipeStopEvent = nullptr;
    }
    if (g_pipeLockInitialized)
    {
        DeleteCriticalSection(&g_pipeLock);
        g_pipeLockInitialized = false;
    }
    printf("[Shutdown] StreamDeck pipe server stopped.\n");
}

// ─── Update profile titles on disk and restart Stream Deck ────────────────

static void UpdateStreamDeckProfileTitles()
{
    // 1. Load organ list from Settings.xml
    wchar_t* appDataPath = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath) != S_OK)
        return;
    std::wstring settingsW = std::wstring(appDataPath) + L"\\AhlbornBridge2\\Settings.xml";
    CoTaskMemFree(appDataPath);

    std::string settingsPath(settingsW.begin(), settingsW.end());
    std::vector<OrganInfo> organs = LoadInstalledOrgans(settingsPath);
    if (organs.empty()) return;

    // 2. Build organIndex -> wrapped title map
    std::map<int, std::string> titleMap;
    for (int i = 0; i < (int)organs.size(); ++i)
    {
        std::string raw = organs[i].displayName.empty()
            ? ("ORGAN " + std::to_string(i + 1))
            : organs[i].displayName;
        titleMap[i + 1] = WrapTitle(raw);
    }

    // 3. Find existing AhlbornBridge profile
    std::wstring rootPath = GetProfilesRootPath();
    if (rootPath.empty()) return;
    std::wstring profileFolder = FindExistingProfile(rootPath, "AhlbornBridge");
    if (profileFolder.empty())
    {
        printf("[ProfileUpdate] AhlbornBridge profile not found\n");
        return;
    }

    namespace fs = std::filesystem;
    std::wstring profilesDir = profileFolder + L"\\Profiles";

    bool anyChanged = false;
    std::error_code dirEc;
    for (auto& entry : fs::directory_iterator(profilesDir, dirEc))
    {
        if (!entry.is_directory()) continue;
        std::wstring manifestPath = entry.path().wstring() + L"\\manifest.json";

        std::ifstream f(manifestPath);
        if (!f.is_open()) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();

        if (content.find("com.ahlbornbridge.organ.load") == std::string::npos)
            continue;

        std::string updated = content;

        // For each organ button, find organIndex and update Title/ShowTitle fields in States
        size_t searchPos = 0;
        while (true)
        {
            std::string indexKey = "\"organIndex\":";
            size_t indexPos = updated.find(indexKey, searchPos);
            if (indexPos == std::string::npos) break;

            size_t numStart = indexPos + indexKey.size();
            size_t numEnd = updated.find_first_not_of("0123456789", numStart);
            if (numEnd == std::string::npos || numEnd == numStart)
            { searchPos = numStart + 1; continue; }
            int organIndex = std::stoi(updated.substr(numStart, numEnd - numStart));

            // Verify this is our plugin action (UUID must follow within ~2000 chars)
            size_t uuidCheck = updated.find("\"com.ahlbornbridge.organ.load\"", numEnd);
            if (uuidCheck == std::string::npos || uuidCheck - numEnd > 2000)
            { searchPos = numEnd; continue; }

            auto it = titleMap.find(organIndex);
            if (it == titleMap.end()) { searchPos = numEnd; continue; }
            std::string newTitle = it->second;

            // Find "States":[ between organIndex and UUID
            std::string statesKey = "\"States\":[";
            size_t statesPos = updated.find(statesKey, numEnd);
            if (statesPos == std::string::npos || statesPos > uuidCheck)
            { searchPos = numEnd; continue; }

            // Find matching ] for the States array (skip string contents)
            size_t bracketStart = statesPos + statesKey.size() - 1;
            int depth = 1;
            size_t bracketEnd = bracketStart + 1;
            for (; bracketEnd < updated.size() && depth > 0; ++bracketEnd)
            {
                if (updated[bracketEnd] == '"')
                {
                    ++bracketEnd;
                    while (bracketEnd < updated.size())
                    {
                        if (updated[bracketEnd] == '\\') { bracketEnd += 2; continue; }
                        if (updated[bracketEnd] == '"') break;
                        ++bracketEnd;
                    }
                    continue;
                }
                if (updated[bracketEnd] == '[') ++depth;
                else if (updated[bracketEnd] == ']') --depth;
            }

            // Replace all "Title":"..." within the States array
            std::string titleKey = "\"Title\":\"";
            size_t pos = bracketStart;
            while (pos < bracketEnd)
            {
                size_t titlePos = updated.find(titleKey, pos);
                if (titlePos == std::string::npos || titlePos >= bracketEnd) break;

                size_t valueStart = titlePos + titleKey.size();
                size_t valueEnd = valueStart;
                while (valueEnd < updated.size())
                {
                    if (updated[valueEnd] == '\\') { valueEnd += 2; continue; }
                    if (updated[valueEnd] == '"') break;
                    ++valueEnd;
                }
                if (valueEnd >= updated.size()) break;

                std::string oldTitle = updated.substr(valueStart, valueEnd - valueStart);
                if (oldTitle != newTitle)
                {
                    updated.replace(valueStart, valueEnd - valueStart, newTitle);
                    int delta = (int)newTitle.size() - (int)oldTitle.size();
                    bracketEnd += delta;
                }
                pos = valueStart + newTitle.size() + 1;
            }

            std::string imageKey = "\"Image\":\"";
            pos = bracketStart;
            while (pos < bracketEnd)
            {
                size_t imagePos = updated.find(imageKey, pos);
                if (imagePos == std::string::npos || imagePos >= bracketEnd) break;

                size_t valueStart = imagePos + imageKey.size();
                size_t valueEnd = valueStart;
                while (valueEnd < updated.size())
                {
                    if (updated[valueEnd] == '\\') { valueEnd += 2; continue; }
                    if (updated[valueEnd] == '"') break;
                    ++valueEnd;
                }
                if (valueEnd >= updated.size()) break;

                size_t removeEnd = valueEnd + 1;
                if (removeEnd < updated.size() && updated[removeEnd] == ',')
                    ++removeEnd;

                updated.erase(imagePos, removeEnd - imagePos);
                bracketEnd -= (removeEnd - imagePos);
                pos = imagePos;
            }

            std::string showTitleFalse = "\"ShowTitle\":false";
            std::string showTitleTrue = "\"ShowTitle\":true";
            pos = bracketStart;
            while (pos < bracketEnd)
            {
                size_t showTitlePos = updated.find(showTitleTrue, pos);
                if (showTitlePos == std::string::npos || showTitlePos >= bracketEnd) break;
                updated.replace(showTitlePos, showTitleTrue.size(), showTitleFalse);
                int delta = (int)showTitleFalse.size() - (int)showTitleTrue.size();
                bracketEnd += delta;
                pos = showTitlePos + showTitleFalse.size();
            }

            searchPos = bracketEnd;
        }

        if (updated != content)
        {
            std::ofstream out(manifestPath, std::ios::trunc);
            if (out.is_open())
            {
                out << updated;
                anyChanged = true;
                printf("[ProfileUpdate] Updated titles in page: %ls\n",
                    entry.path().filename().c_str());
            }
        }
    }

    if (anyChanged)
    {
        printf("[ProfileUpdate] Titles changed, restarting Stream Deck...\n");

        // Kill Stream Deck (this also kills the plugin process)
        system("taskkill /f /im StreamDeck.exe >nul 2>&1");
        Sleep(3000);

        // Restart Stream Deck
        ShellExecuteW(nullptr, L"open",
            L"C:\\Program Files\\Elgato\\StreamDeck\\StreamDeck.exe",
            nullptr, nullptr, SW_HIDE);

        printf("[ProfileUpdate] Stream Deck restarted.\n");
    }
    else
    {
        printf("[ProfileUpdate] No title changes needed.\n");
    }
}

void NotifyStreamDeckOrganList()
{
    printf("[PipeServer] NotifyStreamDeckOrganList called, pipeConnected=%d\n", (int)g_pipeConnected.load());
    if (g_pipeConnected)
    {
        std::string json = BuildOrganListJson();
        printf("[PipeServer] Sending organ list (%d bytes)\n", (int)json.size());
        PipeSend(json);
    }

    // Update profile titles on disk and restart SD (runs on background thread
    // to avoid blocking the caller while waiting for SD restart)
    std::thread([]() { UpdateStreamDeckProfileTitles(); }).detach();
}

void NotifyStreamDeckOrganState(int loadedIndex)
{
    if (g_pipeConnected)
        PipeSend(BuildStateJson(loadedIndex));
}

void NotifyStreamDeckOrganUnloaded(bool resetSwitches)
{
    if (!g_pipeConnected)
        return;

    if (resetSwitches)
    {
        // Reset all switch states to off in the plugin UI
        std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
        for (int i = 0; i < static_cast<int>(switches.size()); ++i)
        {
            int switchIndex = i + 1;
            PipeSend(std::string("{\"type\":\"switchState\",\"index\":") +
                     std::to_string(switchIndex) + ",\"isOn\":false}");
        }
    }

    PipeSend(BuildStateJson(0));
}

void NotifyStreamDeckSwitchState(int switchIndex, bool isOn)
{
    if (!g_pipeConnected || switchIndex <= 0)
    {
        printf("[NotifyStreamDeckSwitchState] SKIPPED index=%d pipeConnected=%d\n", switchIndex, (int)g_pipeConnected.load());
        return;
    }
    std::string msg = std::string("{\"type\":\"switchState\",\"index\":") +
             std::to_string(switchIndex) +
             ",\"isOn\":" + (isOn ? "true" : "false") + "}";
    printf("[NotifyStreamDeckSwitchState] Sending: %s\n", msg.c_str());
    PipeSend(msg);
}

void NotifyStreamDeckSwitchesForOrgan(int loadedIndex)
{
    if (loadedIndex <= 0)
        return;

    std::vector<InstalledOrganInfo> organs = LoadInstalledOrganInfos();
    if (loadedIndex > static_cast<int>(organs.size()))
    {
        printf("[NotifyStreamDeckSwitchesForOrgan] loadedIndex=%d exceeds organs.size()=%d, aborting\n",
            loadedIndex, (int)organs.size());
        return;
    }

    const InstalledOrganInfo& organInfo = organs[loadedIndex - 1];
    std::wstring uniqueId = organInfo.uniqueOrganId;
    if (uniqueId.empty())
        uniqueId = organInfo.id;  // fallback: use positional id when UniqueOrganID is absent
    if (uniqueId.empty())
    {
        printf("[NotifyStreamDeckSwitchesForOrgan] uniqueId empty for loadedIndex=%d, aborting\n", loadedIndex);
        return;
    }

    printf("[NotifyStreamDeckSwitchesForOrgan] loadedIndex=%d id=%S uniqueOrganId=%S -> using uniqueId=%S\n",
        loadedIndex, organInfo.id.c_str(), organInfo.uniqueOrganId.c_str(), uniqueId.c_str());

    std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
    std::map<int, bool> states = LoadOrganSwitchStates(uniqueId);

    printf("[NotifyStreamDeckSwitchesForOrgan] pipeConnected=%d stateCount=%d\n",
        (int)g_pipeConnected.load(), (int)states.size());

    // Send the state first so the plugin knows an organ is loaded before
    // receiving switchState messages (which are ignored when loadedIndex == 0).
    // Use a direct state message with the known loadedIndex instead of
    // BuildStateJson() which returns loadedIndex=0 when g_isLoadingOrgan is still set.
    if (g_pipeConnected)
    {
        bool consoleConnected = (deviceState.load() == DeviceState::Connected);
        bool hauptwerkRunning = IsProcessRunningByName(L"Hauptwerk.exe");
        bool vstLink = IsVstLinkEngineForInstalledOrgan(loadedIndex);
        std::string stateMsg = "{\"type\":\"state\",\"loadedIndex\":" + std::to_string(loadedIndex) +
            ",\"vstLink\":" + std::string(vstLink ? "true" : "false") +
            ",\"consoleConnected\":" + std::string(consoleConnected ? "true" : "false") +
            ",\"hauptwerkRunning\":" + std::string(hauptwerkRunning ? "true" : "false") +
            ",\"loadingIndex\":0,\"loadingProgress\":0,\"loadingVisible\":false,\"biduleSplashActive\":false}";
        PipeSend(stateMsg);

        // Reset all switches to OFF first so stale ON states from the previous
        // organ are cleared before the new organ's saved states arrive.
        for (int i = 1; i <= static_cast<int>(switches.size()); ++i)
            NotifyStreamDeckSwitchState(i, false);
    }

    for (const auto& [switchIndex, isOn] : states)
    {
        // Update Stream Deck UI
        printf("[NotifyStreamDeckSwitchesForOrgan] Sending switchState #%d isOn=%d pipeConnected=%d\n",
            switchIndex, (int)isOn, (int)g_pipeConnected.load());
        if (g_pipeConnected)
            NotifyStreamDeckSwitchState(switchIndex, isOn);

        // Send the actual CC to Hauptwerk to restore the physical stop state
        if (switchIndex >= 1 && switchIndex <= static_cast<int>(switches.size()))
        {
            const AhlbornSwitchInfo& s = switches[switchIndex - 1];
            int value = isOn ? s.valueOn : s.valueOff;
            printf("[NotifyStreamDeckSwitchesForOrgan] Restoring switch #%d ch=%d cc=%d value=%d (isOn=%d)\n",
                switchIndex, s.channel, s.controlChange, value, (int)isOn);
            SendAhlbornSwitchControlChange(s.channel, s.controlChange, value);
        }
    }
}

void NotifyStreamDeckActiveSensingState(bool enabled)
{
    if (!g_pipeConnected)
        return;
    PipeSend(std::string("{\"type\":\"activeSensingState\",\"enabled\":") +
        (enabled ? "true" : "false") + "}");
}

void SendAhlbornSwitchControlChange(int channel, int controlChange, int value)
{
    if (channel < 1 || channel > 16 || controlChange < 0 || controlChange > 127 || value < 0 || value > 127)
    {
        printf("[StreamDeck] Invalid switch CC request: ch=%d cc=%d value=%d\n", channel, controlChange, value);
        return;
    }

    DWORD status = static_cast<DWORD>(0xB0 | ((channel - 1) & 0x0F));
    DWORD midiMsg = status | (static_cast<DWORD>(controlChange) << 8) | (static_cast<DWORD>(value) << 16);
    bool enqueued = EnqueueMidiOutMessage(midiMsg);
    printf("[StreamDeck] Send switch CC ch=%d cc=%d value=%d: %s\n",
        channel, controlChange, value, enqueued ? "enqueued OK" : "FAILED");
}
