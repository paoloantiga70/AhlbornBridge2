#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>

struct InstalledOrganInfo
{
    std::wstring id;
    std::wstring displayName;
    std::wstring name;
    std::wstring uniqueOrganId;
    std::wstring outputDeviceId;
    std::wstring biduleProfile;
    int numberChannels = 0;
};

struct AudioDeviceInfo
{
    std::wstring id;
    std::wstring name;
};

struct AhlbornSwitchInfo
{
    std::wstring name;
    int channel = 1;
    int controlChange = 0;
    int valueOn = 127;
    int valueOff = 0;
    bool momentary = false;
};

// ---------- Dynamic multi-device assignment (new API) ----------
// Load/save ordered lists of assigned device names for inputs and outputs.
std::vector<std::wstring> LoadAssignedMidiInputNames();
std::vector<std::wstring> LoadAssignedMidiOutputNames();
bool SaveAssignedMidiInputNames(const std::vector<std::wstring>& names);
bool SaveAssignedMidiOutputNames(const std::vector<std::wstring>& names);
bool SaveFixedHauptwerkOutputName(const std::wstring& name);
std::wstring LoadFixedHauptwerkOutputName();
std::vector<InstalledOrganInfo> LoadInstalledOrganInfos();
std::vector<AudioDeviceInfo> LoadAudioOutputDevices();
bool SaveInstalledOrganOutputDevice(const std::wstring& uniqueOrganId, const std::wstring& outputDeviceId);
bool SaveInstalledOrganBiduleProfile(const std::wstring& uniqueOrganId, const std::wstring& biduleProfile);
bool EnsureHauptwerkAudioOutputDeviceId(const std::wstring& outputDeviceId, bool* changed = nullptr);
bool IsHauptwerkAudioOutputDeviceIdAligned(const std::wstring& outputDeviceId);

// Write the Hauptwerk Config.Config_Hauptwerk_xml to reflect the given
// MIDI input and output device names.  Looks up each name in the
// PreviouslySeenDevice list (typ=2 for inputs, typ=3 for outputs) to find
// the matching id, then replaces EnabledMIDIInputPort / EnabledMIDIOutputPort.
// Returns true if the file was written successfully.
bool WriteHauptwerkMidiConfig(const std::vector<std::wstring>& inputNames,
                              const std::vector<std::wstring>& outputNames);

// Configure AudioOutputSystem and AudioOutputUnit sections in the Hauptwerk
// Config.Config_Hauptwerk_xml with the fixed preset for this installation.
// Called once on first install after MIDI device detection.
// The <dev> field in AudioOutputUnit entries is left empty and must be
// filled later when the audio device ID is known.
bool WriteHauptwerkAudioConfig();

// ---------- Legacy wrappers (kept for backward compat) ----------
bool SaveSelectedDeviceId(UINT deviceId);
bool LoadSelectedDeviceId(UINT& deviceId);
bool SaveSelectedInput2DeviceId(UINT deviceId);
bool LoadSelectedInput2DeviceId(UINT& deviceId);
bool SaveSelectedOutputDeviceId(UINT deviceId);
bool LoadSelectedOutputDeviceId(UINT& deviceId);
bool SaveSelectedOutput2DeviceId(UINT deviceId);
bool LoadSelectedOutput2DeviceId(UINT& deviceId);
bool SaveMidiRouterEnabled(bool enabled);
bool LoadMidiRouterEnabled(bool& enabled);
bool SaveCloseSettingsOnDisconnect(bool enabled);
bool LoadCloseSettingsOnDisconnect(bool& enabled);
bool SaveShowDebugConsole(bool enabled);
bool LoadShowDebugConsole(bool& enabled);
bool SaveCheckForUpdateOnStart(bool enabled);
bool LoadCheckForUpdateOnStart(bool& enabled);
bool SaveBidulePath(const std::wstring& path);
bool LoadBidulePath(std::wstring& path);
bool SaveBiduleCloseOnUnload(bool enabled);
bool LoadBiduleCloseOnUnload(bool& enabled);
bool SaveActiveSensingEnabled(bool enabled);
bool LoadActiveSensingEnabled(bool& enabled);
bool SaveActiveSensingOutputName(const std::wstring& name);
bool LoadActiveSensingOutputName(std::wstring& name);
void RefreshSettingsFile();

