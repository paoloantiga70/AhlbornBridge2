#pragma once

// ---------------------------------------------------------------------------
// Midi2Endpoint
// ---------------------------------------------------------------------------
// Creates two Windows MIDI Services (MIDI 2.0) app-to-app loopback pairs:
//   Pair A: "AhlbornBridge Virtual Port" / "(B)"
//           Bridge → Hauptwerk path (Stream Deck switch CCs).
//   Pair B: "Hauptwerk Virtual" / "Hauptwerk Virtual (B)"
//           Hauptwerk output → Bridge monitoring path.
//           Hauptwerk must be configured to output to "Hauptwerk Virtual (A)".
//           The bridge opens "Hauptwerk Virtual (B)" as an additional input
//           and mirrors incoming messages to the physical console output.
//
// Call EnableMidi2Endpoint() once during startup (after initMidiState).
// Call ForwardToMidi2Endpoint() for messages the bridge sends to Hauptwerk.
// Call DisableMidi2Endpoint() on shutdown.
//
// Requires Windows 11 with Windows MIDI Services installed.
// The functions are safe to call on older systems: they return false and do
// nothing if the WinRT API is unavailable at runtime.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cstdint>
#include <string>

// Returns true if Windows MIDI Services (midisrv) is installed on this system.
// Use this for prerequisite checks before attempting to create virtual ports.
bool IsMidi2ServiceInstalled();

// Returns true if the virtual MIDI 2.0 endpoint was successfully created.
bool EnableMidi2Endpoint();

// Publishes a legacy WinMM short message (packed DWORD) as a 32-bit UMP
// MIDI 1.0 Channel Voice message on the virtual endpoint.
// Safe to call from any thread; internally lock-free.
void ForwardToMidi2Endpoint(DWORD wmidiMsg);

// Tears down the virtual endpoint and releases all MIDI Services resources.
void DisableMidi2Endpoint();

// Returns true if the endpoint is currently active.
bool IsMidi2EndpointEnabled();

// Returns the GetTickCount() timestamp of the last message forwarded to the
// virtual endpoint, or 0 if no message has been forwarded yet.
DWORD GetMidi2EndpointLastMsgTime();

// Returns the WinMM device name of the "Hauptwerk Virtual (B)" loopback side,
// which the bridge should open as an additional MIDI input to monitor
// Hauptwerk's own output.  Returns an empty string if the endpoint has not
// been created (e.g. Windows MIDI Services not installed).
std::wstring GetHauptwerkVirtualBDeviceName();

