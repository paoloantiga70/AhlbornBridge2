#pragma once

#define WIN32_LEAN_AND_MEAN

// * Fissatore key */
#define AHLBORN_FISSATORE_CC 0x47
#define AHLBORN_FISSATORE_DN 0x46
#define AHLBORN_FISSATORE_UP 0x06
//
#define CC_ch16 0xBF
#define ACTIVE_SENSING 0xFE
#define ALL_RESET 0xFF

// * Control Change 80 (load favorite organ)
#define BF_0x50 0x50
// * Control Change 81 (load installed organ by index)
#define BF_0x51 0x51

#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma warning(disable: 4996)

// Global functions ***************************
bool initMidiState();
// Dynamic multi-device API (new)
void SetAssignedMidiInputNames(const std::vector<std::wstring>& names);
void SetAssignedMidiOutputNames(const std::vector<std::wstring>& names);
bool RefreshMidiInputDevice();
// Legacy single-slot wrappers kept for any remaining callers
bool SwitchMidiInputDevice(UINT deviceId);
bool SwitchMidiInput2Device(UINT deviceId);
bool SwitchMidiOutputDevice(UINT deviceId);
bool SwitchMidiOutput2Device(UINT deviceId);
void CloseMidiInputDeviceOnly();
void CloseMidiInput2DeviceOnly();
void CloseMidiOutputDeviceOnly();
void CloseMidiOutput2DeviceOnly();
bool IsMidiInputDeviceOpen();
bool IsMidiInput2DeviceOpen();
bool IsMidiOutputDeviceOpen();
bool IsMidiOutput2DeviceOpen();
// Per-slot MIDI activity: returns GetTickCount() of last message on slot i
// (0=hMidiIn, 1=hMidiIn2, 2+=extras). Returns 0 if never received.
DWORD GetMidiInputSlotLastMsg(int slotIndex);
// Per-slot MIDI output activity: returns GetTickCount() of last message sent on slot i.
DWORD GetMidiOutputSlotLastMsg(int slotIndex);
// Returns the last activity timestamp for the currently open input/output handle
// that matches the given device name. Returns 0 if not open or never active.
DWORD GetMidiInputLastMsgByDeviceName(const std::wstring& deviceName);
DWORD GetMidiOutputLastMsgByDeviceName(const std::wstring& deviceName);
// Returns the GetTickCount() timestamp of the last channel-voice message
// received on "Hauptwerk Virtual (B)". Used for the activity LED in the
// internal bridge ports view.  Returns 0 if no message has arrived yet.
DWORD GetHauptwerkVirtualBLastMsgTime();
void RefreshMidiDeviceStatus();
void CleanupMidiLocks();
void CALLBACK MidiInProc(
    HMIDIIN hMidiIn,
    UINT wMsg,
    DWORD_PTR dwInstance,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2);

void clearChannel(int ch);
bool startsWithFe();
DWORD WINAPI WatchdogThread(LPVOID);
bool anyNoteActive();
void clearAllNotes();
bool IsChannelActive(int ch);
bool IsOutputChannelActive(int ch);
void ClearOutputNotes();
// Enqueue a raw MIDI message for the output worker thread.
bool EnqueueMidiOutMessage(DWORD msg);
// Enable/disable verbose MIDI monitor tracing on debug console.
void SetMidiMonitorEnabled(bool enabled);
bool IsMidiMonitorEnabled();
// Enable/disable Active Sensing (0xFE) filtering in MIDI monitor logs.
void SetMidiMonitorFilterFeEnabled(bool enabled);
bool IsMidiMonitorFilterFeEnabled();
// Set the output device name used by the Active Sensing sender; empty = use default "Default App Loopback (A)".
void SetActiveSensingOutputName(const std::wstring& name);
// Request the tray icon to enter the "starting Hauptwerk" flashing state.
void TriggerTrayIconStartupFlash();
// Stop the tray icon flashing animation.
void StopTrayIconFlashing();
// Enqueue a deferred command to load an installed organ by 1-based index.
void EnqueueLoadInstalledOrgan(int index);
// Enqueue a deferred command to unload the current organ.
void EnqueueUnloadOrgan();
// Enqueue a deferred command to toggle Bidule window visibility.
void EnqueueToggleBidule();
enum class FeStatus
{
    None,
    Interrupted,
    Active
};
FeStatus GetFeStatus();

// Global variables *********************************

constexpr int kChannels = 16;
constexpr int kNotes = 128;

constexpr std::chrono::milliseconds feTimeout(500);
constexpr std::chrono::milliseconds disconnectConfirm(1500);

extern HMIDIIN hMidiIn;   // slot-0 handle kept for watchdog / legacy code
extern HMIDIIN hMidiIn2;
extern HMIDIOUT hMidiOut;  // slot-0 handle kept for legacy code
extern HMIDIOUT hMidiOut2;
extern HANDLE hThread;
extern std::atomic<bool> g_midiRouterEnabled;
extern bool is_organ_loaded;
extern std::atomic<bool> g_inputDeviceOpen;   // true if at least one input open
extern std::atomic<bool> g_input2DeviceOpen;
extern std::atomic<bool> g_outputDeviceOpen;  // true if at least one output open
extern std::atomic<bool> g_output2DeviceOpen;
extern std::atomic<bool> g_isLoadingOrgan;
extern std::atomic<bool> g_activeSensingEnabled;
extern std::wstring g_activeSensingOutputName;
extern std::atomic<int> g_currentLoadedFavoriteIndex;
extern std::atomic<int> g_currentLoadedInstalledOrganIndex;
extern std::atomic<int> g_streamDeckLoadingInstalledOrganIndex;
extern std::atomic<int> g_streamDeckLoadingProgress;
extern std::atomic<bool> g_streamDeckBiduleSplashActive;

using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

extern std::atomic<uint64_t> noteState[kChannels][2];
extern std::atomic<uint64_t> outputNoteState[kChannels][2];
extern std::atomic<TimePoint> lastActiveSensingTime;
extern std::atomic<bool> running;
extern TimePoint programStart;
extern std::atomic<TimePoint> lastFeTime;

enum class DeviceState
{
    Connected,
    Disconnected
};

enum class TrayIconImageStatus
{
	Unknown,
	Disabled,
	Standby,
	Online,
	Error,
	Offline
};

extern std::atomic<DeviceState> deviceState;
extern std::atomic<HWND> g_hauptwerkMainWindow;
extern std::atomic<bool> g_hauptwerkKeyHeld;
extern std::atomic<TrayIconImageStatus> g_trayIconImageStatus;
extern std::wstring g_hauptwerkOrganTitle;
extern std::atomic<bool> g_autoDetectMidiSettingsActive;

// Inline functions ******************************************************

inline void setNoteOn(int ch, int note)
{
    int block = note / 64;
    int bit = note % 64;
    noteState[ch][block].fetch_or(1ULL << bit, std::memory_order_relaxed);
}

inline void setNoteOff(int ch, int note)
{
    int block = note / 64;
    int bit = note % 64;
    noteState[ch][block].fetch_and(~(1ULL << bit), std::memory_order_relaxed);
}

inline void setOutputNoteOn(int ch, int note)
{
    int block = note / 64;
    int bit = note % 64;
    outputNoteState[ch][block].fetch_or(1ULL << bit, std::memory_order_relaxed);
}

inline void setOutputNoteOff(int ch, int note)
{
    int block = note / 64;
    int bit = note % 64;
    outputNoteState[ch][block].fetch_and(~(1ULL << bit), std::memory_order_relaxed);
}