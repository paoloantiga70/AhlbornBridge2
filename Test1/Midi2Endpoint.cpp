// ---------------------------------------------------------------------------
// Midi2Endpoint.cpp
// ---------------------------------------------------------------------------
// Implements a Windows MIDI Services (MIDI 2.0) virtual loopback endpoint.
// All WinRT calls are wrapped in try-catch so the application continues to
// run normally on systems where Windows MIDI Services is not installed.
// ---------------------------------------------------------------------------

#include "Midi2Endpoint.h"
#include <windows.h>
#include <winsvc.h>

// ---- C++/WinRT ----
#define WINRT_LEAN_AND_MEAN
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

// ---- Windows MIDI Services SDK bootstrapper ----
#include <winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp>

// ---- Windows MIDI Services C++/WinRT projection headers ----
#include <winrt/Microsoft.Windows.Devices.Midi2.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Endpoints.Loopback.h>

#include <atomic>
#include <cstdio>

// ---------------------------------------------------------------------------
// Namespace aliases
// ---------------------------------------------------------------------------
namespace midi2    = winrt::Microsoft::Windows::Devices::Midi2;
namespace loopback = winrt::Microsoft::Windows::Devices::Midi2::Endpoints::Loopback;

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------
namespace
{
	std::atomic<bool>             g_enabled{ false };
	midi2::MidiSession            g_session{ nullptr };
	midi2::MidiEndpointConnection g_conn{ nullptr };
	winrt::guid                   g_associationId{};
	winrt::hstring                g_endpointDeviceId{};

	constexpr wchar_t kEndpointName[]    = L"AhlbornBridge Virtual Port";
	constexpr wchar_t kEndpointUniqueId[] = L"ahlbornbridge-virtual";
	constexpr wchar_t kSessionName[]     = L"AhlbornBridge";

	bool IsMidi2EndpointOptInEnabled()
	{
		// Allow explicit override via environment variable.
		wchar_t value[16] = {};
		DWORD len = GetEnvironmentVariableW(L"AHLBORN_ENABLE_MIDI2", value, _countof(value));
		if (len > 0)
		{
			wchar_t c = value[0];
			if (c == L'0' || c == L'N' || c == L'n' || c == L'F' || c == L'f')
			{
				printf("[Midi2] Virtual ports disabled by environment variable (AHLBORN_ENABLE_MIDI2=0).\n");
				return false;
			}
			printf("[Midi2] Virtual ports enabled by environment variable (AHLBORN_ENABLE_MIDI2=1).\n");
			return true;
		}

		// Auto-detect: check if Windows MIDI Services is installed via SCM,
		// without loading any WinRT/SDK code that could cause ABI mismatch crashes.
		SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (!hScm)
		{
			printf("[Midi2] Cannot open SCM — virtual ports disabled.\n");
			return false;
		}

		bool serviceFound = false;
		for (const wchar_t* svcName : { L"MidiSrv2", L"WindowsMidiService", L"midisrv" })
		{
			SC_HANDLE hSvc = OpenServiceW(hScm, svcName, SERVICE_QUERY_STATUS);
			if (hSvc) { serviceFound = true; CloseServiceHandle(hSvc); break; }
		}
		CloseServiceHandle(hScm);

		if (serviceFound)
			printf("[Midi2] Windows MIDI Services detected — virtual ports will be created.\n");
		else
			printf("[Midi2] Windows MIDI Services not installed on this PC — virtual ports unavailable.\n"
				   "[Midi2] Install Windows MIDI Services from https://aka.ms/midisrv to enable them.\n");

		return serviceFound;
	}
}

// ---------------------------------------------------------------------------
// IsMidi2ServiceInstalled (public)
// ---------------------------------------------------------------------------
bool IsMidi2ServiceInstalled()
{
	SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!hScm)
		return false;

	bool serviceFound = false;
	for (const wchar_t* svcName : { L"MidiSrv2", L"WindowsMidiService", L"midisrv" })
	{
		SC_HANDLE hSvc = OpenServiceW(hScm, svcName, SERVICE_QUERY_STATUS);
		if (hSvc) { serviceFound = true; CloseServiceHandle(hSvc); break; }
	}
	CloseServiceHandle(hScm);
	return serviceFound;
}

