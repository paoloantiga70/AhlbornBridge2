#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr wchar_t kServiceName[] = L"AhlbornBridgeProcessManager";
	constexpr wchar_t kServiceDisplayName[] = L"AhlbornBridge Process Manager";
	constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\AhlbornBridgeProcessManager";

	SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
	SERVICE_STATUS g_serviceStatus{};
	HANDLE g_stopEvent = nullptr;

	std::wstring Trim(const std::wstring& s)
	{
		size_t start = 0;
		while (start < s.size() && (iswspace(s[start]) || s[start] == 0xFEFF)) ++start;
		size_t end = s.size();
		while (end > start && (iswspace(s[end - 1]) || s[end - 1] == L'\0')) --end;
		return s.substr(start, end - start);
	}

	std::wstring ToUpper(std::wstring s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
			return static_cast<wchar_t>(towupper(ch));
			});
		return s;
	}

	DWORD PriorityClassFromName(std::wstring name)
	{
		name = ToUpper(Trim(name));
		if (name == L"IDLE") return IDLE_PRIORITY_CLASS;
		if (name == L"BELOW_NORMAL") return BELOW_NORMAL_PRIORITY_CLASS;
		if (name == L"NORMAL") return NORMAL_PRIORITY_CLASS;
		if (name == L"ABOVE_NORMAL") return ABOVE_NORMAL_PRIORITY_CLASS;
		if (name == L"HIGH") return HIGH_PRIORITY_CLASS;
		if (name == L"REALTIME") return REALTIME_PRIORITY_CLASS;
		return 0;
	}

	std::wstring PriorityClassToName(DWORD priorityClass)
	{
		switch (priorityClass)
		{
		case IDLE_PRIORITY_CLASS: return L"IDLE";
		case BELOW_NORMAL_PRIORITY_CLASS: return L"BELOW_NORMAL";
		case NORMAL_PRIORITY_CLASS: return L"NORMAL";
		case ABOVE_NORMAL_PRIORITY_CLASS: return L"ABOVE_NORMAL";
		case HIGH_PRIORITY_CLASS: return L"HIGH";
		case REALTIME_PRIORITY_CLASS: return L"REALTIME";
		default: return L"UNKNOWN";
		}
	}

	std::vector<DWORD> FindProcessIdsByName(const std::wstring& exeName)
	{
		std::vector<DWORD> pids;
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return pids;

		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		if (Process32FirstW(snapshot, &pe))
		{
			do
			{
				if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0)
					pids.push_back(pe.th32ProcessID);
			} while (Process32NextW(snapshot, &pe));
		}

		CloseHandle(snapshot);
		return pids;
	}

	std::wstring HandleSetPriority(const std::wstring& processName, const std::wstring& priorityName)
	{
		if (processName.empty())
			return L"ERROR Missing process name";

		DWORD desiredPriority = PriorityClassFromName(priorityName);
		if (desiredPriority == 0)
			return L"ERROR Invalid priority";

		std::vector<DWORD> pids = FindProcessIdsByName(processName);
		if (pids.empty())
			return L"ERROR Process not found";

		int updated = 0;
		int failed = 0;

		for (DWORD pid : pids)
		{
			HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
			if (!hProcess)
			{
				++failed;
				continue;
			}

			if (SetPriorityClass(hProcess, desiredPriority))
				++updated;
			else
				++failed;

			CloseHandle(hProcess);
		}

		std::wstringstream ss;
		ss << L"OK updated=" << updated << L" failed=" << failed;
		return ss.str();
	}

	std::wstring HandleGetPriority(const std::wstring& processName)
	{
		if (processName.empty())
			return L"ERROR Missing process name";

		std::vector<DWORD> pids = FindProcessIdsByName(processName);
		if (pids.empty())
			return L"ERROR Process not found";

		HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pids.front());
		if (!hProcess)
			return L"ERROR OpenProcess failed";

		DWORD pc = GetPriorityClass(hProcess);
		CloseHandle(hProcess);
		if (pc == 0)
			return L"ERROR GetPriorityClass failed";

		std::wstringstream ss;
		ss << L"OK priority=" << PriorityClassToName(pc) << L" pid=" << pids.front();
		return ss.str();
	}

	std::wstring HandleCommand(const std::wstring& command)
	{
		std::wstring cmd = Trim(command);
		if (cmd.empty())
			return L"ERROR Empty command";

		std::wstringstream stream(cmd);
		std::wstring verb;
		stream >> verb;
		verb = ToUpper(verb);

		if (verb == L"PING")
			return L"OK PONG";

		if (verb == L"SET_PRIORITY")
		{
			std::wstring processName;
			std::wstring priorityName;
			stream >> processName >> priorityName;
			return HandleSetPriority(processName, priorityName);
		}

		if (verb == L"GET_PRIORITY")
		{
			std::wstring processName;
			stream >> processName;
			return HandleGetPriority(processName);
		}

		return L"ERROR Unknown command";
	}

	void SetServiceStatusSafe(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
	{
		g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		g_serviceStatus.dwCurrentState = currentState;
		g_serviceStatus.dwWin32ExitCode = win32ExitCode;
		g_serviceStatus.dwWaitHint = waitHint;
		g_serviceStatus.dwControlsAccepted =
			(currentState == SERVICE_START_PENDING)
			? 0
			: SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
		SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
	}

	DWORD WINAPI PipeServerThread(LPVOID)
	{
		while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
		{
			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = FALSE;

			SECURITY_DESCRIPTOR sd{};
			bool securityReady = false;
			if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
				SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE))
			{
				sa.lpSecurityDescriptor = &sd;
				securityReady = true;
			}
			else
			{
				sa.lpSecurityDescriptor = nullptr;
				printf("[ProcessManagerService] Warning: failed to initialize pipe security descriptor (err=%lu).\n", GetLastError());
			}

			HANDLE hPipe = CreateNamedPipeW(
				kPipeName,
				PIPE_ACCESS_DUPLEX,
				PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
				PIPE_UNLIMITED_INSTANCES,
				4096,
				4096,
				0,
				securityReady ? &sa : nullptr);

			if (hPipe == INVALID_HANDLE_VALUE)
			{
				Sleep(500);
				continue;
			}

			BOOL connected = ConnectNamedPipe(hPipe, nullptr)
				? TRUE
				: (GetLastError() == ERROR_PIPE_CONNECTED);

			if (!connected)
			{
				CloseHandle(hPipe);
				continue;
			}

			wchar_t buffer[1024] = {};
			DWORD bytesRead = 0;
			if (ReadFile(hPipe, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, nullptr) && bytesRead > 0)
			{
				buffer[bytesRead / sizeof(wchar_t)] = L'\0';
				std::wstring response = HandleCommand(buffer);

				DWORD bytesWritten = 0;
				WriteFile(hPipe, response.c_str(), static_cast<DWORD>((response.size() + 1) * sizeof(wchar_t)), &bytesWritten, nullptr);
			}

			FlushFileBuffers(hPipe);
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}

		return 0;
	}

	VOID WINAPI ServiceControlHandler(DWORD control)
	{
		switch (control)
		{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			SetServiceStatusSafe(SERVICE_STOP_PENDING, NO_ERROR, 2000);
			if (g_stopEvent)
				SetEvent(g_stopEvent);
			break;
		default:
			break;
		}
	}

	VOID WINAPI ServiceMain(DWORD, LPWSTR*)
	{
		g_serviceStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControlHandler);
		if (!g_serviceStatusHandle)
			return;

		SetServiceStatusSafe(SERVICE_START_PENDING, NO_ERROR, 3000);

		g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_stopEvent)
		{
			SetServiceStatusSafe(SERVICE_STOPPED, GetLastError());
			return;
		}

		HANDLE serverThread = CreateThread(nullptr, 0, PipeServerThread, nullptr, 0, nullptr);
		if (!serverThread)
		{
			CloseHandle(g_stopEvent);
			g_stopEvent = nullptr;
			SetServiceStatusSafe(SERVICE_STOPPED, GetLastError());
			return;
		}

		SetServiceStatusSafe(SERVICE_RUNNING);

		WaitForSingleObject(g_stopEvent, INFINITE);
		WaitForSingleObject(serverThread, 2000);

		CloseHandle(serverThread);
		CloseHandle(g_stopEvent);
		g_stopEvent = nullptr;

		SetServiceStatusSafe(SERVICE_STOPPED);
	}

	bool InstallService()
	{
		wchar_t exePath[MAX_PATH] = {};
		if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
			return false;

		SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
		if (!scm)
			return false;

		SC_HANDLE service = CreateServiceW(
			scm,
			kServiceName,
			kServiceDisplayName,
			SERVICE_ALL_ACCESS,
			SERVICE_WIN32_OWN_PROCESS,
			SERVICE_AUTO_START,
			SERVICE_ERROR_NORMAL,
			exePath,
			nullptr,
			nullptr,
			nullptr,
			L"LocalSystem",
			nullptr);

		if (!service)
		{
			CloseServiceHandle(scm);
			return false;
		}

		CloseServiceHandle(service);
		CloseServiceHandle(scm);
		return true;
	}

	bool UninstallService()
	{
		SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (!scm)
			return false;

		SC_HANDLE service = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (!service)
		{
			CloseServiceHandle(scm);
			return false;
		}

		SERVICE_STATUS status{};
		ControlService(service, SERVICE_CONTROL_STOP, &status);

		bool deleted = DeleteService(service) != FALSE;

		CloseServiceHandle(service);
		CloseServiceHandle(scm);
		return deleted;
	}

	int RunConsoleDebugServer()
	{
		g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_stopEvent)
			return 1;

		wprintf(L"[ProcessManagerService] Debug mode started on %s\n", kPipeName);
		HANDLE serverThread = CreateThread(nullptr, 0, PipeServerThread, nullptr, 0, nullptr);
		if (!serverThread)
		{
			CloseHandle(g_stopEvent);
			g_stopEvent = nullptr;
			return 1;
		}

		wprintf(L"Press ENTER to stop...\n");
		(void)getwchar();

		SetEvent(g_stopEvent);
		WaitForSingleObject(serverThread, 2000);

		CloseHandle(serverThread);
		CloseHandle(g_stopEvent);
		g_stopEvent = nullptr;
		return 0;
	}
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc > 1)
	{
		std::wstring arg = ToUpper(argv[1]);
		if (arg == L"--INSTALL")
		{
			wprintf(InstallService() ? L"Service installed.\n" : L"Service install failed.\n");
			return 0;
		}
		if (arg == L"--UNINSTALL")
		{
			wprintf(UninstallService() ? L"Service uninstalled.\n" : L"Service uninstall failed.\n");
			return 0;
		}
		if (arg == L"--CONSOLE")
		{
			return RunConsoleDebugServer();
		}
	}

	SERVICE_TABLE_ENTRYW table[] =
	{
		{ const_cast<LPWSTR>(kServiceName), ServiceMain },
		{ nullptr, nullptr }
	};

	if (!StartServiceCtrlDispatcherW(table))
	{
		wprintf(L"StartServiceCtrlDispatcher failed: %lu\n", GetLastError());
		return 1;
	}

	return 0;
}
