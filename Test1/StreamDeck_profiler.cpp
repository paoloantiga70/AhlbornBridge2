
#include "StreamDeck_profiler.h"
#include "Xml.h"

#include <algorithm>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;

// Returns the path: %APPDATA%\Elgato\StreamDeck\ProfilesV3
std::wstring GetProfilesRootPath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return {};
    return std::wstring(appData) + L"\\Elgato\\StreamDeck\\ProfilesV3\\";
}

// Generates a simple GUID string (without braces)
std::string GenerateGuid()
{
    GUID guid = {};
    CoCreateGuid(&guid);
    char buf[40] = {};
    snprintf(buf, sizeof(buf),
        "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5],
        guid.Data4[6], guid.Data4[7]);
    return std::string(buf);
}

// Converts a GUID string to lowercase
std::string GuidToLower(const std::string& guid)
{
    std::string lower = guid;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lower;
}

// Device info from an existing .sdProfile manifest
// Reads device model and UUID from <profile>.sdProfile\manifest.json
DeviceInfo ReadDeviceInfo(const fs::path& sdProfilePath)
{
    std::ifstream f(sdProfilePath / "manifest.json");
    if (!f.is_open()) return {};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto extractValue = [&](const std::string& key) -> std::string
        {
            std::string search = "\"" + key + "\":\"";
            size_t pos = content.find(search);
            if (pos == std::string::npos) return {};
            pos += search.size();
            size_t end = content.find("\"", pos);
            return (end == std::string::npos) ? std::string{} : content.substr(pos, end - pos);
        };

    return { extractValue("Model"), extractValue("UUID") };
}

// Finds the first valid device profile in ProfilesV3
DeviceInfo FindFirstDevice(const std::wstring& rootPath)
{
    for (const auto& entry : fs::directory_iterator(rootPath))
    {
        if (!entry.is_directory() || entry.path().extension() != L".sdProfile") continue;
        DeviceInfo info = ReadDeviceInfo(entry.path());
        if (!info.model.empty() && !info.uuid.empty())
            return info;
    }
    return {};
}

// Maps a device model string to the deviceType integer used in BuildPageManifest
// 0 = standard 3x5 (15 keys), 3 = Mini 2x3 (6 keys), 4 = XL 4x8 (32 keys)
int DeviceTypeFromModel(const std::string& model)
{
    std::string lower = model;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // Stream Deck SDK device types: 0=Standard, 1=Mini, 2=XL, 3=Mobile.
    if (lower.find("mini") != std::string::npos) return 1;
    if (lower.find("xl") != std::string::npos) return 2;
    // Stream Deck Mobile reports its model as "VSD2/WiFi".
    if (lower.find("vsd") != std::string::npos || lower.find("mobile") != std::string::npos) return 3;
    return 0;
}

// Returns the number of keys per page for the given device type
int GetKeysPerPage(int deviceType)
{
    if (deviceType == 1) return 6;   // Mini 2x3
    if (deviceType == 2) return 32;  // XL 4x8
    return 15;                        // Standard / Mobile 3x5
}

// MIDI action settings for the "se.trevligaspel.midi" plugin
// Action types for Stream Deck keys


// Reads the installed version of a plugin from its manifest.json
std::string GetInstalledPluginVersion(const std::string& pluginUUID)
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return "1.0";
    std::wstring path = std::wstring(appData) + L"\\Elgato\\StreamDeck\\Plugins\\"
        + std::wstring(pluginUUID.begin(), pluginUUID.end()) + L".sdPlugin\\manifest.json";
    std::ifstream f(path);
    if (!f.is_open()) return "1.0";
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = content.find("\"Version\":\"");
    if (pos == std::string::npos) return "1.0";
    pos += 11;
    size_t end = content.find("\"", pos);
    return (end == std::string::npos) ? "1.0" : content.substr(pos, end - pos);
}

// Builds the Settings JSON for a MIDI Generic action
// Key parameters are exposed via MidiSettings; all other fields use plugin defaults
std::string BuildMidiSettingsJson(const MidiSettings& m)
{
    return
        "{\"a0\":true,\"a1\":false,\"a2\":null,\"a3\":false,"
        "\"b01\":\"\",\"b02\":null,\"b05\":\"0\",\"b06\":[],\"b07\":[],\"b08\":\"Never\","
        "\"b09\":\"700\",\"b10\":\"\",\"b11\":null,\"b12\":\"None\",\"b13\":[],"
        "\"b14\":\"Default group\",\"b15\":\"\",\"b15a\":false,\"b15b\":\"700\","
        "\"b16\":true,\"b17\":false,\"b18\":false,\"b19\":true,\"b20\":true,"
        "\"b21\":false,\"b22\":false,\"b23\":[],\"b24\":\"None\",\"b25\":[],"
        "\"b26\":\"None\",\"c01\":false,\"c03\":\"\",\"c05\":null,\"c06\":null,"
        "\"c07\":false,\"c08\":null,\"el\":false,\"f01\":50,\"f02\":\"0\","
        "\"f03\":\"127\",\"f04\":false,\"f05\":false,\"f06\":true,\"f07\":false,"
        "\"f08\":true,\"f09\":[],\"f10\":\"\",\"f11\":null,\"f12\":false,"
        "\"f13\":false,\"f14\":null,\"f15\":null,\"f16\":false,\"f17\":null,"
        "\"f18\":true,\"f19\":null,\"f20\":-67,\"f21\":67,\"f22\":40,"
        "\"f23\":\"\",\"f24\":\"\",\"f30\":\"\",\"f31\":[],\"f32\":null,"
        "\"f33\":[],\"f34\":null,"
        "\"g01\":[],\"g02\":\"" + m.buttonMode + "\","
        "\"g03\":[],\"g04\":\"" + m.messageType + "\","
        "\"g05\":\"0\","
        "\"g06\":null,\"g07\":null,"
        "\"g08\":\"" + m.valueOn + "\","
        "\"g09\":\"" + m.valueOff + "\","
        "\"g10\":[],\"g11\":\"" + m.actionWhenReceived + "\",\"g13\":\"" + m.valueOn + "\",\"g14\":\"0\",\"g15\":\"0\","
        "\"g16\":\"0\",\"g17\":\"\",\"g18\":\"\",\"g19\":\"\",\"g20\":\"\","
        "\"g21\":\"\",\"g22\":\"\",\"g23\":\"\",\"g24\":\"\",\"g25\":false,"
        "\"g27\":\"\",\"g28\":\"\",\"g29\":\"\",\"g30\":null,\"g31\":null,"
        "\"g32\":null,\"g33\":[],"
        "\"g34\":\"" + m.note + "\","
        "\"g36\":\"\",\"g37\":false,\"g38\":true,\"g39\":false,\"g40\":null,"
        "\"g41\":false,\"g42\":\"\",\"g43\":\"\",\"g44\":[],\"g45\":\"" + m.ccNumber + "\","
        "\"g46\":[],\"g47\":\"" + m.channel + "\",\"g48\":[],\"g49\":\"" + m.ccNumber + "\",\"g50\":[],"
        "\"g51\":null,\"g52\":\"" + m.valueOn + "\",\"g53\":\"" + m.valueOff + "\","
        "\"id\":\"\",\"il\":false,\"mi\":[],\"mo\":[],"
        "\"s01\":false,\"s04\":false,\"s05\":true,\"s06\":false,\"s07\":false,"
        "\"s08\":true,\"s09\":true,\"s10\":true,\"s11\":true,\"s12\":false,"
        "\"s13\":false,\"s14\":false,\"s15\":false,\"s16\":true,\"s17\":false,"
        "\"s18\":false,\"s19\":false,\"s21\":false,\"s22\":false,\"s23\":false,"
        "\"s24\":false,\"s25\":false,\"s26\":false,\"s29\":false,\"s30\":true,"
        "\"s31\":false,\"s32\":true,\"s34\":true,\"s35\":false,\"s36\":false,"
        "\"sb\":true,\"sc\":false,\"scc\":true,\"sco\":false,\"scp\":false,"
        "\"se01\":false,\"se02\":[],\"se03\":\"CC\",\"se04\":[],\"se05\":\"C3\","
        "\"se07\":\"1\",\"se08\":\"0\",\"se09\":\"127\",\"se10\":\"0\","
        "\"se11\":false,\"se12\":false,\"se13\":\"\",\"se14\":\"\","
        "\"se15\":null,\"se16\":null,\"se17\":null,\"se18\":null,\"se19\":[],"
        "\"se20\":\"0\",\"se22\":\"0\",\"se23\":\"127\",\"se24\":\"0\","
        "\"se30\":\"0\",\"se32\":[],\"se33\":[],"
        "\"sf\":false,\"sh\":false,\"sl\":false,\"sl01\":true,\"sl02\":false,"
        "\"smi\":\"" + m.inputPort + "\","
        "\"smo\":\"" + m.outputPort + "\","
        "\"snote\":false,\"snrpn\":false,\"sp\":false,\"spb\":false,\"spc\":false,"
        "\"sr\":false,\"ss\":false,\"sse\":false,\"st\":true,\"stf\":false,"
        "\"sv\":false,\"svu\":false,"
        "\"v01\":50,\"v02\":\"0\",\"v03\":\"127\",\"v07\":\"700\",\"v08\":false,"
        "\"v09\":false,\"v10\":[],\"v11\":\"\",\"v12\":null,\"v13\":[],"
        "\"v14\":\"Vol\",\"v15\":[],\"v16\":\"Auto\",\"v17\":false,\"v18\":false,"
        "\"v19\":false,\"v20\":\"\",\"vm\":\"\","
        "\"vu01\":false,\"vu02\":[],\"vu03\":\"CC\",\"vu04\":[],\"vu05\":\"C3\","
        "\"vu07\":\"1\",\"vu08\":\"0\",\"vu13\":\"\",\"vu14\":\"\","
        "\"vu15\":[],\"vu16\":\"0\",\"vu17\":[],\"vu18\":\"0\",\"vu9\":\"\","
        "\"w01\":\"700\",\"w02\":\"10\",\"w05\":[],\"w06\":\"Now\","
        "\"w07\":\"700\",\"w07s\":false,"
        "\"x90\":false,\"x91\":true,\"x92\":false,\"x93\":false,\"x94\":true,"
        "\"x95\":true,\"x96\":false,\"x97\":true,\"x98\":true,\"x99\":[]}";
}

