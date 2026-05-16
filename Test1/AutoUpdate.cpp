#include "AutoUpdate.h"
#include "Version.h"
#include "../StreamDeckPlugin/PluginVersion.h"

#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <shlobj.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Simple version comparison: "major.minor.patch" → integer tuple compare.
// Returns  1 if a > b, -1 if a < b, 0 if equal.
static int CompareVersions(const wchar_t* a, const wchar_t* b)
{
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;
    swscanf_s(a, L"%d.%d.%d", &a1, &a2, &a3);
    swscanf_s(b, L"%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 > b1 ? 1 : -1;
    if (a2 != b2) return a2 > b2 ? 1 : -1;
    if (a3 != b3) return a3 > b3 ? 1 : -1;
    return 0;
}

// Perform an HTTPS GET and return the response body as a narrow string.
static bool HttpsGet(const wchar_t* host, const wchar_t* path,
                     std::string& responseBody)
{
    HINTERNET hSession = WinHttpOpen(
        L"AhlbornBridge-AutoUpdate/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host,
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path,
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Limit wait time so a missing/slow network does not block the caller.
    DWORD timeout = 5000; // 5 seconds
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    responseBody.clear();
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead))
            responseBody.append(buf.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

// Download a file from an HTTPS URL to a local path. Handles HTTP 302
// redirects returned by GitHub when downloading release assets.
static bool HttpsDownloadFile(const wchar_t* url, const wchar_t* localPath)
{
    // Parse the URL.
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = _countof(hostBuf);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = _countof(pathBuf);
    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return false;

    HINTERNET hSession = WinHttpOpen(
        L"AhlbornBridge-AutoUpdate/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf,
                                        uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", pathBuf,
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Follow redirects (GitHub sends 302 to S3).
    DWORD optRedirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                     &optRedirect, sizeof(optRedirect));

    // Accept any content type for the binary download.
    const wchar_t* acceptTypes = L"application/octet-stream";
    WinHttpAddRequestHeaders(hRequest, (std::wstring(L"Accept: ") + acceptTypes).c_str(),
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    HANDLE hFile = CreateFileW(localPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD bytesAvailable = 0;
    bool ok = true;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead))
        {
            DWORD written = 0;
            if (!WriteFile(hFile, buf.data(), bytesRead, &written, nullptr))
            {
                ok = false;
                break;
            }
        }
    }

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok) DeleteFileW(localPath);
    return ok;
}

// Minimal JSON string value extractor.
// Finds the first occurrence of "key": "value" and returns value.
static bool JsonExtractString(const std::string& json, const char* key,
                              std::wstring& value)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;

    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    pos++; // skip opening quote

    size_t end = json.find('"', pos);
    if (end == std::string::npos) return false;

    std::string val(json, pos, end - pos);
    value.assign(val.begin(), val.end());
    return true;
}

// Find the browser_download_url for the first .exe asset.
static bool JsonExtractInstallerUrl(const std::string& json,
                                    std::wstring& url)
{
    // Look inside "assets":[ ... ] for "browser_download_url" ending with .exe
    const char* searchStart = "\"assets\"";
    size_t assetsPos = json.find(searchStart);
    if (assetsPos == std::string::npos) return false;

    // Scan for all browser_download_url entries inside assets array
    size_t cursor = assetsPos;
    while (true)
    {
        const char* key = "\"browser_download_url\"";
        size_t pos = json.find(key, cursor);
        if (pos == std::string::npos) break;

        pos = json.find(':', pos + strlen(key));
        if (pos == std::string::npos) break;

        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) break;
        pos++;

        size_t end = json.find('"', pos);
        if (end == std::string::npos) break;

        std::string val(json, pos, end - pos);
        // Check if URL ends with .exe (Inno Setup installer)
        if (val.size() > 4 && val.substr(val.size() - 4) == ".exe")
        {
            url.assign(val.begin(), val.end());
            return true;
        }
        cursor = end + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool CheckForUpdateGeneric(const wchar_t* repoOwner, const wchar_t* repoName,
                           const wchar_t* currentVersion,
                           std::wstring& newVersion, std::wstring& downloadUrl)
{
    std::wstring apiPath = L"/repos/";
    apiPath += repoOwner;
    apiPath += L"/";
    apiPath += repoName;
    apiPath += L"/releases/latest";

    std::string body;
    if (!HttpsGet(L"api.github.com", apiPath.c_str(), body))
    {
        printf("AutoUpdate: failed to fetch latest release from GitHub.\n");
        return false;
    }

    std::wstring tagName;
    if (!JsonExtractString(body, "tag_name", tagName))
    {
        printf("AutoUpdate: tag_name not found in response.\n");
        return false;
    }

    if (!tagName.empty() && (tagName[0] == L'v' || tagName[0] == L'V'))
        tagName = tagName.substr(1);

    std::wstring current(currentVersion);

    if (CompareVersions(tagName.c_str(), current.c_str()) <= 0)
    {
        printf("AutoUpdate: current version %ls is up to date (remote %ls).\n",
               current.c_str(), tagName.c_str());
        return false;
    }

    if (!JsonExtractInstallerUrl(body, downloadUrl))
    {
        printf("AutoUpdate: no .exe asset found in release.\n");
        return false;
    }

    newVersion = tagName;
    printf("AutoUpdate: new version %ls available. URL: %ls\n",
           newVersion.c_str(), downloadUrl.c_str());
    return true;
}

bool CheckForUpdate(std::wstring& newVersion, std::wstring& downloadUrl)
{
    const char* av = APP_VERSION;
    std::wstring current(av, av + strlen(av));
    return CheckForUpdateGeneric(GITHUB_REPO_OWNER, GITHUB_REPO_NAME,
                                 current.c_str(), newVersion, downloadUrl);
}

bool DownloadAndInstallUpdate(const std::wstring& downloadUrl,
                              const wchar_t* installerFilename)
{
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);

    std::wstring tempFile = std::wstring(tempDir) + installerFilename;

    printf("AutoUpdate: downloading %ls -> %ls\n",
           downloadUrl.c_str(), tempFile.c_str());

    if (!HttpsDownloadFile(downloadUrl.c_str(), tempFile.c_str()))
    {
        printf("AutoUpdate: download failed.\n");
        return false;
    }

    printf("AutoUpdate: launching installer...\n");

    // Launch the installer with /SILENT so it upgrades in place.
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = tempFile.c_str();
    sei.lpParameters = nullptr;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
    {
        printf("AutoUpdate: failed to launch installer (error %lu).\n",
               GetLastError());
        return false;
    }

    return true;
}

