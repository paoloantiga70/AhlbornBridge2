// ---------------------------------------------------------------------------
// Midi2Endpoint.cpp
// ---------------------------------------------------------------------------
// Implements Windows MIDI Services (MIDI 2.0) virtual loopback endpoints
// using the system-installed Windows MIDI Services tool (midi.exe) and WinMM.
//
// This eliminates dependency on MidiDesktopAppSdkInitializer and C++/WinRT
// App SDK projection binaries, which fail with 0x80040154 (Class not
// registered) when the runtime CLSID is not registered in the system registry.
//
// Virtual ports created:
//   Pair A: "AhlbornBridge Virtual Port" (A) / "AhlbornBridge Virtual Port (B)"
//           Bridge writes to (A); Hauptwerk reads from (B).
//   Pair B: "Hauptwerk Virtual (A)" / "Hauptwerk Virtual (B)"
//           Hauptwerk outputs to (A); Bridge reads from (B).
// ---------------------------------------------------------------------------

#include "Midi2Endpoint.h"
#include <windows.h>
#include <mmsystem.h>
#include <winsvc.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

static std::atomic<DWORD> g_lastForwardedTick{ 0 };

// Known constant name for the Hauptwerk monitoring port.
static constexpr wchar_t kHwVirtualBNameConst[] = L"Hauptwerk Virtual (B)";

namespace
{
	std::atomic<bool> g_enabled{ false };

	// Pair A: AhlbornBridge Virtual Port (Bridge -> Hauptwerk)
	std::wstring g_pairAAssocId;
	HMIDIOUT     g_hMidiOutBridge{ nullptr };

	// Pair B: Hauptwerk Virtual (Hauptwerk -> Bridge monitoring)
	std::wstring g_pairBAssocId;
	std::wstring g_hwVirtualBName;

	constexpr wchar_t kEndpointNameA[] = L"AhlbornBridge Virtual Port";
	constexpr wchar_t kEndpointNameB[] = L"AhlbornBridge Virtual Port (B)";

	constexpr wchar_t kHwVirtualAName[] = L"Hauptwerk Virtual (A)";
	constexpr wchar_t kHwVirtualBName[] = L"Hauptwerk Virtual (B)";