// ---------------------------------------------------------------------------
// EnableMidi2Endpoint
// ---------------------------------------------------------------------------
bool EnableMidi2Endpoint()
{
	if (g_enabled.load())
		return true;

	if (!IsMidi2EndpointOptInEnabled())
	{
		// IsMidi2EndpointOptInEnabled already printed the reason (service not found or env var=0).
		return false;
	}

	try
	{
		// --- 1. Initialize WinRT apartment on this thread ---
		winrt::init_apartment();

		// --- 2. Bootstrap the Windows MIDI Services SDK runtime ---
		Microsoft::Windows::Devices::Midi2::Initialization::MidiDesktopAppSdkInitializer initializer;

		if (!initializer.IsServiceInstalled())
		{
			printf("[Midi2] Windows MIDI Services not installed on this system.\n");
			return false;
		}

		if (!initializer.InitializeSdkRuntime())
		{
			printf("[Midi2] Failed to initialize Windows MIDI Services SDK runtime.\n");
			return false;
		}

		if (!initializer.EnsureServiceAvailable())
		{
			printf("[Midi2] Windows MIDI Services is not available (service not running).\n");
			return false;
		}

		// --- 3. Open a MIDI session ---
		g_session = midi2::MidiSession::Create(kSessionName);
		if (g_session == nullptr)
		{
			printf("[Midi2] Failed to create MIDI session.\n");
			return false;
		}

		// --- 4. Define the loopback endpoint pair ---
		// MidiLoopbackEndpointDefinition is a plain struct: { Name, UniqueId, Description }
		loopback::MidiLoopbackEndpointDefinition defA{};
		defA.Name        = kEndpointName;
		defA.UniqueId    = kEndpointUniqueId;
		defA.Description = L"AhlbornBridge MIDI 2.0 loopback endpoint (A)";

		loopback::MidiLoopbackEndpointDefinition defB{};
		defB.Name        = L"AhlbornBridge Virtual Port (B)";
		defB.UniqueId    = L"ahlbornbridge-virtual-b";
		defB.Description = L"AhlbornBridge MIDI 2.0 loopback endpoint (B)";

		winrt::guid associationId = winrt::Windows::Foundation::GuidHelper::CreateNewGuid();

		loopback::MidiLoopbackEndpointCreationConfig creationConfig{ associationId, defA, defB };

		auto result = loopback::MidiLoopbackEndpointManager::CreateTransientLoopbackEndpoints(
			creationConfig);

		if (!result.Success())
		{
			printf("[Midi2] Failed to create loopback endpoint: %ls\n",
				result.ErrorInformation().c_str());
			g_session.Close();
			g_session = nullptr;
			return false;
		}

		g_associationId    = result.AssociationId();
		g_endpointDeviceId = result.EndpointDeviceIdA();

		// --- 5. Connect to the loopback endpoint ---
		g_conn = g_session.CreateEndpointConnection(g_endpointDeviceId);
		if (g_conn == nullptr)
		{
			printf("[Midi2] Failed to connect to loopback endpoint.\n");
			loopback::MidiLoopbackEndpointRemovalConfig removal{ g_associationId };
			loopback::MidiLoopbackEndpointManager::RemoveTransientLoopbackEndpoints(removal);
			g_session.Close();
			g_session = nullptr;
			return false;
		}

		g_conn.Open();

		g_enabled = true;
		printf("[Midi2] Virtual endpoint created: %ls\n", g_endpointDeviceId.c_str());
		return true;
	}
	catch (winrt::hresult_error const& ex)
	{
		printf("[Midi2] WinRT exception in EnableMidi2Endpoint: %ls\n",
			ex.message().c_str());
		return false;
	}
	catch (...)
	{
		printf("[Midi2] Unknown exception in EnableMidi2Endpoint.\n");
		return false;
	}
}

// ---------------------------------------------------------------------------
// ForwardToMidi2Endpoint
// ---------------------------------------------------------------------------
// Converts a WinMM packed DWORD (status | data1<<8 | data2<<16) to a 32-bit
// MIDI 1.0 UMP word (message type 0x2) and sends it on the virtual endpoint.
// ---------------------------------------------------------------------------
void ForwardToMidi2Endpoint(DWORD wmidiMsg)
{
	if (!g_enabled.load())
		return;

	try
	{
		uint8_t status = static_cast<uint8_t>(wmidiMsg & 0xFF);
		uint8_t data1  = static_cast<uint8_t>((wmidiMsg >> 8)  & 0xFF);
		uint8_t data2  = static_cast<uint8_t>((wmidiMsg >> 16) & 0xFF);

		// Build a 32-bit UMP MIDI 1.0 Channel Voice message
		//   bits [31:28] = message type 0x2  (MIDI 1.0 Channel Voice)
		//   bits [27:24] = group 0
		//   bits [23:16] = status byte
		//   bits [15:8]  = data1
		//   bits [7:0]   = data2
		uint32_t ump = (0x2u << 28)
					 | (0x0u << 24)
					 | (static_cast<uint32_t>(status) << 16)
					 | (static_cast<uint32_t>(data1)  <<  8)
					 |  static_cast<uint32_t>(data2);

		// timestamp 0 = send immediately
		g_conn.SendSingleMessageWords(static_cast<uint64_t>(0), ump);
	}
	catch (...)
	{
		// Swallow silently — must not disrupt the MIDI callback chain
	}
}

// ---------------------------------------------------------------------------
// DisableMidi2Endpoint
// ---------------------------------------------------------------------------
void DisableMidi2Endpoint()
{
	if (!g_enabled.exchange(false))
		return;

	try
	{
		if (g_conn)
		{
			g_session.DisconnectEndpointConnection(g_conn.ConnectionId());
			g_conn = nullptr;
		}
		if (g_session)
		{
			if (g_associationId != winrt::guid{})
			{
				loopback::MidiLoopbackEndpointRemovalConfig removal{ g_associationId };
				loopback::MidiLoopbackEndpointManager::RemoveTransientLoopbackEndpoints(removal);
				g_associationId = winrt::guid{};
			}
			g_session.Close();
			g_session = nullptr;
		}
		printf("[Midi2] Virtual endpoint closed.\n");
	}
	catch (winrt::hresult_error const& ex)
	{
		printf("[Midi2] WinRT exception in DisableMidi2Endpoint: %ls\n",
			ex.message().c_str());
	}
	catch (...)
	{
		printf("[Midi2] Unknown exception in DisableMidi2Endpoint.\n");
	}
}

// ---------------------------------------------------------------------------
// IsMidi2EndpointEnabled
// ---------------------------------------------------------------------------
bool IsMidi2EndpointEnabled()
{
	return g_enabled.load();
}