std::string WrapTitle(const std::string& text, int maxCharsPerLine, int maxLines)
{
    if (text.empty()) return text;

    // Split input into words
    std::vector<std::string> words;
    std::string word;
    for (char ch : text)
    {
        if (ch == ' ' || ch == '\t')
        {
            if (!word.empty()) { words.push_back(word); word.clear(); }
        }
        else
        {
            word += ch;
        }
    }
    if (!word.empty()) words.push_back(word);

    std::vector<std::string> lines;
    std::string currentLine;

    for (const auto& w : words)
    {
        if (static_cast<int>(lines.size()) >= maxLines) break;

        if (static_cast<int>(w.size()) > maxCharsPerLine)
        {
            // Flush current line first
            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
                if (static_cast<int>(lines.size()) >= maxLines) break;
            }
            // Break long word across multiple lines
            for (size_t pos = 0; pos < w.size() && static_cast<int>(lines.size()) < maxLines; pos += maxCharsPerLine)
            {
                std::string chunk = w.substr(pos, maxCharsPerLine);
                if (pos + maxCharsPerLine < w.size())
                {
                    lines.push_back(chunk);
                }
                else
                {
                    currentLine = chunk;
                }
            }
        }
        else if (currentLine.empty())
        {
            currentLine = w;
        }
        else if (static_cast<int>(currentLine.size()) + 1 + static_cast<int>(w.size()) <= maxCharsPerLine)
        {
            currentLine += ' ';
            currentLine += w;
        }
        else
        {
            lines.push_back(currentLine);
            currentLine = w;
            if (static_cast<int>(lines.size()) >= maxLines) { currentLine.clear(); break; }
        }
    }
    if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines)
    {
        lines.push_back(currentLine);
    }

    // Join with literal \n for JSON
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0) result += "\\n";
        result += lines[i];
    }
    return result;
}

