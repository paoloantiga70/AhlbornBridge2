#pragma once

// ---------------------------------------------------------------------------
// Midi2Endpoint
// ---------------------------------------------------------------------------
// Creates a Windows MIDI Services (MIDI 2.0) app-to-app loopback endpoint
// named "AhlbornBridge Virtual Port".
//
// Call EnableMidi2Endpoint() once during startup (after initMidiState).
// Call ForwardToMidi2Endpoint() from EnqueueMidiOutMessage or MidiInProc to
// publish every incoming WinMM message as a UMP word on the virtual port.
// Call DisableMidi2Endpoint() on shutdown.
//
// Requires Windows 11 with Windows MIDI Services installed.
// The functions are safe to call on older systems: they return false and do
// nothing if the WinRT API is unavailable at runtime.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cstdint>

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
