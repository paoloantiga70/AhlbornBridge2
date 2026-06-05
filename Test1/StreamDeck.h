#pragma once

#include <windows.h>

#define LOAD_FAVORITE_ORGAN_1   1
#define LOAD_FAVORITE_ORGAN_2   2
#define LOAD_FAVORITE_ORGAN_3   3
#define LOAD_FAVORITE_ORGAN_4   4
#define LOAD_FAVORITE_ORGAN_5   5
#define LOAD_FAVORITE_ORGAN_6   6
#define LOAD_FAVORITE_ORGAN_7   7
#define LOAD_FAVORITE_ORGAN_8   8

// Sends MIDI message BF 50 00 (CC ch16, CC#80, value 0) on the current output device.
void SendUnloadOrganMidiMessage(bool resetSwitches = true);

// Named pipe server for Stream Deck plugin IPC
void StartStreamDeckPipeServer();
void StopStreamDeckPipeServer();

// Notify the connected Stream Deck plugin of state changes.
// Safe to call even when no plugin is connected.
void NotifyStreamDeckOrganList();
void NotifyStreamDeckOrganState(int loadedIndex);
void NotifyStreamDeckOrganUnloaded(bool resetSwitches = true);
void NotifyStreamDeckSwitchState(int switchIndex, bool isOn);
void NotifyStreamDeckSwitchesForOrgan(int loadedIndex);
void NotifyStreamDeckActiveSensingState(bool enabled);

struct AhlbornSwitchInfo;

// Updates organ titles in the existing SD profile files and restarts Stream Deck.
// Called automatically when organ names change.
void UpdateStreamDeckProfileTitles();
void SendAhlbornSwitchControlChange(int channel, int controlChange, int value);
// Closes the physical MIDI output handle used for console LED sync. Call at app shutdown.
void ClosePhysicalOutputForSwitches();

// Mirrors a MIDI message received from "Hauptwerk Virtual (B)" to the physical
// console output for LED register sync.  Called from MidiInProc; best-effort.
void MirrorHauptwerkOutputToConsole(DWORD midiMsg);

// Refreshes the internal cache of AhlbornSwitchInfo used by MirrorHauptwerkOutputToConsole.
// Call after organ load so the cache reflects the current switch definitions.
void RefreshSwitchesCache();