	std::wstring FindMidiCli()
	{
		const wchar_t* defaultPath = L"C:\\Program Files\\Windows MIDI Services\\Tools\\midi.exe";
		DWORD attrib = GetFileAttributesW(defaultPath);
		if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY))
		{
			return defaultPath;
		}
		return L"midi.exe";
	}

	std::wstring ExecuteCommand(const std::wstring& commandLine)
	{
		std::wstring result;
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE hReadPipe = NULL, hWritePipe = NULL;
		if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return L"";
		SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.hStdOutput = hWritePipe;
		si.hStdError = hWritePipe;
		si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;

		PROCESS_INFORMATION pi{};
		std::vector<wchar_t> cmdBuffer(commandLine.begin(), commandLine.end());
		cmdBuffer.push_back(L'\0');

		if (CreateProcessW(NULL, cmdBuffer.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		{
			CloseHandle(hWritePipe);
			hWritePipe = NULL;

			char buffer[1024];
			DWORD bytesRead = 0;
			std::string rawOutput;
			while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
			{
				buffer[bytesRead] = '\0';
				rawOutput.append(buffer, bytesRead);
			}

			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);

			int wlen = MultiByteToWideChar(CP_UTF8, 0, rawOutput.c_str(), -1, NULL, 0);
			if (wlen > 0)
			{
				std::vector<wchar_t> wbuf(wlen);
				MultiByteToWideChar(CP_UTF8, 0, rawOutput.c_str(), -1, wbuf.data(), wlen);
				result = wbuf.data();
			}
		}
		else
		{
			if (hWritePipe) CloseHandle(hWritePipe);
		}

		CloseHandle(hReadPipe);
		return result;
	}

	std::wstring ParseAssociationId(const std::wstring& output)
	{
		auto assocPos = output.find(L"Association Id");
		if (assocPos == std::wstring::npos)
			return L"";

		auto guidStart = output.find(L"{", assocPos);
		auto guidEnd   = output.find(L"}", guidStart);
		if (guidStart != std::wstring::npos && guidEnd != std::wstring::npos)
		{
			return output.substr(guidStart, guidEnd - guidStart + 1);
		}
		return L"";
	}

	bool CreateLoopbackPair(const std::wstring& nameA, const std::wstring& nameB, std::wstring& outAssocId)
	{
		std::wstring midiExe = FindMidiCli();
		if (midiExe.empty())
			return false;

		std::wstring cmd = L"\"" + midiExe + L"\" loopback create --name-a \"" + nameA + L"\" --name-b \"" + nameB + L"\"";
		std::wstring output = ExecuteCommand(cmd);

		outAssocId = ParseAssociationId(output);
		return !outAssocId.empty();
	}

	void RemoveLoopbackPair(const std::wstring& assocId)
	{
		if (assocId.empty())
			return;

		std::wstring midiExe = FindMidiCli();
		if (midiExe.empty())
			return;

		std::wstring cmd = L"\"" + midiExe + L"\" loopback remove --association-id \"" + assocId + L"\"";
		ExecuteCommand(cmd);
	}

	bool OpenPairAOutDevice()
	{
		if (g_hMidiOutBridge != nullptr)
			return true;

		// Scan WinMM output devices for "AhlbornBridge Virtual Port"
		for (int retry = 0; retry < 10; ++retry)
		{
			UINT numDevs = midiOutGetNumDevs();
			for (UINT i = 0; i < numDevs; ++i)
			{
				MIDIOUTCAPSW caps{};
				if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
				{
					if (wcscmp(caps.szPname, kEndpointNameA) == 0)
					{
						HMIDIOUT h = nullptr;
						if (midiOutOpen(&h, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
						{
							g_hMidiOutBridge = h;
							printf("[Midi2] Opened '%ls' as WinMM output index %u.\n", kEndpointNameA, i);
							return true;
						}
					}
				}
			}
			Sleep(100);
		}

		printf("[Midi2] Could not find or open WinMM output '%ls'.\n", kEndpointNameA);
		return false;
	}

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
		return false;
	}

	// 1. Create Pair A: "AhlbornBridge Virtual Port" / "(B)"
	if (!CreateLoopbackPair(kEndpointNameA, kEndpointNameB, g_pairAAssocId))
	{
		printf("[Midi2] Failed to create AhlbornBridge loopback endpoint pair.\n");
		return false;
	}
	printf("[Midi2] AhlbornBridge Virtual Port created: assoc ID %ls\n", g_pairAAssocId.c_str());

	// 2. Open Pair A side A via WinMM for real-time dispatch
	if (!OpenPairAOutDevice())
	{
		printf("[Midi2] Failed to connect to AhlbornBridge loopback endpoint.\n");
		RemoveLoopbackPair(g_pairAAssocId);
		g_pairAAssocId.clear();
		return false;
	}

	// 3. Create Pair B: "Hauptwerk Virtual (A)" / "(B)"
	if (!CreateLoopbackPair(kHwVirtualAName, kHwVirtualBName, g_pairBAssocId))
	{
		printf("[Midi2] Failed to create Hauptwerk Virtual loopback endpoint pair.\n");
		// Pair A is still valid; continue with just pair A
	}
	else
	{
		g_hwVirtualBName = kHwVirtualBName;
		printf("[Midi2] Hauptwerk Virtual endpoints created (HW outputs to '%ls', bridge reads '%ls').\n",
			kHwVirtualAName, kHwVirtualBNameConst);
	}

	g_enabled = true;
	return true;
}

// ---------------------------------------------------------------------------
// ForwardToMidi2Endpoint
// ---------------------------------------------------------------------------
void ForwardToMidi2Endpoint(DWORD wmidiMsg)
{
	if (!g_enabled.load())
		return;

	HMIDIOUT h = g_hMidiOutBridge;
	if (h != nullptr)
	{
		midiOutShortMsg(h, wmidiMsg);
		g_lastForwardedTick.store(GetTickCount(), std::memory_order_relaxed);
	}
}

DWORD GetMidi2EndpointLastMsgTime()
{
	return g_lastForwardedTick.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// DisableMidi2Endpoint
// ---------------------------------------------------------------------------
void DisableMidi2Endpoint()
{
	if (!g_enabled.exchange(false))
		return;

	if (g_hMidiOutBridge != nullptr)
	{
		midiOutClose(g_hMidiOutBridge);
		g_hMidiOutBridge = nullptr;
	}

	if (!g_pairAAssocId.empty())
	{
		RemoveLoopbackPair(g_pairAAssocId);
		g_pairAAssocId.clear();
	}

	if (!g_pairBAssocId.empty())
	{
		RemoveLoopbackPair(g_pairBAssocId);
		g_pairBAssocId.clear();
		g_hwVirtualBName.clear();
	}

	printf("[Midi2] Virtual endpoints closed.\n");
}

// ---------------------------------------------------------------------------
// StopMidi2Service
// ---------------------------------------------------------------------------
bool StopMidi2Service()
{
	DisableMidi2Endpoint();

	SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!hScm)
	{
		printf("[Midi2] Cannot open SCM to stop MIDI service (error %lu).\n", GetLastError());
		return false;
	}

	bool stopped = false;
	for (const wchar_t* svcName : { L"MidiSrv2", L"WindowsMidiService", L"midisrv" })
	{
		SC_HANDLE hSvc = OpenServiceW(hScm, svcName, SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (hSvc)
		{
			SERVICE_STATUS status{};
			if (ControlService(hSvc, SERVICE_CONTROL_STOP, &status))
			{
				printf("[Midi2] Sent stop control to service '%ls'.\n", svcName);
				stopped = true;
			}
			else
			{
				DWORD err = GetLastError();
				if (err == ERROR_SERVICE_NOT_ACTIVE)
				{
					printf("[Midi2] Service '%ls' already stopped.\n", svcName);
					stopped = true;
				}
				else
				{
					printf("[Midi2] ControlService failed for '%ls' (error %lu).\n", svcName, err);
				}
			}
			CloseServiceHandle(hSvc);
			break;
		}
	}
	CloseServiceHandle(hScm);
	return stopped;
}

// ---------------------------------------------------------------------------
// IsMidi2EndpointEnabled
// ---------------------------------------------------------------------------
bool IsMidi2EndpointEnabled()
{
	return g_enabled.load();
}

// ---------------------------------------------------------------------------
// GetHauptwerkVirtualBDeviceName
// ---------------------------------------------------------------------------
// Returns the WinMM name for the Hauptwerk Virtual (B) monitoring input.
// If the endpoints were already visible in WinMM at startup (g_hwVirtualBName
// is empty because EnableMidi2Endpoint skipped creation), fall back to the
// known constant name so OpenHauptwerkVirtualBInput can still find the device.
// ---------------------------------------------------------------------------
std::wstring GetHauptwerkVirtualBDeviceName()
{
	if (!g_hwVirtualBName.empty())
		return g_hwVirtualBName;

	// The device may have been created in a previous session and is still
	// registered in WinMM.  Check directly rather than requiring a fresh create.
	UINT n = midiInGetNumDevs();
	for (UINT i = 0; i < n; ++i)
	{
		MIDIINCAPS caps{};
		if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
			std::wstring(caps.szPname) == kHwVirtualBNameConst)
		{
			return kHwVirtualBNameConst;
		}
	}
	return {};
}