// Force a re-read of the standby organ names from the Hauptwerk config.
// Call this when Hauptwerk is launched or detected running so the cache
// is refreshed with up-to-date data.
void ReloadStandbyOrgans();

// Force a re-scan of installed organs from the OrganDefinitions folder
// and persist the updated list to Settings.xml.
void ReloadInstalledOrgans();

// Returns the standby organ names (1..32) read from Settings.xml.
// The vector always has 32 entries; unused slots are empty strings.
// Slots 0..7  correspond to Standby_Organ01..08 (Hauptwerk config nodes).
// Slots 8..31 correspond to sborg09..sborg32 (Hauptwerk config nodes).
std::vector<std::wstring> LoadStandbyOrganNames();

// Returns the names of all installed organs found in the OrganDefinitions
// folder, as written to the <InstalledOrgans> section of Settings.xml.
std::vector<std::wstring> LoadInstalledOrganNames();
std::vector<AhlbornSwitchInfo> LoadAhlbornSwitches();
bool SaveOrganSwitchState(const std::wstring& uniqueOrganId, int switchIndex, bool isOn);
std::map<int, bool> LoadOrganSwitchStates(const std::wstring& uniqueOrganId);

// Detect / configure Hauptwerk installation folders.
// On first run, checks the default install path or shows a folder picker.
// Reads FileLocations to extract UserData, SampleSets and WorkingFiles paths.
// Returns true if paths were successfully configured.
bool InitHauptwerkPaths();

// Load the saved Hauptwerk application root folder from Settings.xml.
bool LoadHauptwerkAppPath(std::wstring& path);

// Returns the physical MIDI output fixed at first installation and always
// preserved in Hauptwerk's EnabledMIDIOutputPort list.
std::wstring LoadPrimaryHauptwerkOutputName();

// Read actual enabled MIDI input and output ports straight from Hauptwerk's Config.Config_Hauptwerk_xml
void ReadActualHauptwerkMidiPorts(std::vector<std::wstring>& inputNames, std::vector<std::wstring>& outputNames);

// Start/stop a background thread that monitors the OrganDefinitions folder
// for file additions or removals and automatically calls ReloadInstalledOrgans().
void StartOrganFolderWatcher();
void StopOrganFolderWatcher();

// Load/save the enabled (open) state of each MIDI device LED toggle.
bool LoadMidiInput1DeviceEnabled(bool& enabled);
bool LoadMidiInput2DeviceEnabled(bool& enabled);
bool LoadMidiOutput1DeviceEnabled(bool& enabled);
bool LoadMidiOutput2DeviceEnabled(bool& enabled);
bool SaveMidiInput1DeviceEnabled(bool enabled);
bool SaveMidiInput2DeviceEnabled(bool enabled);
bool SaveMidiOutput1DeviceEnabled(bool enabled);
bool SaveMidiOutput2DeviceEnabled(bool enabled);

bool SaveStreamDeckSettings(int ccNumber, const std::wstring& midiOut, const std::wstring& midiIn);
bool LoadStreamDeckSettings(int& ccNumber, std::wstring& midiOut, std::wstring& midiIn);
bool SaveStreamDeckPipeServerEnabled(bool enabled);
bool LoadStreamDeckPipeServerEnabled(bool& enabled);

// Returns the path to the AhlbornBridge2 settings directory in the current user's AppData\Roaming.
// Always use this instead of hardcoded paths so it works correctly on any user account.
std::wstring GetSettingsDirPath();