void CheckForUpdateInteractive(HWND hParent, bool silent)
{
    std::wstring newVersion;
    std::wstring downloadUrl;

    if (!CheckForUpdate(newVersion, downloadUrl))
    {
        if (!silent)
        {
            MessageBoxW(hParent,
                         L"You are running the latest version of AhlbornBridge.",
                         L"AhlbornBridge Update",
                         MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    std::wstring msg = L"A new version of AhlbornBridge is available: v" + newVersion +
                       L"\nCurrent version: " + 
                       []{ const char* v = APP_VERSION; return std::wstring(v, v + strlen(v)); }() +
                       L"\n\nDo you want to download and install it now?";

    int result = MessageBoxW(hParent, msg.c_str(),
                              L"AhlbornBridge Update",
                              MB_YESNO | MB_ICONQUESTION);
    if (result != IDYES)
        return;

    if (DownloadAndInstallUpdate(downloadUrl))
    {
        // Signal the app to exit so the installer can replace the executable.
        PostMessage(hParent, WM_CLOSE, 0, 0);
    }
    else
    {
        MessageBoxW(hParent,
                     L"Failed to download the update.\nPlease try again later or download manually from GitHub.",
                     L"AhlbornBridge Update",
                     MB_OK | MB_ICONERROR);
    }
}

// ---------------------------------------------------------------------------
// Plugin Update
// ---------------------------------------------------------------------------

// Read the installed plugin version from manifest.json in the SD plugins folder.
static bool GetInstalledPluginVersion(std::wstring& version)
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return false;

    std::wstring manifestPath = std::wstring(appData) +
        L"\\Elgato\\StreamDeck\\Plugins\\com.ahlbornbridge.organ.sdPlugin\\manifest.json";

    HANDLE hFile = CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        return false;
    }

    std::string json(fileSize, '\0');
    DWORD bytesRead = 0;
    ReadFile(hFile, json.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);

    return JsonExtractString(json, "Version", version);
}

void CheckForPluginUpdateInteractive(HWND hParent, bool silent)
{
    std::wstring currentVersion;
    if (!GetInstalledPluginVersion(currentVersion))
    {
        if (!silent)
        {
            MessageBoxW(hParent,
                         L"Stream Deck plugin is not installed.\n"
                         L"Install it first, then check for updates.",
                         L"Plugin Update",
                         MB_OK | MB_ICONWARNING);
        }
        return;
    }

    std::wstring newVersion;
    std::wstring downloadUrl;

    if (!CheckForUpdateGeneric(PLUGIN_GITHUB_REPO_OWNER, PLUGIN_GITHUB_REPO_NAME,
                               currentVersion.c_str(), newVersion, downloadUrl))
    {
        if (!silent)
        {
            std::wstring msg = L"Stream Deck plugin v" + currentVersion +
                               L" is up to date.";
            MessageBoxW(hParent, msg.c_str(),
                         L"Plugin Update",
                         MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    std::wstring msg = L"A new version of the Stream Deck plugin is available: v" + newVersion +
                       L"\nInstalled version: v" + currentVersion +
                       L"\n\nDo you want to download and install it now?";

    int result = MessageBoxW(hParent, msg.c_str(),
                              L"Plugin Update",
                              MB_YESNO | MB_ICONQUESTION);
    if (result != IDYES)
        return;

    if (DownloadAndInstallUpdate(downloadUrl, L"AhlbornBridgeSD_Setup.exe"))
    {
        MessageBoxW(hParent,
                     L"Plugin installer started.\n"
                     L"Stream Deck will restart automatically.",
                     L"Plugin Update",
                     MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        MessageBoxW(hParent,
                     L"Failed to download the plugin update.\n"
                     L"Please try again later or download manually from GitHub.",
                     L"Plugin Update",
                     MB_OK | MB_ICONERROR);
    }
}