// Builds the compact JSON block for a single key action (V3 format)
// imageRef: relative path of the icon inside the page folder (e.g. "Images/foo.png"), or empty
std::string BuildActionJson(const KeyAction& action, const std::string& imageRef, const std::string& imageRefOn)
{
    std::string instanceId = GuidToLower(GenerateGuid());

    std::string pluginName, pluginUUID = action.actionUUID, actionDisplayName;
    std::string pluginVersion = "1.0";

    if (action.actionUUID == "com.elgato.streamdeck.system.website")
    {
        pluginName = "Website";
        actionDisplayName = "Website";
    }
    else if (action.actionUUID == "com.elgato.streamdeck.system.hotkey")
    {
        pluginName = "Activate a Key Command";
        actionDisplayName = "Hotkey";
    }
    else if (action.actionUUID == "se.trevligaspel.midi.genericmidi")
    {
        pluginUUID = "se.trevligaspel.midi";
        pluginName = "Midi";
        actionDisplayName = "Generic Midi";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else if (action.actionUUID == "com.ahlbornbridge.organ.load")
    {
        pluginUUID = "com.ahlbornbridge.organ";
        pluginName = "AhlbornBridge";
        actionDisplayName = "Load Organ";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else if (action.actionUUID == "com.ahlbornbridge.switch")
    {
        pluginUUID = "com.ahlbornbridge.organ";
        pluginName = "AhlbornBridge";
        actionDisplayName = "Ahlborn Switches";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else if (action.actionUUID == "com.ahlbornbridge.activesensing")
    {
        pluginUUID = "com.ahlbornbridge.organ";
        pluginName = "AhlbornBridge";
        actionDisplayName = "Console Power";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else if (action.actionUUID == "com.ahlbornbridge.dsp")
    {
        pluginUUID = "com.ahlbornbridge.organ";
        pluginName = "AhlbornBridge";
        actionDisplayName = "DSP Command";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else if (action.actionUUID == "com.ahlbornbridge.ir")
    {
        pluginUUID = "com.ahlbornbridge.organ";
        pluginName = "AhlbornBridge";
        actionDisplayName = "IR Church";
        pluginVersion = GetInstalledPluginVersion(pluginUUID);
    }
    else
    {
        pluginName = action.actionUUID;
        actionDisplayName = action.actionUUID;
    }

    std::string settings;
    if (action.actionUUID == "com.elgato.streamdeck.system.website")
        settings = "{\"browser\":\"\",\"openInBrowser\":true,\"path\":\"" + action.url + "\"}";
    else if (action.actionUUID == "com.elgato.streamdeck.system.hotkey")
        settings = "{\"hotkeys\":[{\"KeyCmd\":\"" + action.hotkey + "\"}]}";
    else if (action.actionUUID == "se.trevligaspel.midi.genericmidi")
        settings = BuildMidiSettingsJson(action.midi);
    else if (action.actionUUID == "com.ahlbornbridge.organ.load")
    {
        std::string safeName;
        for (char c : action.title)
        {
            if (c == '"') safeName += "\\\"";
            else if (c == '\\') safeName += "\\\\";
            else safeName += c;
        }
        settings = "{\"organIndex\":" + std::to_string(action.organIndex)
            + ",\"organName\":\"" + safeName + "\"}";
    }
    else if (action.actionUUID == "com.ahlbornbridge.switch")
    {
        std::string safeName;
        for (char c : action.title)
        {
            if (c == '"') safeName += "\\\"";
            else if (c == '\\') safeName += "\\\\";
            else safeName += c;
        }
        settings = "{\"switchIndex\":" + std::to_string(action.organIndex)
            + ",\"switchIsOn\":false"
            + ",\"switchName\":\"" + safeName + "\"}";
    }
    else if (action.actionUUID == "com.ahlbornbridge.activesensing")
    {
        settings = "{}";
    }
    else if (action.actionUUID == "com.ahlbornbridge.dsp")
    {
        auto escapeJson = [](const std::string& in) {
            std::string out;
            for (char c : in)
            {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else out += c;
            }
            return out;
        };
        settings = "{\"oscAddress\":\"" + escapeJson(action.oscAddress)
            + "\",\"oscValue\":\"" + escapeJson(action.oscValue) + "\"}";
    }
    else if (action.actionUUID == "com.ahlbornbridge.ir")
    {
        auto escapeJson = [](const std::string& in) {
            std::string out;
            for (char c : in)
            {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else out += c;
            }
            return out;
        };
        settings = "{\"churchName\":\"" + escapeJson(action.churchName)
            + "\",\"thumbnail\":\"" + escapeJson(action.thumbnail)
            + "\",\"oscFront\":\"" + escapeJson(action.oscFront)
            + "\",\"oscSide\":\"" + escapeJson(action.oscSide)
            + "\",\"oscRear\":\"" + escapeJson(action.oscRear) + "\"}";
    }
    else
        settings = "{}";

    // AhlbornBridge native actions (switch, activesensing) use the exact
    // same structure as when the user manually adds them in Stream Deck:
    // LinkedTitle=true, empty FontFamily/FontStyle, ShowTitle=true on both
    // states, no Title field, no image (the plugin renders its own SVG).
    std::string states;
    std::string fontSizeStr = std::to_string(action.fontSize);

    if (action.actionUUID == "com.ahlbornbridge.ir")
    {
        // IR Church: single state, title hidden (plugin renders its own SVG
        // badge from the thumbnail). An image may still be referenced so the
        // profile shows the thumbnail before the plugin takes over.
        std::string imageField = imageRef.empty() ? "" : "\"Image\":\"" + imageRef + "\",";
        std::string stateBlock =
            "{" + imageField +
            "\"FontFamily\":\"\",\"FontSize\":" + fontSizeStr + ",\"FontStyle\":\"\","
            "\"FontUnderline\":false,\"OutlineThickness\":2,\"ShowTitle\":false,"
            "\"TitleAlignment\":\"" + action.titleAlignment + "\","
            "\"TitleColor\":\"" + action.titleColor + "\"}";
        states = "[" + stateBlock + "]";

        return
            "{\"ActionID\":\"" + instanceId + "\","
            "\"LinkedTitle\":true,"
            "\"Name\":\"" + actionDisplayName + "\","
            "\"Plugin\":{\"Name\":\"" + pluginName + "\","
            "\"UUID\":\"" + pluginUUID + "\","
            "\"Version\":\"" + pluginVersion + "\"},"
            "\"Resources\":null,"
            "\"Settings\":" + settings + ","
            "\"State\":0,"
            "\"States\":" + states + ","
            "\"UUID\":\"" + action.actionUUID + "\"}";
    }

    if (action.actionUUID == "com.ahlbornbridge.switch"
        || action.actionUUID == "com.ahlbornbridge.activesensing")
    {
        std::string stateBlock =
            "{\"FontFamily\":\"\",\"FontSize\":" + fontSizeStr + ","
            "\"FontStyle\":\"\",\"FontUnderline\":false,\"OutlineThickness\":2,"
            "\"ShowTitle\":true,\"TitleAlignment\":\"" + action.titleAlignment + "\","
            "\"TitleColor\":\"" + action.titleColor + "\"}";
        states = "[" + stateBlock + "," + stateBlock + "]";

        return
            "{\"ActionID\":\"" + instanceId + "\","
            "\"LinkedTitle\":true,"
            "\"Name\":\"" + actionDisplayName + "\","
            "\"Plugin\":{\"Name\":\"" + pluginName + "\","
            "\"UUID\":\"" + pluginUUID + "\","
            "\"Version\":\"" + pluginVersion + "\"},"
            "\"Resources\":null,"
            "\"Settings\":" + settings + ","
            "\"State\":0,"
            "\"States\":" + states + ","
            "\"UUID\":\"" + action.actionUUID + "\"}";
    }

    // MIDI and custom plugin use 2 states (off/on); other actions use 1 state with title
    bool isTwoStateAction = (action.actionUUID == "se.trevligaspel.midi.genericmidi"
        || action.actionUUID == "com.ahlbornbridge.organ.load"
        || action.actionUUID == "com.ahlbornbridge.switch"
        || action.actionUUID == "com.ahlbornbridge.activesensing");
    if (isTwoStateAction)
    {
        bool showTitle = !action.title.empty()
            && action.actionUUID != "com.ahlbornbridge.organ.load";
        std::string titleField = showTitle ? "\"Title\":\"" + action.title + "\"," : "";
        auto buildMidiState = [&](const std::string& color, const std::string& img) {
            bool includeImage = !img.empty() && action.actionUUID != "com.ahlbornbridge.organ.load";
            std::string imgField = includeImage ? "\"Image\":\"" + img + "\"," : "";
            return
                "{" + imgField +
                "\"FontFamily\":\"" + action.fontFamily + "\",\"FontSize\":" + fontSizeStr + ",\"FontStyle\":\"Regular\","
                "\"FontUnderline\":false,\"OutlineThickness\":2,"
                + titleField +
                "\"ShowTitle\":" + (showTitle ? "true" : "false") + ","
                "\"TitleAlignment\":\"" + action.titleAlignment + "\","
                "\"TitleColor\":\"" + color + "\"}";
            };
        std::string imgOn = imageRefOn.empty() ? imageRef : imageRefOn;
        states = "[" + buildMidiState(action.titleColor, imageRef) + "," + buildMidiState(action.titleColorOn, imgOn) + "]";
    }
    else
    {
        std::string imageField = imageRef.empty() ? "" : "\"Image\":\"" + imageRef + "\",";
        states =
            "[{" + imageField +
            "\"FontFamily\":\"" + action.fontFamily + "\",\"FontSize\":" + fontSizeStr + ",\"FontStyle\":\"Regular\","
            "\"FontUnderline\":false,\"OutlineThickness\":2,\"ShowTitle\":true,"
            "\"Title\":\"" + action.title + "\","
            "\"TitleAlignment\":\"" + action.titleAlignment + "\","
            "\"TitleColor\":\"" + action.titleColor + "\"}]";
    }

    bool linkedTitle = (action.actionUUID != "com.ahlbornbridge.organ.load"
        && action.actionUUID != "com.ahlbornbridge.switch"
        && action.actionUUID != "com.ahlbornbridge.activesensing"
        && !isTwoStateAction);
    return
        "{\"ActionID\":\"" + instanceId + "\","
        "\"LinkedTitle\":" + (linkedTitle ? "true" : "false") + ","
        "\"Name\":\"" + actionDisplayName + "\","
        "\"Plugin\":{\"Name\":\"" + pluginName + "\","
        "\"UUID\":\"" + pluginUUID + "\","
        "\"Version\":\"" + pluginVersion + "\"},"
        "\"Resources\":null,"
        "\"Settings\":" + settings + ","
        "\"State\":0,"
        "\"States\":" + states + ","
        "\"UUID\":\"" + action.actionUUID + "\"}";
}

// Builds Profiles\<GUID>\manifest.json (page manifest, V3 compact format)
// Key index uses "col,row" format as required by Stream Deck 7.x
std::string BuildPageManifest(const std::string& pageName, int deviceType,
    const std::map<int, KeyAction>& keys,
    const std::map<int, std::string>& imageRefs,
    const std::map<int, std::string>& imageRefsOn)
{
    int rows = 3, cols = 5;
    if (deviceType == 3) { rows = 3; cols = 5; }  // Stream Deck Mobile 5x3 = 15 keys
    if (deviceType == 4) { rows = 4; cols = 8; }
    int totalKeys = rows * cols;

    std::string actionsJson;
    bool first = true;
    for (int i = 0; i < totalKeys; ++i)
    {
        auto it = keys.find(i);
        if (it == keys.end()) continue;
        int row = i / cols;
        int col = i % cols;
        std::string keyIndex = std::to_string(col) + "," + std::to_string(row);
        auto imgIt = imageRefs.find(i);
        std::string imageRef = (imgIt != imageRefs.end()) ? imgIt->second : "";
        auto imgOnIt = imageRefsOn.find(i);
        std::string imageRefOn = (imgOnIt != imageRefsOn.end()) ? imgOnIt->second : "";
        if (!first) actionsJson += ",";
        actionsJson += "\"" + keyIndex + "\":" + BuildActionJson(it->second, imageRef, imageRefOn);
        first = false;
    }

    std::string actionsValue = actionsJson.empty() ? "null" : "{" + actionsJson + "}";

    return
        "{\"Controllers\":[{\"Actions\":" + actionsValue + ",\"Type\":\"Keypad\"}],"
        "\"Icon\":\"\",\"Name\":\"" + pageName + "\"}";
}

// Builds the top-level <GUID>.sdProfile\manifest.json (device profile manifest, V3 compact format)
std::string BuildDeviceManifest(const std::string& profileName,
    const DeviceInfo& device, const std::vector<std::string>& pageGuidsLower)
{
    std::string pagesArray;
    for (size_t i = 0; i < pageGuidsLower.size(); ++i)
    {
        if (i > 0) pagesArray += ",";
        pagesArray += "\"" + pageGuidsLower[i] + "\"";
    }

    return
        "{\"Device\":{\"Model\":\"" + device.model + "\",\"UUID\":\"" + device.uuid + "\"},"
        "\"Name\":\"" + profileName + "\","
        "\"Pages\":{\"Current\":\"" + pageGuidsLower[0] + "\","
        "\"Default\":\"" + pageGuidsLower[0] + "\","
        "\"Pages\":[" + pagesArray + "]},"
        "\"Version\":\"3.0\"}";
}

// Reads all <Organ id="...">...</Organ> nodes from <InstalledOrgans> in Settings.xml
std::vector<OrganInfo> LoadInstalledOrgans(const std::string& xmlPath)
{
    std::ifstream f(xmlPath);
    if (!f.is_open()) { std::cerr << "Cannot open: " << xmlPath << "\n"; return {}; }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    size_t start = content.find("<InstalledOrgans>");
    size_t end = content.find("</InstalledOrgans>", start);
    if (start == std::string::npos || end == std::string::npos) return {};
    std::string block = content.substr(start, end - start);

    std::vector<OrganInfo> organs;
    size_t pos = 0;
    while ((pos = block.find("<Organ id=\"", pos)) != std::string::npos)
    {
        size_t idStart = pos + 11;
        size_t idEnd = block.find('"', idStart);
        if (idEnd == std::string::npos) break;
        std::string id = block.substr(idStart, idEnd - idStart);

        // Look for displayName attribute between the id and the closing >
        std::string displayName;
        size_t tagEnd = block.find('>', idEnd);
        if (tagEnd != std::string::npos)
        {
            std::string tagContent = block.substr(idEnd, tagEnd - idEnd);
            std::string dnKey = "displayName=\"";
            size_t dnPos = tagContent.find(dnKey);
            if (dnPos != std::string::npos)
            {
                size_t dnStart = dnPos + dnKey.size();
                size_t dnEnd = tagContent.find('"', dnStart);
                if (dnEnd != std::string::npos)
                    displayName = tagContent.substr(dnStart, dnEnd - dnStart);
            }
        }

        size_t titleStart = block.find('>', idEnd) + 1;
        size_t titleEnd = block.find("</Organ>", titleStart);
        if (titleStart == std::string::npos || titleEnd == std::string::npos) break;
        std::string title = block.substr(titleStart, titleEnd - titleStart);

        organs.push_back({ id, title, displayName });
        pos = titleEnd;
    }
    return organs;
}

// Reads the installed plugin's churches.json (a JSON array of church records)
// and returns them sorted alphabetically by name.
std::vector<ChurchInfo> LoadChurches()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return {};
    std::wstring pathW = std::wstring(appData)
        + L"\\Elgato\\StreamDeck\\Plugins\\com.ahlbornbridge.organ.sdPlugin\\churches.json";

    std::ifstream f(pathW);
    if (!f.is_open())
    {
        std::cerr << "LoadChurches: cannot open churches.json in installed plugin.\n";
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Extracts the string value of "key":"value" starting from a search origin.
    auto extractField = [](const std::string& obj, const std::string& key) -> std::string
        {
            std::string k = "\"" + key + "\"";
            size_t kp = obj.find(k);
            if (kp == std::string::npos) return {};
            size_t colon = obj.find(':', kp + k.size());
            if (colon == std::string::npos) return {};
            size_t q1 = obj.find('"', colon + 1);
            if (q1 == std::string::npos) return {};
            std::string out;
            for (size_t i = q1 + 1; i < obj.size(); ++i)
            {
                char c = obj[i];
                if (c == '"') break;
                if (c == '\\' && i + 1 < obj.size())
                {
                    char esc = obj[++i];
                    switch (esc)
                    {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u':
                    {
                        // Decode \uXXXX; emit UTF-8. ASCII (< 0x80) is the common
                        // case here (e.g. \u0027 apostrophe in church names).
                        if (i + 4 < obj.size())
                        {
                            std::string hex = obj.substr(i + 1, 4);
                            unsigned code = 0;
                            bool ok = true;
                            for (char h : hex)
                            {
                                code <<= 4;
                                if (h >= '0' && h <= '9') code |= (h - '0');
                                else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok)
                            {
                                i += 4;
                                if (code < 0x80)
                                    out += static_cast<char>(code);
                                else if (code < 0x800)
                                {
                                    out += static_cast<char>(0xC0 | (code >> 6));
                                    out += static_cast<char>(0x80 | (code & 0x3F));
                                }
                                else
                                {
                                    out += static_cast<char>(0xE0 | (code >> 12));
                                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                    out += static_cast<char>(0x80 | (code & 0x3F));
                                }
                                break;
                            }
                        }
                        out += esc;   // malformed \u — keep literally
                        break;
                    }
                    default: out += esc; break;
                    }
                    continue;
                }
                out += c;
            }
            return out;
        };

    std::vector<ChurchInfo> churches;
    size_t pos = 0;
    while ((pos = content.find('{', pos)) != std::string::npos)
    {
        size_t objEnd = content.find('}', pos);
        if (objEnd == std::string::npos) break;
        std::string obj = content.substr(pos, objEnd - pos + 1);

        ChurchInfo c;
        c.name = extractField(obj, "name");
        c.thumbnail = extractField(obj, "thumbnail");
        c.front = extractField(obj, "front");
        c.side = extractField(obj, "side");
        c.rear = extractField(obj, "rear");
        if (!c.name.empty())
            churches.push_back(c);

        pos = objEnd + 1;
    }

    std::sort(churches.begin(), churches.end(),
        [](const ChurchInfo& a, const ChurchInfo& b) { return a.name < b.name; });
    return churches;
}

// Searches ProfilesV3 for an existing .sdProfile whose manifest.json has "Name":"<profileName>"
std::wstring FindExistingProfile(const std::wstring& rootPath, const std::string& profileName)
{
    std::string nameKey = "\"Name\":\"" + profileName + "\"";
    for (const auto& entry : fs::directory_iterator(rootPath))
    {
        if (!entry.is_directory() || entry.path().extension() != L".sdProfile") continue;
        std::ifstream f(entry.path() / "manifest.json");
        if (!f.is_open()) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.find(nameKey) != std::string::npos)
            return entry.path().wstring();
    }
    return {};
}

// Writes the page folders (with images + page manifests) and the top-level
// device profile manifest into the given .sdProfile folder. Shared by the
// ProfilesV3 generator and the plugin-packaged .streamDeckProfile generator.
bool WriteProfileContents(const std::wstring& profileFolder,
    const std::wstring& profilesDir,
    const std::string& profileName,
    const DeviceInfo& device,
    int deviceType,
    const std::map<int, KeyAction>& keys,
    const std::string& firstPageGuidReuse)
{
    int keysPerPage = GetKeysPerPage(deviceType);
    int maxKeyIdx = keys.empty() ? 0 : (keys.rbegin()->first + 1);
    int numPages = (maxKeyIdx > 0) ? ((maxKeyIdx + keysPerPage - 1) / keysPerPage) : 1;
    if (numPages < 1) numPages = 1;

    // Generate GUIDs for all pages
    std::vector<std::string> pageGuidsUpper(numPages);
    std::vector<std::string> pageGuidsLower(numPages);
    for (int p = 0; p < numPages; ++p)
    {
        if (p == 0 && !firstPageGuidReuse.empty())
        {
            pageGuidsLower[p] = GuidToLower(firstPageGuidReuse);
            pageGuidsUpper[p] = firstPageGuidReuse;
            for (auto& c : pageGuidsUpper[p]) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
        else
        {
            pageGuidsUpper[p] = GenerateGuid();
            pageGuidsLower[p] = GuidToLower(pageGuidsUpper[p]);
        }
    }

    // Create each page folder, copy images, write page manifest
    for (int p = 0; p < numPages; ++p)
    {
        std::wstring pageFolder = profilesDir + L"\\"
            + std::wstring(pageGuidsUpper[p].begin(), pageGuidsUpper[p].end());

        std::error_code ec;
        fs::create_directories(pageFolder + L"\\Images", ec);
        if (ec)
        {
            std::cerr << "Failed to create page " << (p + 1) << " directories: " << ec.message() << "\n";
            return false;
        }

        int startIdx = p * keysPerPage;
        std::map<int, KeyAction> pageKeys;
        std::map<int, std::string> pageImageRefs, pageImageRefsOn;

        auto copyImage = [&](const std::string& srcPath) -> std::string
            {
                if (srcPath.empty()) return {};
                std::string rawGuid = GenerateGuid();
                std::string filename;
                for (char c : rawGuid)
                    if (c != '-') filename += c;
                filename += ".png";
                std::wstring src(srcPath.begin(), srcPath.end());
                std::wstring dst = pageFolder + L"\\Images\\" + std::wstring(filename.begin(), filename.end());
                std::error_code copyEc;
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, copyEc);
                if (copyEc) std::cerr << "  copy_file error: " << copyEc.message() << " (" << srcPath << ")\n";
                return copyEc ? std::string{} : "Images/" + filename;
            };

        for (const auto& [idx, keyAction] : keys)
        {
            if (idx < startIdx || idx >= startIdx + keysPerPage) continue;
            int localIdx = idx - startIdx;
            pageKeys[localIdx] = keyAction;

            std::string ref = copyImage(keyAction.imagePath);
            if (!ref.empty()) pageImageRefs[localIdx] = ref;
            else if (!keyAction.imagePath.empty())
                std::cerr << "Warning: could not copy image for key " << idx << "\n";

            std::string refOn = copyImage(keyAction.imagePathOn);
            if (!refOn.empty()) pageImageRefsOn[localIdx] = refOn;
            else if (!keyAction.imagePathOn.empty())
                std::cerr << "Warning: could not copy imageOn for key " << idx << "\n";
        }

        std::string pageName = "Page " + std::to_string(p + 1);
        {
            std::ofstream f(pageFolder + L"\\manifest.json");
            if (!f.is_open()) { std::cerr << "Failed to create page manifest.json for " << pageName << "\n"; return false; }
            f << BuildPageManifest(pageName, deviceType, pageKeys, pageImageRefs, pageImageRefsOn);
        }
    }

    // Write device profile manifest (always rewrite — page count may have changed)
    {
        std::ofstream f(profileFolder + L"\\manifest.json");
        if (!f.is_open()) { std::cerr << "Failed to create device manifest.json\n"; return false; }
        f << BuildDeviceManifest(profileName, device, pageGuidsLower);
    }

    return true;
}

bool CreateStreamDeckProfile(const std::string& profileName, int deviceType,
    const std::map<int, KeyAction>& keys)
{
    CoInitialize(nullptr);

    std::wstring rootPath = GetProfilesRootPath();
    if (rootPath.empty())
    {
        std::cerr << "Cannot find AppData path.\n";
        return false;
    }

    // Read device info from the first existing .sdProfile
    DeviceInfo device = FindFirstDevice(rootPath);
    if (device.model.empty())
    {
        std::cerr << "No Stream Deck device found. Ensure the Stream Deck software has run at least once.\n";
        return false;
    }

    if (deviceType < 0)
        deviceType = DeviceTypeFromModel(device.model);
    std::cout << "Device model: " << device.model << "  ->  deviceType=" << deviceType << "\n";

    // Calculate keys per page and number of pages needed
    int keysPerPage = GetKeysPerPage(deviceType);
    int maxKeyIdx = keys.empty() ? 0 : (keys.rbegin()->first + 1);
    int numPages = (maxKeyIdx > 0) ? ((maxKeyIdx + keysPerPage - 1) / keysPerPage) : 1;
    if (numPages < 1) numPages = 1;

    std::cout << "  Keys: " << keys.size() << ", keysPerPage: " << keysPerPage
        << ", pages: " << numPages << "\n";

    // Check if a profile with the same name already exists
    std::wstring profileFolder = FindExistingProfile(rootPath, profileName);
    bool updating = !profileFolder.empty();

    std::wstring profilesDir;
    std::string firstPageGuidReuse;

    if (updating)
    {
        // Read the Current page GUID from the device manifest.
        // The Stream Deck app owns this manifest and may rewrite it at any time,
        // so we MUST match the first page folder name to whatever GUID the manifest contains.
        {
            std::ifstream mf(profileFolder + L"\\manifest.json");
            if (mf.is_open())
            {
                std::string mc((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
                std::string key = "\"Current\":\"";
                size_t pos = mc.find(key);
                if (pos != std::string::npos)
                {
                    pos += key.size();
                    size_t end = mc.find('"', pos);
                    if (end != std::string::npos)
                        firstPageGuidReuse = mc.substr(pos, end - pos);
                }
            }
        }

        // Remove ALL old page folders under Profiles
        profilesDir = profileFolder + L"\\Profiles";
        std::error_code removeEc;
        fs::remove_all(profilesDir, removeEc);

        std::cout << "Updating existing profile: ";
        std::wcout << profileFolder << L"\n";
    }
    else
    {
        // Top-level device profile folder: <GUID>.sdProfile
        std::string deviceProfileGuid = GenerateGuid();
        profileFolder = rootPath + std::wstring(deviceProfileGuid.begin(), deviceProfileGuid.end()) + L".sdProfile";
        profilesDir = profileFolder + L"\\Profiles";

        std::cout << "Creating new profile.\n";
    }

    if (!WriteProfileContents(profileFolder, profilesDir, profileName, device, deviceType, keys, firstPageGuidReuse))
        return false;

    std::wcout << L"Profile \"" << std::wstring(profileName.begin(), profileName.end())
        << (updating ? L"\" updated at:\n  " : L"\" created at:\n  ")
        << profileFolder << L"\n";
    return true;
}

bool CreateStreamDeckProfileFromSettings()
{
    // Resolve %APPDATA%\AhlbornBridge2 paths
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
    {
        std::cerr << "CreateStreamDeckProfileFromSettings: cannot resolve AppData.\n";
        return false;
    }
    std::wstring baseDir = std::wstring(appData) + L"\\AhlbornBridge2";

    // Settings.xml path (narrow string for LoadInstalledOrgans)
    std::wstring settingsW = baseDir + L"\\Settings.xml";
    std::string settingsPath(settingsW.begin(), settingsW.end());

    // Load organs from Settings.xml
    std::vector<OrganInfo> organs = LoadInstalledOrgans(settingsPath);
    if (organs.empty())
    {
        // Cache may be stale from early startup — force a re-scan
        printf("CreateStreamDeckProfileFromSettings: no organs on first read, forcing re-scan...\n");
        ReloadInstalledOrgans();
        organs = LoadInstalledOrgans(settingsPath);
    }
    if (organs.empty())
    {
        printf("CreateStreamDeckProfileFromSettings: no organs found in Settings.xml.\n");
        return false;
    }

    // Icon paths (installed by .iss to %APPDATA%\AhlbornBridge\Icons)
    std::wstring iconsDir = baseDir + L"\\Icons\\";
    std::string iconOff(iconsDir.begin(), iconsDir.end());
    std::string iconOn = iconOff;
    iconOff += "organ-off.png";
    iconOn  += "organ-on.png";

    // Common key template for native AhlbornBridge Stream Deck plugin
    KeyAction keyTemplate;
    keyTemplate.actionUUID = "com.ahlbornbridge.organ.load";
    keyTemplate.titleAlignment = "middle";
    keyTemplate.titleColor = "#ffffff";   // white when Off
    keyTemplate.titleColorOn = "#44ff44"; // green when On
    keyTemplate.imagePath = iconOff;
    keyTemplate.imagePathOn = iconOn;

    // Create one key per organ: organIndex = 1-based, title = organ name
    std::map<int, KeyAction> keys;
    for (int i = 0; i < static_cast<int>(organs.size()); ++i)
    {
        KeyAction key = keyTemplate;
        key.organIndex = i + 1;
        key.title = organs[i].displayName.empty() ? organs[i].title : organs[i].displayName;
        keys[i] = key;
    }

    return CreateStreamDeckProfile("AhlbornBridge", -1, keys);
}

bool CreateStreamDeckSwitchesProfile()
{
    // Load AhlbornSwitches from Settings.xml
    std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
    if (switches.empty())
    {
        printf("CreateStreamDeckSwitchesProfile: no switches found in Settings.xml.\n");
        return false;
    }

    // Icon paths
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return false;
    std::wstring iconsDir = std::wstring(appData) + L"\\AhlbornBridge2\\Icons\\";
    std::string iconOff(iconsDir.begin(), iconsDir.end());
    std::string iconOn = iconOff;
    iconOff += "organ-off.png";
    iconOn  += "organ-on.png";

    std::map<int, KeyAction> keys;
    for (int i = 0; i < static_cast<int>(switches.size()); ++i)
    {
        KeyAction key;
        key.actionUUID     = "com.ahlbornbridge.switch";
        key.fontFamily     = "";
        key.fontSize       = 11;
        key.titleAlignment = "middle";
        key.titleColor     = "#ffffff";
        key.titleColorOn   = "#ffffff";
        // No imagePath — the plugin renders its own SVG
        key.organIndex     = i + 1;  // switchIndex is 1-based
        const std::wstring& wname = switches[i].name;
        key.title = std::string(wname.begin(), wname.end());
        keys[i] = key;
    }

    return CreateStreamDeckProfile("AhlbornBridge Switches", -1, keys);
}

// Zips the CONTENTS of 'srcDir' (its files/subfolders at the archive root, not
// srcDir itself) into 'zipPath' using PowerShell's Compress-Archive, which is
// far more reliable than the async Shell "Compressed Folder" automation.
static bool ZipFolderContents(const std::wstring& srcDir, const std::wstring& zipPath)
{
    std::error_code ec;
    fs::remove(zipPath, ec);

    // Build the ZIP manually so that entry paths use forward slashes. .NET's
    // CreateFromDirectory writes backslash separators on Windows, which Stream
    // Deck (and the ZIP spec) do not accept for nested folders, so the profile
    // would not be recognized. We open a ZipArchive and add each file with a
    // normalized relative name. The script is written to a temp .ps1 file to
    // avoid command-line escaping issues.
    wchar_t appData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    std::wstring scriptPath = std::wstring(appData) + L"\\AhlbornBridge2\\_zipprofile.ps1";

    {
        std::wofstream s(scriptPath);
        if (!s.is_open())
        {
            std::cerr << "ZipFolderContents: cannot write temp script.\n";
            return false;
        }
        s << L"$ErrorActionPreference='Stop'\n";
        s << L"Add-Type -AssemblyName System.IO.Compression\n";
        s << L"Add-Type -AssemblyName System.IO.Compression.FileSystem\n";
        s << L"$src='" << srcDir << L"'\n";
        s << L"$zip='" << zipPath << L"'\n";
        s << L"if (Test-Path $zip) { Remove-Item $zip -Force }\n";
        s << L"$fs=[System.IO.File]::Open($zip,'Create')\n";
        s << L"$archive=New-Object System.IO.Compression.ZipArchive($fs,'Create')\n";
        s << L"Get-ChildItem -Path $src -Recurse -File | ForEach-Object {\n";
        s << L"  $rel=$_.FullName.Substring($src.Length).TrimStart('\\').Replace('\\','/')\n";
        s << L"  $entry=$archive.CreateEntry($rel)\n";
        s << L"  $es=$entry.Open()\n";
        s << L"  $bytes=[System.IO.File]::ReadAllBytes($_.FullName)\n";
        s << L"  $es.Write($bytes,0,$bytes.Length)\n";
        s << L"  $es.Close()\n";
        s << L"}\n";
        s << L"$archive.Dispose()\n";
        s << L"$fs.Close()\n";
    }

    std::wstring cmd =
        L"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""
        + scriptPath + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        std::cerr << "ZipFolderContents: failed to launch PowerShell.\n";
        return false;
    }

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    fs::remove(scriptPath, ec);

    if (exitCode != 0)
    {
        std::cerr << "ZipFolderContents: zip script exited with code " << exitCode << "\n";
        return false;
    }

    // Verify the archive exists and is non-trivial.
    return fs::exists(zipPath, ec) && fs::file_size(zipPath, ec) > 100;
}

// Builds the profile structure into a temp folder and packages it as a
// plugin-bundled <profileName>.streamDeckProfile inside the installed plugin
// folder. This is the ONLY profile format the switchToProfile API can activate.
bool CreateStreamDeckPluginProfile(const std::string& profileName,
    const std::map<int, KeyAction>& keys)
{
    CoInitialize(nullptr);

    std::wstring rootPath = GetProfilesRootPath();
    if (rootPath.empty())
    {
        std::cerr << "CreateStreamDeckPluginProfile: cannot find AppData path.\n";
        return false;
    }

    // Device info is still required so the bundled profile's manifest carries a
    // valid Model/UUID; switchToProfile matches by profile Name + DeviceType.
    DeviceInfo device = FindFirstDevice(rootPath);
    if (device.model.empty())
    {
        std::cerr << "CreateStreamDeckPluginProfile: no Stream Deck device found.\n";
        return false;
    }
    int deviceType = DeviceTypeFromModel(device.model);

    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return false;

    const std::wstring pluginDir = std::wstring(appData)
        + L"\\Elgato\\StreamDeck\\Plugins\\com.ahlbornbridge.organ.sdPlugin";
    if (!fs::exists(pluginDir))
    {
        std::wcerr << L"CreateStreamDeckPluginProfile: plugin folder not found: " << pluginDir << L"\n";
        return false;
    }

    // Build the profile into a temp staging folder: <temp>\<GUID>.sdProfile
    std::string stageGuid = GenerateGuid();
    std::wstring tempBase = std::wstring(appData) + L"\\AhlbornBridge2\\_sdprofile_stage";
    std::error_code ec;
    fs::remove_all(tempBase, ec);
    std::wstring sdProfileFolder = tempBase + L"\\"
        + std::wstring(stageGuid.begin(), stageGuid.end()) + L".sdProfile";
    std::wstring profilesDir = sdProfileFolder + L"\\Profiles";
    fs::create_directories(profilesDir, ec);
    if (ec)
    {
        std::wcerr << L"CreateStreamDeckPluginProfile: cannot create staging dir.\n";
        return false;
    }

    if (!WriteProfileContents(sdProfileFolder, profilesDir, profileName, device, deviceType, keys, ""))
    {
        std::cerr << "CreateStreamDeckPluginProfile: failed to write profile contents.\n";
        return false;
    }

    // Package the .sdProfile contents into <profileName>.streamDeckProfile (a ZIP)
    // placed directly inside the installed plugin folder.
    std::wstring wProfileName(profileName.begin(), profileName.end());
    std::wstring packagePath = pluginDir + L"\\" + wProfileName + L".streamDeckProfile";
    fs::remove(packagePath, ec);

    if (!ZipFolderContents(sdProfileFolder, packagePath))
    {
        std::cerr << "CreateStreamDeckPluginProfile: failed to zip profile package.\n";
        return false;
    }

    fs::remove_all(tempBase, ec);

    std::wcout << L"Plugin profile package created at:\n  " << packagePath << L"\n";
    return true;
}

// Installs the "OrganLoaded" profile by copying the profile package that was
// exported from the Stream Deck app and bundled with the plugin. Dynamically
// generated packages are NOT accepted by Stream Deck (the real export format has
// an outer package.json + nested Profiles/<GUID>.sdProfile layout and a
// Controllers/Actions page format), so we ship the known-good exported file and
// just copy it into the installed plugin folder.
bool CreateStreamDeckOrganLoadedProfile()
{
    // Diagnostic log so first-install failures on other PCs can be traced.
    wchar_t appDataLog[MAX_PATH] = {};
    std::wofstream dbg;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataLog)))
    {
        std::wstring logPath = std::wstring(appDataLog) + L"\\AhlbornBridge2\\OrganLoaded_debug.log";
        dbg.open(logPath, std::ios::trunc);
    }
    auto log = [&dbg](const std::string& m) {
        if (dbg.is_open()) { dbg << std::wstring(m.begin(), m.end()) << L"\n"; dbg.flush(); }
    };

    // Load the 23 IR churches from the installed plugin's churches.json.
    std::vector<ChurchInfo> churches = LoadChurches();
    log("Churches loaded: " + std::to_string(churches.size()));
    if (churches.empty())
    {
        log("FAILED: no churches found in churches.json (check plugin folder deployment).");
        std::cerr << "CreateStreamDeckOrganLoadedProfile: no churches found in churches.json.\n";
        return false;
    }

    // Resolve the installed plugin's IR_thumbnails directory (image source).
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return false;
    std::wstring thumbsDirW = std::wstring(appData)
        + L"\\Elgato\\StreamDeck\\Plugins\\com.ahlbornbridge.organ.sdPlugin\\IR_thumbnails\\";
    std::string thumbsDir(thumbsDirW.begin(), thumbsDirW.end());

    // Determine the device grid so we can place the Current Organ button at
    // slot 0 of EVERY page and pad the final page with black fillers. This is
    // the layout that works reliably on Stream Deck Mobile.
    std::wstring rootPath = GetProfilesRootPath();
    DeviceInfo device = FindFirstDevice(rootPath);
    log("Profiles root: " + std::string(rootPath.begin(), rootPath.end()));
    log("Device model: '" + device.model + "'  uuid: '" + device.uuid + "'");
    if (device.model.empty())
        log("WARNING: no Stream Deck device found. CreateStreamDeckProfile will fail.");
    int deviceType = DeviceTypeFromModel(device.model);
    int keysPerPage = GetKeysPerPage(deviceType);
    if (keysPerPage < 2) keysPerPage = 15;
    int churchesPerPage = keysPerPage - 1;  // slot 0 is the organ button

    // Current Organ (unload) button template.
    KeyAction organTemplate;
    organTemplate.actionUUID = "com.ahlbornbridge.organ.load";
    organTemplate.titleAlignment = "middle";
    organTemplate.titleColor = "#ffffff";
    organTemplate.titleColorOn = "#44ff44";
    organTemplate.organIndex = 0;  // 0 = "current organ" / unload button

    // Build the full key map, paginated: each page starts with the organ
    // button, then up to (keysPerPage-1) churches, padded with ir fillers.
    std::map<int, KeyAction> keys;
    int numPages = (static_cast<int>(churches.size()) + churchesPerPage - 1) / churchesPerPage;
    if (numPages < 1) numPages = 1;

    int churchIdx = 0;
    for (int p = 0; p < numPages; ++p)
    {
        int base = p * keysPerPage;
        // Slot 0: Current Organ button.
        keys[base] = organTemplate;

        // Slots 1..keysPerPage-1: churches then fillers.
        for (int s = 1; s < keysPerPage; ++s)
        {
            KeyAction key;
            key.actionUUID = "com.ahlbornbridge.ir";
            key.titleAlignment = "bottom";
            key.titleColor = "#ffffff";
            key.fontSize = 11;
            if (churchIdx < static_cast<int>(churches.size()))
            {
                const ChurchInfo& c = churches[churchIdx++];
                key.churchName = c.name;
                key.thumbnail = c.thumbnail;
                key.oscFront = c.front;
                key.oscSide = c.side;
                key.oscRear = c.rear;
                if (!c.thumbnail.empty())
                    key.imagePath = thumbsDir + c.thumbnail;
            }
            // else: empty IR action = black filler (plugin paints it black)
            keys[base + s] = key;
        }
    }

    bool ok = CreateStreamDeckProfile("OrganLoaded", -1, keys);
    log(std::string("CreateStreamDeckProfile returned: ") + (ok ? "SUCCESS" : "FAILED"));
    if (ok)
        std::cout << "OrganLoaded profile generated dynamically for "
                  << churches.size() << " churches across " << numPages << " page(s).\n";
    return ok;
}

// Creates any of the three standard profiles that are not already present in
// the ProfilesV3 store. Idempotent, so it is safe to call on every startup;
// existing profiles are left untouched. Intended for first-install bootstrap.
bool EnsureStreamDeckProfilesInstalled()
{
    std::wstring rootPath = GetProfilesRootPath();
    if (rootPath.empty())
    {
        std::cerr << "EnsureStreamDeckProfilesInstalled: cannot find ProfilesV3 path.\n";
        return false;
    }

    // If no Stream Deck device is registered yet we cannot generate profiles;
    // bail out quietly so startup is not blocked.
    DeviceInfo device = FindFirstDevice(rootPath);
    if (device.model.empty())
    {
        std::cout << "EnsureStreamDeckProfilesInstalled: no Stream Deck device found; skipping.\n";
        return false;
    }

    auto profileExists = [&](const std::string& name) -> bool
        {
            return !FindExistingProfile(rootPath, name).empty();
        };

    bool allOk = true;

    if (!profileExists("AhlbornBridge"))
    {
        std::cout << "First install: creating 'AhlbornBridge' profile...\n";
        if (!CreateStreamDeckProfileFromSettings()) allOk = false;
    }

    if (!profileExists("AhlbornBridge Switches"))
    {
        std::cout << "First install: creating 'AhlbornBridge Switches' profile...\n";
        if (!CreateStreamDeckSwitchesProfile()) allOk = false;
    }

    if (!profileExists("OrganLoaded"))
    {
        std::cout << "First install: creating 'OrganLoaded' profile...\n";
        if (!CreateStreamDeckOrganLoadedProfile()) allOk = false;
    }

    return allOk;
}


/*
int main()
{
    // Load organs from Settings.xml
    std::vector<OrganInfo> organs = LoadInstalledOrgans(
        "C:\\Users\\paolo\\AppData\\Roaming\\AhlbornBridge\\Settings.xml");
    if (organs.empty())
    {
        std::cerr << "No organs found in Settings.xml\n";
        return 1;
    }

    // Common MIDI key template (shared settings for all organ keys)
    KeyAction keyTemplate;
    keyTemplate.actionUUID = "se.trevligaspel.midi.genericmidi";
    keyTemplate.midi.outputPort = "Tx[01]";
    keyTemplate.midi.inputPort = "Rx[02]";
    keyTemplate.midi.messageType = "CC";
    keyTemplate.midi.ccNumber = "81";
    keyTemplate.midi.valueOff = "0";
    keyTemplate.midi.buttonMode = "Toggle";
    keyTemplate.midi.actionWhenReceived = "2";  // Value 127 = On, 0 = Off, others ignored
    keyTemplate.midi.channel = "15"; // MIDI channel 16 (0-indexed)
    keyTemplate.titleAlignment = "middle";
    keyTemplate.titleColor = "#ffffff";  // white when Off
    keyTemplate.titleColorOn = "#44ff44";  // green when On
    keyTemplate.imagePath = "C:\\Users\\paolo\\source\\repos\\AhlbornBridge\\x64\\Release\\Icons\\Azio1_OFF.png";
    keyTemplate.imagePathOn = "C:\\Users\\paolo\\source\\repos\\AhlbornBridge\\x64\\Release\\Icons\\Azio1_ON.png";

    // Create one key per organ: title = organ display name, valueOn = organ id
    std::map<int, KeyAction> keys;
    for (int i = 0; i < static_cast<int>(organs.size()); ++i)
    {
        KeyAction key = keyTemplate;
        std::string raw = organs[i].displayName.empty()
            ? ("ORGAN " + std::to_string(i + 1))
            : organs[i].displayName;
        key.title = WrapTitle(raw);
        key.midi.valueOn = organs[i].id;
        keys[i] = key;
    }

    if (CreateStreamDeckProfile("Paolo_Test4", 4, keys))
        std::cout << "Profile created successfully.\n";
    else
        std::cout << "Failed to create profile.\n";

    return 0;
}
*/