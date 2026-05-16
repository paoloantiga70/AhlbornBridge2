#pragma once

#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <map>
#include <vector>
#include <cctype>

#pragma comment(lib, "shell32.lib")

// Types and function declarations for StreamDeck profile generation

struct DeviceInfo
{
    std::string model;
    std::string uuid;
};

std::wstring GetProfilesRootPath();

// Generates a simple GUID string (without braces)
std::string GenerateGuid();

// Converts a GUID string to lowercase
std::string GuidToLower(const std::string& guid);

// Reads device model and UUID from <profile>.sdProfile\manifest.json
DeviceInfo ReadDeviceInfo(const std::filesystem::path& sdProfilePath);

// Finds the first valid device profile in ProfilesV3
DeviceInfo FindFirstDevice(const std::wstring& rootPath);

// Maps a device model string to the deviceType integer used in BuildPageManifest
int DeviceTypeFromModel(const std::string& model);

// Returns the number of keys per page for the given device type
int GetKeysPerPage(int deviceType);

// MIDI action settings for the "se.trevligaspel.midi" plugin
struct MidiSettings
{
    std::string outputPort;
    std::string inputPort;
    std::string messageType;
    std::string ccNumber;
    std::string valueOn;
    std::string valueOff;
    std::string note;
    std::string buttonMode;
    std::string actionWhenReceived;
    std::string channel;
};

// Action types for Stream Deck keys
struct KeyAction
{
    std::string title;
    std::string actionUUID;
    std::string url;
    std::string hotkey;
    std::string imagePath;
    std::string imagePathOn;
    std::string titleAlignment;
    std::string titleColor;
    std::string titleColorOn;
    std::string fontFamily = "Arial";
    int fontSize = 11;
    int organIndex = 0;         // 1-based organ index (for com.ahlbornbridge.organ.load)
    MidiSettings midi;
};

// Word-wraps text for Stream Deck button titles.
// maxCharsPerLine = max characters per line, maxLines = max number of lines.
// Returns a string with \n line separators suitable for JSON title fields.
std::string WrapTitle(const std::string& text, int maxCharsPerLine = 10, int maxLines = 6);

// Reads the installed version of a plugin from its manifest.json
std::string GetInstalledPluginVersion(const std::string& pluginUUID);

// Builds the Settings JSON for a MIDI Generic action
std::string BuildMidiSettingsJson(const MidiSettings& m);

// Builds the compact JSON block for a single key action (V3 format)
std::string BuildActionJson(const KeyAction& action, const std::string& imageRef = "", const std::string& imageRefOn = "");

// Builds Profiles\<GUID>\manifest.json (page manifest, V3 compact format)
std::string BuildPageManifest(const std::string& pageName, int deviceType,
    const std::map<int, KeyAction>& keys,
    const std::map<int, std::string>& imageRefs = {},
    const std::map<int, std::string>& imageRefsOn = {});

// Builds the top-level <GUID>.sdProfile\manifest.json (device profile manifest, V3 compact format)
std::string BuildDeviceManifest(const std::string& profileName,
    const DeviceInfo& device, const std::vector<std::string>& pageGuidsLower);

// Organ entry loaded from Settings.xml <InstalledOrgans>
struct OrganInfo
{
    std::string id;
    std::string title;
    std::string displayName;
};

// Reads all <Organ id=...> nodes from <InstalledOrgans> in Settings.xml
std::vector<OrganInfo> LoadInstalledOrgans(const std::string& xmlPath);

// Searches ProfilesV3 for an existing .sdProfile with the given name.
// Returns the full path to the .sdProfile folder, or empty if not found.
std::wstring FindExistingProfile(const std::wstring& rootPath, const std::string& profileName);

bool CreateStreamDeckProfile(const std::string& profileName, int deviceType = -1,
    const std::map<int, KeyAction>& keys = {});

// Creates the Stream Deck profile from the installed organs in Settings.xml.
// Called once on first launch after Settings.xml has been written.
bool CreateStreamDeckProfileFromSettings();


