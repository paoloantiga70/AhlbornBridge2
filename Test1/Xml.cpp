#include "Xml.h"
#include "Midi.h"
#include "TrayIcon.h"
#include "StreamDeck.h"

#include <string>
#include <set>
#include <vector>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace
{
    // Settings are stored per-user in the Roaming AppData folder.
    // Path: %APPDATA%\TuaApp\Settings.xml

    std::wstring GetSettingsDirPath()
    {
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)))
        {
            std::wstring dir(buf);
            dir += L"\\AhlbornBridge";
            return dir;
        }

        // Fallback to C:\Users\Default\AppData\Roaming\TuaApp if retrieval fails
        return std::wstring(L"C:\\Users\\Default\\AppData\\Roaming\\AhlbornBridge");
    }

    std::wstring GetSettingsFilePath()
    {
        std::wstring dir = GetSettingsDirPath();
        return dir + L"\\Settings.xml";
    }

    bool TryGetTagEnabledAttribute(const std::wstring& xml, const std::wstring& tagName, bool& enabled)
    {
        std::wstring search = L"<" + tagName;
        size_t pos = xml.find(search);
        if (pos == std::wstring::npos) { enabled = true; return true; }
        size_t tagEnd = xml.find(L'>', pos);
        if (tagEnd == std::wstring::npos) return false;
        std::wstring tagContent = xml.substr(pos, tagEnd - pos);
        std::wstring attr = L"enabled=\"";
        size_t attrPos = tagContent.find(attr);
        if (attrPos == std::wstring::npos) { enabled = true; return true; }
        attrPos += attr.size();
        size_t attrEnd = tagContent.find(L'"', attrPos);
        if (attrEnd == std::wstring::npos) return false;
        enabled = (tagContent.substr(attrPos, attrEnd - attrPos) != L"0");
        return true;
    }

    struct DeviceEnabledStates
    {
        bool input1  = true;
        bool input2  = true;
        bool output1 = true;
        bool output2 = true;
    };

    // Forward declarations.
    bool WriteSettingsXml(const std::wstring& inputDeviceName, const std::wstring& input2DeviceName, const std::wstring& outputDeviceName, const std::wstring& output2DeviceName, bool routerEnabled, bool closeSettingsOnDisconnect, bool showDebugConsole, bool checkForUpdateOnStart, const DeviceEnabledStates& devEnabled);
    std::wstring ReadHauptwerkStandbyOrgans();
    std::wstring ReadHauptwerkInstalledOrgans();

    // Cached standby-organ XML fragment (read once from the Hauptwerk config).
    std::wstring s_cachedStandbyOrgans;
    bool s_standbyOrgansLoaded = false;

    // Cached installed-organs XML fragment (scanned once from OrganDefinitions).
    std::wstring s_cachedInstalledOrgans;
    bool s_installedOrgansLoaded = false;

    // Cached Stream Deck settings (persisted to <StreamDeck> in Settings.xml).
    std::wstring s_cachedStreamDeckCC = L"81";
    std::wstring s_cachedStreamDeckMidiOut;
    std::wstring s_cachedStreamDeckMidiIn;
    bool s_streamDeckSettingsLoaded = false;

    // Cached Hauptwerk folder paths (detected once at startup).
    std::wstring s_rootHauptwerkApp;
    std::wstring s_rootHauptwerkUserData;
    std::wstring s_rootHauptwerkSampleSets;
    std::wstring s_rootHauptwerkWorkingFiles;
    bool s_hauptwerkPathsLoaded = false;

    void EnsureStandbyOrgansLoaded()
    {
        if (!s_standbyOrgansLoaded)
        {
            s_standbyOrgansLoaded = true;
            s_cachedStandbyOrgans = ReadHauptwerkStandbyOrgans();
        }
    }

    void EnsureInstalledOrgansLoaded()
    {
        if (!s_installedOrgansLoaded)
        {
            s_installedOrgansLoaded = true;
            s_cachedInstalledOrgans = ReadHauptwerkInstalledOrgans();
        }
    }

    bool TryReadSettingsXml(std::wstring& xml)
    {
        std::wstring settingsFile = GetSettingsFilePath();
        HANDLE fileHandle = CreateFileW(settingsFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            // If the settings file is missing, create a default one and try again.
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            {
                // Ensure directory exists and write defaults (device 0, output 0, router enabled, auto-close enabled, debug console hidden)
                WriteSettingsXml(L"", L"", L"", L"", true, true, false, true, DeviceEnabledStates{});
                fileHandle = CreateFileW(settingsFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (fileHandle == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }

        DWORD fileSize = GetFileSize(fileHandle, nullptr);
        if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
        {
            CloseHandle(fileHandle);
            return false;
        }

        std::string utf8(fileSize, '\0');
        DWORD bytesRead = 0;
        BOOL readOk = ReadFile(fileHandle, utf8.data(), fileSize, &bytesRead, nullptr);
        CloseHandle(fileHandle);

        if (readOk == FALSE || bytesRead != fileSize)
        {
            return false;
        }

        int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wideSize <= 0)
        {
            return false;
        }

        xml.assign(wideSize, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), xml.data(), wideSize);

        return true;
    }

    // Extract the inner text of a named section, e.g. sectionName = L"Midi" will
    // return the substring between <Midi> and </Midi> (not including the tags).
    bool TryGetSection(const std::wstring& xml, const std::wstring& sectionName, std::wstring& sectionContent)
    {
        std::wstring startTag = L"<" + sectionName + L">";
        std::wstring endTag = L"</" + sectionName + L">";
        size_t start = xml.find(startTag);
        if (start == std::wstring::npos)
            return false;

        start += startTag.size();
        size_t end = xml.find(endTag, start);
        if (end == std::wstring::npos || end <= start)
            return false;

        sectionContent = xml.substr(start, end - start);
        return true;
    }

    bool TryGetTagValue(const std::wstring& xml, const std::wstring& startTag, const std::wstring& endTag, UINT& value)
    {
        size_t start = xml.find(startTag);
        if (start == std::wstring::npos)
        {
            return false;
        }

        start += startTag.size();
        size_t end = xml.find(endTag, start);
        if (end == std::wstring::npos || end <= start)
        {
            return false;
        }

        std::wstring rawValue = xml.substr(start, end - start);
        try
        {
            value = static_cast<UINT>(std::stoul(rawValue));
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

	bool TryGetTagStringValue(const std::wstring& xml, const std::wstring& startTag, const std::wstring& endTag, std::wstring& value)
	{
		// Support tags with attributes: strip trailing '>' from startTag
		// and scan forward to the actual '>' so that
		// <MidiInputDevice01 id="0">value</...> is matched by startTag "<MidiInputDevice01>".
		std::wstring prefix = startTag;
		if (!prefix.empty() && prefix.back() == L'>')
			prefix.pop_back();

		size_t pos = xml.find(prefix);
		if (pos == std::wstring::npos)
			return false;

		size_t start = xml.find(L'>', pos + prefix.size());
		if (start == std::wstring::npos)
			return false;
		++start; // move past '>'

		size_t end = xml.find(endTag, start);
		if (end == std::wstring::npos)
			return false;

		value = xml.substr(start, end - start);
		return true;
	}

	// Extract the numeric value of an id="X" attribute from a tag.
	// tagName is the bare element name, e.g. L"MidiInputDevice01".
	bool TryGetTagIdAttribute(const std::wstring& xml, const std::wstring& tagName, UINT& id)
	{
		std::wstring search = L"<" + tagName;
		size_t pos = xml.find(search);
		if (pos == std::wstring::npos)
			return false;

		size_t tagEnd = xml.find(L'>', pos);
		if (tagEnd == std::wstring::npos)
			return false;

		std::wstring tagContent = xml.substr(pos, tagEnd - pos);
		std::wstring idAttr = L"id=\"";
		size_t idPos = tagContent.find(idAttr);
		if (idPos == std::wstring::npos)
			return false;

		idPos += idAttr.size();
		size_t idEnd = tagContent.find(L'"', idPos);
		if (idEnd == std::wstring::npos)
			return false;

		std::wstring idStr = tagContent.substr(idPos, idEnd - idPos);
		try
		{
			id = static_cast<UINT>(std::stoul(idStr));
		}
		catch (...)
		{
			return false;
		}
		return true;
	}

	std::wstring GetMidiInputDeviceName(UINT index)
	{
		MIDIINCAPS caps = {};
		if (midiInGetDevCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			return std::wstring(caps.szPname);
		return {};
	}

	std::wstring GetMidiOutputDeviceName(UINT index)
	{
		MIDIOUTCAPS caps = {};
		if (midiOutGetDevCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			return std::wstring(caps.szPname);
		return {};
	}

	bool FindMidiInputDeviceIndex(const std::wstring& name, UINT& index)
	{
		if (name.empty())
			return false;
		UINT numDevs = midiInGetNumDevs();
		for (UINT i = 0; i < numDevs; ++i)
		{
			MIDIINCAPS caps = {};
			if (midiInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			{
				if (name == caps.szPname)
				{
					index = i;
					return true;
				}
			}
		}
		return false;
	}

	bool FindMidiOutputDeviceIndex(const std::wstring& name, UINT& index)
	{
		if (name.empty())
			return false;
		UINT numDevs = midiOutGetNumDevs();
		for (UINT i = 0; i < numDevs; ++i)
		{
			MIDIOUTCAPS caps = {};
			if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			{
				if (name == caps.szPname)
				{
					index = i;
					return true;
				}
			}
		}
		return false;
	}

	// Read a UTF-8 file into a wide string.  Returns empty on failure.
	std::wstring ReadFileToWString(const std::wstring& path)
	{
		HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (fh == INVALID_HANDLE_VALUE) return {};
		DWORD fileSize = GetFileSize(fh, nullptr);
		if (fileSize == INVALID_FILE_SIZE || fileSize == 0) { CloseHandle(fh); return {}; }
		std::string raw(fileSize, '\0');
		DWORD bytesRead = 0;
		BOOL ok = ReadFile(fh, raw.data(), fileSize, &bytesRead, nullptr);
		CloseHandle(fh);
		if (!ok || bytesRead != fileSize) return {};
		int wideSize = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
		if (wideSize <= 0) return {};
		std::wstring result(wideSize, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), result.data(), wideSize);
		return result;
	}

	// Navigate into a section whose opening tag may carry attributes.
	bool TrySectionWithAttrs(const std::wstring& xml, const std::wstring& name, std::wstring& out)
	{
		std::wstring prefix = L"<" + name;
		size_t pos = xml.find(prefix);
		if (pos == std::wstring::npos) return false;
		size_t gt = xml.find(L'>', pos + prefix.size());
		if (gt == std::wstring::npos) return false;
		size_t start = gt + 1;
		std::wstring endTag = L"</" + name + L">";
		size_t end = xml.find(endTag, start);
		if (end == std::wstring::npos || end <= start) return false;
		out = xml.substr(start, end - start);
		return true;
	}

	// Read Hauptwerk FileLocations config and extract root folders.
	bool ReadHauptwerkFileLocations(const std::wstring& appPath)
	{
		std::wstring filePath = appPath + L"\\InternalResources\\FileLocations.FileLocations_Hauptwerk_xml";
		std::wstring xml = ReadFileToWString(filePath);
		if (xml.empty())
		{
			printf("ReadHauptwerkFileLocations: Cannot read %S\n", filePath.c_str());
			return false;
		}

		std::wstring hwSection, objList, general;
		if (!TrySectionWithAttrs(xml, L"Hauptwerk", hwSection)) return false;
		if (!TrySectionWithAttrs(hwSection, L"ObjectList", objList)) return false;
		if (!TrySectionWithAttrs(objList, L"_General", general)) return false;

		TryGetTagStringValue(general, L"<RootFolder_HauptwerkUserData>", L"</RootFolder_HauptwerkUserData>", s_rootHauptwerkUserData);
		TryGetTagStringValue(general, L"<RootFolder_HauptwerkSampleSetsAndComponents>", L"</RootFolder_HauptwerkSampleSetsAndComponents>", s_rootHauptwerkSampleSets);
		TryGetTagStringValue(general, L"<RootFolder_HauptwerkInternalWorkingFiles>", L"</RootFolder_HauptwerkInternalWorkingFiles>", s_rootHauptwerkWorkingFiles);

		printf("ReadHauptwerkFileLocations: UserData      = %S\n", s_rootHauptwerkUserData.c_str());
		printf("ReadHauptwerkFileLocations: SampleSets    = %S\n", s_rootHauptwerkSampleSets.c_str());
		printf("ReadHauptwerkFileLocations: WorkingFiles  = %S\n", s_rootHauptwerkWorkingFiles.c_str());
		return true;
	}

	// Show a folder picker dialog.  Returns true if a folder was selected.
	bool BrowseForHauptwerkFolder(std::wstring& selectedPath)
	{
		IFileOpenDialog* pDialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
			IID_IFileOpenDialog, (void**)&pDialog);
		if (FAILED(hr) || !pDialog) return false;

		DWORD options = 0;
		pDialog->GetOptions(&options);
		pDialog->SetOptions(options | FOS_PICKFOLDERS);
		pDialog->SetTitle(L"Seleziona la cartella di installazione di Hauptwerk");

		hr = pDialog->Show(nullptr);
		if (FAILED(hr)) { pDialog->Release(); return false; }

		IShellItem* pItem = nullptr;
		hr = pDialog->GetResult(&pItem);
		if (FAILED(hr) || !pItem) { pDialog->Release(); return false; }

		PWSTR pPath = nullptr;
		hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath);
		if (SUCCEEDED(hr) && pPath)
		{
			selectedPath = pPath;
			CoTaskMemFree(pPath);
		}

		pItem->Release();
		pDialog->Release();
		return !selectedPath.empty();
	}

	// Read the Hauptwerk config file and extract Standby_Organ01..08 values
	// from <Hauptwerk><ObjectList><_General>.  Returns an XML fragment ready
	// to be inserted inside <StandbyeOrgans>.
	std::wstring ReadHauptwerkStandbyOrgans()
	{
		// Build the config path from the saved RootFolder_HauptwerkUserData.
		std::wstring userDataRoot = s_rootHauptwerkUserData;
		if (userDataRoot.empty())
		{
			// Fallback: try to load from Settings.xml in case the cache
			// has not been populated yet.
			std::wstring xml;
			if (TryReadSettingsXml(xml))
			{
				std::wstring opts;
				if (TryGetSection(xml, L"Options", opts))
					TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>", L"</RootFolder_HauptwerkUserData>", userDataRoot);
			}
		}
		if (userDataRoot.empty())
		{
			printf("ReadHauptwerkStandbyOrgans: RootFolder_HauptwerkUserData not configured.\n");
			return {};
		}

		// Remove trailing backslash if present.
		if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
			userDataRoot.pop_back();

		std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";
		printf("ReadHauptwerkStandbyOrgans: configPath = %S\n", configPath.c_str());

		HANDLE fh = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (fh == INVALID_HANDLE_VALUE)
		{
			printf("ReadHauptwerkStandbyOrgans: Cannot open config (error %lu)\n", GetLastError());
			return {};
		}

		DWORD fileSize = GetFileSize(fh, nullptr);
		if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
		{
			CloseHandle(fh);
			return {};
		}

		std::string raw(fileSize, '\0');
		DWORD bytesRead = 0;
		BOOL readOk = ReadFile(fh, raw.data(), fileSize, &bytesRead, nullptr);
		CloseHandle(fh);

		if (readOk == FALSE || bytesRead != fileSize)
			return {};

		int wideSize = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
			static_cast<int>(raw.size()), nullptr, 0);
		if (wideSize <= 0)
			return {};

		std::wstring hwXml(wideSize, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, raw.data(),
			static_cast<int>(raw.size()), hwXml.data(), wideSize);

		// Navigate: <Hauptwerk> -> <ObjectList> -> <_General>
		// Hauptwerk config tags often carry attributes (e.g.
		// <_General ObjectID="1">), so we use a prefix search
		// that tolerates attributes instead of TryGetSection
		// which requires an exact "<tag>" match.
		auto trySectionWithAttrs = [](const std::wstring& xml,
			const std::wstring& name, std::wstring& out) -> bool
		{
			std::wstring prefix = L"<" + name;
			size_t pos = xml.find(prefix);
			if (pos == std::wstring::npos)
			{
				printf("ReadHauptwerkStandbyOrgans: <%S> NOT FOUND\n", name.c_str());
				return false;
			}
			size_t gt = xml.find(L'>', pos + prefix.size());
			if (gt == std::wstring::npos) return false;
			size_t start = gt + 1;

			std::wstring endTag = L"</" + name + L">";
			size_t end = xml.find(endTag, start);
			if (end == std::wstring::npos || end <= start) return false;

			out = xml.substr(start, end - start);
			printf("ReadHauptwerkStandbyOrgans: <%S> found (%zu chars)\n",
				name.c_str(), out.size());
			return true;
		};

		std::wstring hauptwerkSection;
		if (!trySectionWithAttrs(hwXml, L"Hauptwerk", hauptwerkSection))
			return {};

		std::wstring objectListSection;
		if (!trySectionWithAttrs(hauptwerkSection, L"ObjectList", objectListSection))
			return {};

		std::wstring generalSection;
		if (!trySectionWithAttrs(objectListSection, L"_General", generalSection))
			return {};

		// Extract Standby_Organ01 .. Standby_Organ08
		std::wstring result;
		for (int i = 1; i <= 8; ++i)
		{
			std::wstring tagName = L"Standby_Organ0" + std::to_wstring(i);
			std::wstring startTag = L"<" + tagName + L">";
			std::wstring endTag = L"</" + tagName + L">";
			std::wstring value;
			if (TryGetTagStringValue(generalSection, startTag, endTag, value) && !value.empty())
			{
				// Strip the known ".Organ_Hauptwerk_xml" suffix so that only
				// the organ name remains.  Fall back to stripping from the
				// last '.' if the expected suffix is not present.
				// e.g. "Leens Hinsz.Organ_Hauptwerk_xml" -> "Leens Hinsz"
				constexpr wchar_t kSuffix[] = L".Organ_Hauptwerk_xml";
				constexpr size_t kSuffixLen = (sizeof(kSuffix) / sizeof(wchar_t)) - 1;
				if (value.size() > kSuffixLen &&
					value.compare(value.size() - kSuffixLen, kSuffixLen, kSuffix) == 0)
				{
					value = value.substr(0, value.size() - kSuffixLen);
				}
				else
				{
					size_t dot = value.rfind(L'.');
					if (dot != std::wstring::npos)
						value = value.substr(0, dot);
				}

				result += L"    <" + tagName + L">" + value + L"</" + tagName + L">\r\n";
				printf("ReadHauptwerkStandbyOrgans: %S = %S\n", tagName.c_str(), value.c_str());
			}
		}

		// Extract sborg09 .. sborg32
		constexpr wchar_t kSuffix[] = L".Organ_Hauptwerk_xml";
		constexpr size_t kSuffixLen = (sizeof(kSuffix) / sizeof(wchar_t)) - 1;
		for (int i = 9; i <= 32; ++i)
		{
			std::wstring tagName = std::wstring(L"sborg") + (i < 10 ? L"0" : L"") + std::to_wstring(i);
			std::wstring startTag = L"<" + tagName + L">";
			std::wstring endTag = L"</" + tagName + L">";
			std::wstring value;
			if (TryGetTagStringValue(generalSection, startTag, endTag, value) && !value.empty())
			{
				if (value.size() > kSuffixLen &&
					value.compare(value.size() - kSuffixLen, kSuffixLen, kSuffix) == 0)
				{
					value = value.substr(0, value.size() - kSuffixLen);
				}
				else
				{
					size_t dot = value.rfind(L'.');
					if (dot != std::wstring::npos)
						value = value.substr(0, dot);
				}

				result += L"    <" + tagName + L">" + value + L"</" + tagName + L">\r\n";
				printf("ReadHauptwerkStandbyOrgans: %S = %S\n", tagName.c_str(), value.c_str());
			}
		}

		return result;
	}

	// Scan {RootFolder_HauptwerkSampleSetsAndComponents}\OrganDefinitions for
	// installed organ files (*.Organ_Hauptwerk_xml) and return an XML fragment
	// with one <Organ id="NN">name</Organ> entry per file.
	std::wstring ReadHauptwerkInstalledOrgans()
	{
		std::wstring sampleSetsRoot = s_rootHauptwerkSampleSets;
		if (sampleSetsRoot.empty())
		{
			// Fallback: try to load from Settings.xml in case the cache
			// has not been populated yet.
			std::wstring xml;
			if (TryReadSettingsXml(xml))
			{
				std::wstring opts;
				if (TryGetSection(xml, L"Options", opts))
					TryGetTagStringValue(opts,
						L"<RootFolder_HauptwerkSampleSetsAndComponents>",
						L"</RootFolder_HauptwerkSampleSetsAndComponents>",
						sampleSetsRoot);
			}
		}
		if (sampleSetsRoot.empty())
		{
			printf("ReadHauptwerkInstalledOrgans: RootFolder_HauptwerkSampleSetsAndComponents not configured.\n");
			return {};
		}

		// Remove trailing backslash if present.
		if (sampleSetsRoot.back() == L'\\' || sampleSetsRoot.back() == L'/')
			sampleSetsRoot.pop_back();

		std::wstring searchPath = sampleSetsRoot + L"\\OrganDefinitions\\*";
		printf("ReadHauptwerkInstalledOrgans: scanning %S\n", searchPath.c_str());

		WIN32_FIND_DATAW fd = {};
		HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			printf("ReadHauptwerkInstalledOrgans: no files found (error %lu)\n", GetLastError());
			return {};
		}

		constexpr wchar_t kSuffix[] = L".Organ_Hauptwerk_xml";
		constexpr size_t kSuffixLen = (sizeof(kSuffix) / sizeof(wchar_t)) - 1;

		std::wstring result;
		int id = 1;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			std::wstring name(fd.cFileName);
			if (name.size() <= kSuffixLen ||
				name.compare(name.size() - kSuffixLen, kSuffixLen, kSuffix) != 0)
				continue;

			name = name.substr(0, name.size() - kSuffixLen);

			// Read Identification_Name from inside the organ definition XML file
			std::wstring displayName;
			{
				std::wstring organFilePath = sampleSetsRoot + L"\\OrganDefinitions\\" + std::wstring(fd.cFileName);
				HANDLE fh = CreateFileW(organFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (fh != INVALID_HANDLE_VALUE)
				{
					DWORD fileSize = GetFileSize(fh, nullptr);
					if (fileSize != INVALID_FILE_SIZE && fileSize > 0)
					{
						// Read only the first 128KB — Identification_Name is near the start of the file
						DWORD readSize = (std::min)(fileSize, static_cast<DWORD>(131072));
						std::string raw(readSize, '\0');
						DWORD bytesRead = 0;
						if (ReadFile(fh, raw.data(), readSize, &bytesRead, nullptr) && bytesRead > 0)
						{
							int wideSize = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
								static_cast<int>(bytesRead), nullptr, 0);
							if (wideSize > 0)
							{
								std::wstring hwXml(wideSize, L'\0');
								MultiByteToWideChar(CP_UTF8, 0, raw.data(),
									static_cast<int>(bytesRead), hwXml.data(), wideSize);
								TryGetTagStringValue(hwXml,
									L"<Identification_Name>",
									L"</Identification_Name>",
									displayName);
							}
						}
					}
					CloseHandle(fh);
				}
			}

			std::wstring idStr = (id < 10 ? L"0" : L"") + std::to_wstring(id);
			std::wstring displayAttr = displayName.empty() ? L"" : L" displayName=\"" + displayName + L"\"";
			result += L"    <Organ id=\"" + idStr + L"\"" + displayAttr + L">" + name + L"</Organ>\r\n";
			printf("ReadHauptwerkInstalledOrgans: [%02d] %S (displayName: %S)\n", id, name.c_str(),
				displayName.empty() ? L"(none)" : displayName.c_str());
			++id;
		} while (FindNextFileW(hFind, &fd));

		FindClose(hFind);
		return result;
	}

	void EnsureStreamDeckSettingsLoaded()
	{
		if (s_streamDeckSettingsLoaded) return;
		s_streamDeckSettingsLoaded = true;
		std::wstring xml;
		if (!TryReadSettingsXml(xml)) return;
		std::wstring section;
		if (!TryGetSection(xml, L"StreamDeck", section)) return;
		TryGetTagStringValue(section, L"<CC>", L"</CC>", s_cachedStreamDeckCC);
		TryGetTagStringValue(section, L"<MidiOut>", L"</MidiOut>", s_cachedStreamDeckMidiOut);
		TryGetTagStringValue(section, L"<MidiIn>", L"</MidiIn>", s_cachedStreamDeckMidiIn);
	}

	bool WriteSettingsXml(const std::wstring& inputDeviceName, const std::wstring& input2DeviceName, const std::wstring& outputDeviceName, const std::wstring& output2DeviceName, bool routerEnabled, bool closeSettingsOnDisconnect, bool showDebugConsole, bool checkForUpdateOnStart, const DeviceEnabledStates& devEnabled)
	{
		std::wstring settingsDir = GetSettingsDirPath();
		if (!CreateDirectoryW(settingsDir.c_str(), nullptr))
		{
			DWORD error = GetLastError();
			if (error != ERROR_ALREADY_EXISTS)
			{
				return false;
			}
		}

		// Enumerate all currently available MIDI devices.
		std::wstring inputDevices;
		UINT numInDevs = midiInGetNumDevs();
		printf("WriteSettingsXml: %u input device(s), ", numInDevs);
		for (UINT i = 0; i < numInDevs; ++i)
		{
			MIDIINCAPS inCaps = {};
			if (midiInGetDevCaps(i, &inCaps, sizeof(inCaps)) == MMSYSERR_NOERROR)
				inputDevices += L"    <Device id=\"" + std::to_wstring(i) + L"\">" + std::wstring(inCaps.szPname) + L"</Device>\r\n";
		}

		std::wstring outputDevices;
		UINT numOutDevs = midiOutGetNumDevs();
		printf("%u output device(s)\n", numOutDevs);
		for (UINT i = 0; i < numOutDevs; ++i)
		{
			MIDIOUTCAPS outCaps = {};
			if (midiOutGetDevCaps(i, &outCaps, sizeof(outCaps)) == MMSYSERR_NOERROR)
				outputDevices += L"    <Device id=\"" + std::to_wstring(i) + L"\">" + std::wstring(outCaps.szPname) + L"</Device>\r\n";
		}

		// Resolve current device indices so we can store them as id attributes.
		auto buildDeviceTag = [](const wchar_t* tag, const std::wstring& name, bool isOutput, bool enabled) -> std::wstring
		{
			UINT idx = 0;
			bool found = false;
			if (!name.empty())
				found = isOutput ? FindMidiOutputDeviceIndex(name, idx) : FindMidiInputDeviceIndex(name, idx);
			std::wstring s = L"      <";
			s += tag;
			if (found)
				s += L" id=\"" + std::to_wstring(idx) + L"\"";
			s += L" enabled=\"";
			s += enabled ? L"1" : L"0";
			s += L"\">";
			s += name;
			s += L"</";
			s += tag;
			s += L">\r\n";
			return s;
		};

		// Ensure standby organ names are loaded (cached on first call).
		EnsureStandbyOrgansLoaded();
		const std::wstring& standbyOrgans = s_cachedStandbyOrgans;

		// Ensure installed organs are loaded (cached on first call).
		EnsureInstalledOrgansLoaded();
		const std::wstring& installedOrgans = s_cachedInstalledOrgans;

		// Ensure Stream Deck settings are loaded (cached on first call).
		EnsureStreamDeckSettingsLoaded();

		std::wstring xml = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
			L"<Settings>\r\n"
			L"  <Midi>\r\n"
			L"    <SettingsDevices>\r\n"
			+ buildDeviceTag(L"MidiInputDevice01", inputDeviceName, false, devEnabled.input1)
				+ buildDeviceTag(L"MidiInputDevice02", input2DeviceName, false, devEnabled.input2)
				+ buildDeviceTag(L"MidiOutputDevice01", outputDeviceName, true, devEnabled.output1)
				+ buildDeviceTag(L"MidiOutputDevice02", output2DeviceName, true, devEnabled.output2) +
			L"    </SettingsDevices>\r\n"
			L"    <MidiRouterEnabled>" + std::to_wstring(routerEnabled ? 1 : 0) + L"</MidiRouterEnabled>\r\n"
			L"  </Midi>\r\n"
			L"  <Options>\r\n"
			L"    <CloseSettingsOnDisconnect>" + std::to_wstring(closeSettingsOnDisconnect ? 1 : 0) + L"</CloseSettingsOnDisconnect>\r\n"
			L"    <ShowDebugConsole>" + std::to_wstring(showDebugConsole ? 1 : 0) + L"</ShowDebugConsole>\r\n"
			L"    <CheckForUpdateOnStart>" + std::to_wstring(checkForUpdateOnStart ? 1 : 0) + L"</CheckForUpdateOnStart>\r\n"
			L"    <RootFolder_HauptwerkApplication>" + s_rootHauptwerkApp + L"</RootFolder_HauptwerkApplication>\r\n"
			L"    <RootFolder_HauptwerkUserData>" + s_rootHauptwerkUserData + L"</RootFolder_HauptwerkUserData>\r\n"
			L"    <RootFolder_HauptwerkSampleSetsAndComponents>" + s_rootHauptwerkSampleSets + L"</RootFolder_HauptwerkSampleSetsAndComponents>\r\n"
			L"    <RootFolder_HauptwerkInternalWorkingFiles>" + s_rootHauptwerkWorkingFiles + L"</RootFolder_HauptwerkInternalWorkingFiles>\r\n"
			L"  </Options>\r\n"
			L"  <CurrentMidiInputDevices>\r\n" + inputDevices +
			L"  </CurrentMidiInputDevices>\r\n"
			L"  <CurrentMidiOutputDevices>\r\n" + outputDevices +
			L"  </CurrentMidiOutputDevices>\r\n"
			L"  <StandbyeOrgans>\r\n" + standbyOrgans +
			L"  </StandbyeOrgans>\r\n"
			L"  <InstalledOrgans>\r\n" + installedOrgans +
			L"  </InstalledOrgans>\r\n"
			L"  <StreamDeck>\r\n"
			L"    <CC>" + s_cachedStreamDeckCC + L"</CC>\r\n"
			L"    <MidiOut>" + s_cachedStreamDeckMidiOut + L"</MidiOut>\r\n"
			L"    <MidiIn>" + s_cachedStreamDeckMidiIn + L"</MidiIn>\r\n"
			L"  </StreamDeck>\r\n"
			L"  <Organ_Info>\r\n"
			L"  </Organ_Info>\r\n"
			L"</Settings>\r\n";

		int utf8Size = WideCharToMultiByte(CP_UTF8, 0, xml.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8Size <= 0)
		{
			return false;
		}

		std::string utf8(utf8Size - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, xml.c_str(), -1, utf8.data(), utf8Size, nullptr, nullptr);

		std::wstring settingsFile = GetSettingsFilePath();
		HANDLE fileHandle = CreateFileW(settingsFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (fileHandle == INVALID_HANDLE_VALUE)
		{
			printf("WriteSettingsXml: CreateFileW FAILED (error %lu)\n", GetLastError());
			return false;
		}

		DWORD bytesWritten = 0;
		BOOL ok = WriteFile(fileHandle, utf8.data(), static_cast<DWORD>(utf8.size()), &bytesWritten, nullptr);
		CloseHandle(fileHandle);

		return ok != FALSE && bytesWritten == utf8.size();
	}
}
bool SaveSelectedDeviceId(UINT deviceId)
{
    std::wstring input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(GetMidiInputDeviceName(deviceId), input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveSelectedOutputDeviceId(UINT deviceId)
{
    std::wstring inputName, input2Name, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, GetMidiOutputDeviceName(deviceId), output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveMidiRouterEnabled(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, enabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveCloseSettingsOnDisconnect(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, enabled, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool LoadSelectedDeviceId(UINT& deviceId)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
        return false;

    std::wstring devicesSection;
    if (!TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;

    std::wstring name;
    if (!TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", name))
        return false;

    return FindMidiInputDeviceIndex(name, deviceId);
}

bool SaveSelectedInput2DeviceId(UINT deviceId)
{
    std::wstring inputName, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, GetMidiInputDeviceName(deviceId), outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool LoadSelectedInput2DeviceId(UINT& deviceId)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
        return false;

    std::wstring devicesSection;
    if (!TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;

    std::wstring name;
    if (!TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", name))
        return false;

    return FindMidiInputDeviceIndex(name, deviceId);
}

bool LoadSelectedOutputDeviceId(UINT& deviceId)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
        return false;

    std::wstring devicesSection;
    if (!TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;

    std::wstring name;
    if (!TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", name))
        return false;

    return FindMidiOutputDeviceIndex(name, deviceId);
}

bool LoadMidiRouterEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
    {
        return false;
    }

    UINT value = 0;
    // Require the <Midi> section and the <MidiRouterEnabled> tag inside it.
    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
    {
        return false;
    }

    if (!TryGetTagValue(midiSection, L"<MidiRouterEnabled>", L"</MidiRouterEnabled>", value))
    {
        return false;
    }

    enabled = value != 0;
    return true;
}

bool LoadCloseSettingsOnDisconnect(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
    {
        return false;
    }

    UINT value = 0;
    // Require the <Options> section and the <CloseSettingsOnDisconnect> tag inside it.
    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
    {
        return false;
    }

    if (!TryGetTagValue(optionsSection, L"<CloseSettingsOnDisconnect>", L"</CloseSettingsOnDisconnect>", value))
    {
        return false;
    }

    enabled = value != 0;
    return true;
}

bool LoadShowDebugConsole(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
    {
        return false;
    }

    UINT value = 0;
    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
    {
        return false;
    }

    if (!TryGetTagValue(optionsSection, L"<ShowDebugConsole>", L"</ShowDebugConsole>", value))
    {
        return false;
    }

    enabled = value != 0;
    return true;
}

bool SaveShowDebugConsole(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, enabled, checkForUpdateOnStart, devEnabled);
}

bool LoadCheckForUpdateOnStart(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    UINT value = 1;
    if (!TryGetTagValue(optionsSection, L"<CheckForUpdateOnStart>", L"</CheckForUpdateOnStart>", value))
    {
        enabled = true;
        return true;
    }

    enabled = value != 0;
    return true;
}

bool SaveCheckForUpdateOnStart(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, enabled, devEnabled);
}

void RefreshSettingsFile()
{
    std::wstring inputName, input2Name, outputName, output2Name;
    bool routerEnabled = false;
    bool closeSettingsOnDisconnect = false;

    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }

    // --- Snapshot-based connect / disconnect detection ---
    // Build current device name lists.
    std::vector<std::wstring> curInputs, curOutputs;
    {
        UINT n = midiInGetNumDevs();
        for (UINT i = 0; i < n; ++i)
        {
            MIDIINCAPS c = {};
            if (midiInGetDevCaps(i, &c, sizeof(c)) == MMSYSERR_NOERROR)
                curInputs.emplace_back(c.szPname);
        }
    }
    {
        UINT n = midiOutGetNumDevs();
        for (UINT i = 0; i < n; ++i)
        {
            MIDIOUTCAPS c = {};
            if (midiOutGetDevCaps(i, &c, sizeof(c)) == MMSYSERR_NOERROR)
                curOutputs.emplace_back(c.szPname);
        }
    }

    static std::vector<std::wstring> s_prevInputs;
    static std::vector<std::wstring> s_prevOutputs;
    static bool s_firstRun = true;

    auto toSet = [](const std::vector<std::wstring>& v) { return std::set<std::wstring>(v.begin(), v.end()); };
    std::set<std::wstring> prevInSet  = toSet(s_prevInputs);
    std::set<std::wstring> prevOutSet = toSet(s_prevOutputs);
    std::set<std::wstring> curInSet   = toSet(curInputs);
    std::set<std::wstring> curOutSet  = toSet(curOutputs);

    // Compute added / removed
    std::vector<std::wstring> addedInputs, removedInputs, addedOutputs, removedOutputs;
    if (!s_firstRun)
    {
        for (const auto& d : curInSet)
            if (prevInSet.find(d) == prevInSet.end()) addedInputs.push_back(d);
        for (const auto& d : prevInSet)
            if (curInSet.find(d) == curInSet.end()) removedInputs.push_back(d);
        for (const auto& d : curOutSet)
            if (prevOutSet.find(d) == prevOutSet.end()) addedOutputs.push_back(d);
        for (const auto& d : prevOutSet)
            if (curOutSet.find(d) == curOutSet.end()) removedOutputs.push_back(d);

        for (const auto& d : addedInputs)
            printf("MIDI input  CONNECTED:    %S\n", d.c_str());
        for (const auto& d : removedInputs)
            printf("MIDI input  DISCONNECTED: %S\n", d.c_str());
        for (const auto& d : addedOutputs)
            printf("MIDI output CONNECTED:    %S\n", d.c_str());
        for (const auto& d : removedOutputs)
            printf("MIDI output DISCONNECTED: %S\n", d.c_str());
    }
    s_prevInputs  = curInputs;
    s_prevOutputs = curOutputs;
    s_firstRun    = false;

    bool isDisconnect = !removedInputs.empty() || !removedOutputs.empty();
    bool isConnect    = !addedInputs.empty()    || !addedOutputs.empty();

    // Detect whether any saved device is no longer available.
    // Show one combined MessageBox at most once per disappearance;
    // once dismissed it will not reappear until the device comes
    // back and disappears again.
    static std::set<std::wstring> s_alreadyNotified;

    struct DeviceCheck { const wchar_t* label; const std::wstring& name; bool isOutput; };
    DeviceCheck checks[] = {
        { L"MidiInputDevice01",  inputName,   false },
        { L"MidiInputDevice02",  input2Name,  false },
        { L"MidiOutputDevice01", outputName,  true  },
        { L"MidiOutputDevice02", output2Name, true  },
    };

    std::wstring missingMsg;
    for (const auto& dc : checks)
    {
        if (dc.name.empty())
            continue;

        UINT dummy = 0;
        bool found = dc.isOutput
            ? FindMidiOutputDeviceIndex(dc.name, dummy)
            : FindMidiInputDeviceIndex(dc.name, dummy);

        if (found)
        {
            // Device is back — allow a future notification if it disappears again.
            s_alreadyNotified.erase(dc.name);
        }
        else if (s_alreadyNotified.find(dc.name) == s_alreadyNotified.end())
        {
            if (!missingMsg.empty())
                missingMsg += L"\n";
            missingMsg += L"[";
            missingMsg += dc.label;
            missingMsg += L"]  ";
            missingMsg += dc.name;
            s_alreadyNotified.insert(dc.name);
        }
    }

    if (!missingMsg.empty())
    {
        // If configured devices are missing, it is always a disconnection
        // scenario — even on first run when the snapshot diff is empty.
        isDisconnect = true;

        // Build a detailed message showing what happened.
        std::wstring fullMsg;
        fullMsg += L"DISCONNESSIONE rilevata:\n";
        if (!removedInputs.empty() || !removedOutputs.empty())
        {
            for (const auto& d : removedInputs)
                fullMsg += L"  \u2212 Input:  " + d + L"\n";
            for (const auto& d : removedOutputs)
                fullMsg += L"  \u2212 Output: " + d + L"\n";
        }
        fullMsg += L"\n";
        fullMsg += L"Dispositivo/i MIDI configurato/i non pi\u00f9 disponibile/i:\n\n";
        fullMsg += missingMsg;
        fullMsg += L"\n\nRiconnettere il dispositivo per abilitare OK.";

        // Build a modal dialog in memory with a disabled OK button.
        // A timer polls the MIDI subsystem every 500ms and enables OK
        // once all missing devices reappear.
        struct DlgData
        {
            const wchar_t* message;
            DeviceCheck* checks;
            int checkCount;
            HFONT hFontNormal;
            HFONT hFontBold;
            HBRUSH hWhiteBrush;
            bool blinkToggle;
        };

        DlgData dlgData = { fullMsg.c_str(), checks, _countof(checks), nullptr, nullptr, nullptr, false };

        constexpr int kIconCtrlId = 99;
        constexpr int kMessageId = 100;
        constexpr int kStatusTextId = 102;
        constexpr int kSeparatorId = 103;
        constexpr int kSkipBtnId = 200;

        auto dlgProc = [](HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR
        {
            constexpr int kIconId = 99;
            constexpr int kMsgId = 100;
            constexpr int kStatusId = 102;
            constexpr int kSkipId = 200;

            switch (msg)
            {
            case WM_INITDIALOG:
            {
                SetWindowLongPtrW(hDlg, GWLP_USERDATA, lp);
                auto* data = reinterpret_cast<DlgData*>(lp);

                SetWindowTextW(hDlg, L"AhlbornBridge \u2014 Disconnessione MIDI");

                // Create fonts (Segoe UI for a modern look)
                data->hFontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                data->hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                data->hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));

                // Centre on screen and keep on top
                RECT rc;
                GetWindowRect(hDlg, &rc);
                int scrW = GetSystemMetrics(SM_CXSCREEN);
                int scrH = GetSystemMetrics(SM_CYSCREEN);
                int dlgW = rc.right - rc.left;
                int dlgH = rc.bottom - rc.top;
                SetWindowPos(hDlg, HWND_TOPMOST,
                    (scrW - dlgW) / 2, (scrH - dlgH) / 2,
                    0, 0, SWP_NOSIZE);
                SetForegroundWindow(hDlg);

                // Warning icon (32x32 system icon)
                HWND hIconCtrl = CreateWindowW(L"STATIC", nullptr,
                    WS_CHILD | WS_VISIBLE | SS_ICON,
                    20, 20, 32, 32, hDlg, (HMENU)kIconId, nullptr, nullptr);
                SendMessageW(hIconCtrl, STM_SETICON,
                    (WPARAM)LoadIconW(nullptr, IDI_WARNING), 0);

                // Message text (right of the icon)
                HWND hMsg = CreateWindowW(L"STATIC", data->message,
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    64, 20, 390, 170, hDlg, (HMENU)kMsgId, nullptr, nullptr);
                SendMessageW(hMsg, WM_SETFONT, (WPARAM)data->hFontNormal, TRUE);

                // Status indicator text (\u25CF = filled circle)
                HWND hStatus = CreateWindowW(L"STATIC",
                    L"\u25CF  In attesa di riconnessione...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    24, 200, 420, 22, hDlg, (HMENU)kStatusId, nullptr, nullptr);
                SendMessageW(hStatus, WM_SETFONT, (WPARAM)data->hFontBold, TRUE);

                // Etched separator line
                CreateWindowW(L"STATIC", nullptr,
                    WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                    16, 234, 440, 2, hDlg, nullptr, nullptr, nullptr);

                // SKIP button (always enabled, closes dialog and opens Settings)
                HWND hSkipBtn = CreateWindowW(L"BUTTON", L"SKIP",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    260, 248, 90, 30, hDlg, (HMENU)kSkipId, nullptr, nullptr);
                SendMessageW(hSkipBtn, WM_SETFONT, (WPARAM)data->hFontNormal, TRUE);

                // OK button (initially disabled, enabled when devices reconnect)
                HWND hBtn = CreateWindowW(L"BUTTON", L"OK",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
                    360, 248, 90, 30, hDlg, (HMENU)IDOK, nullptr, nullptr);
                SendMessageW(hBtn, WM_SETFONT, (WPARAM)data->hFontNormal, TRUE);

                SetTimer(hDlg, 1, 500, nullptr);
                return TRUE;
            }
            case WM_CTLCOLORDLG:
            {
                auto* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
                if (data && data->hWhiteBrush)
                    return (INT_PTR)data->hWhiteBrush;
                break;
            }
            case WM_CTLCOLORSTATIC:
            {
                HDC hdc = (HDC)wp;
                HWND hCtrl = (HWND)lp;
                auto* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
                if (!data) break;
                SetBkMode(hdc, TRANSPARENT);
                int ctrlId = GetDlgCtrlID(hCtrl);
                if (ctrlId == kStatusId)
                {
                    HWND hBtn = GetDlgItem(hDlg, IDOK);
                    bool reconnected = hBtn && IsWindowEnabled(hBtn);
                    if (reconnected)
                        SetTextColor(hdc, RGB(0, 160, 0));
                    else
                        SetTextColor(hdc, data->blinkToggle ? RGB(200, 0, 0) : RGB(120, 0, 0));
                }
                else
                {
                    SetTextColor(hdc, RGB(30, 30, 30));
                }
                return (INT_PTR)data->hWhiteBrush;
            }
            case WM_TIMER:
            {
                if (wp != 1) break;
                auto* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
                if (!data) break;

                data->blinkToggle = !data->blinkToggle;

                bool allFound = true;
                for (int i = 0; i < data->checkCount; ++i)
                {
                    const auto& dc = data->checks[i];
                    if (dc.name.empty()) continue;
                    UINT dummy = 0;
                    bool found = dc.isOutput
                        ? FindMidiOutputDeviceIndex(dc.name, dummy)
                        : FindMidiInputDeviceIndex(dc.name, dummy);
                    if (!found) { allFound = false; break; }
                }

                HWND hBtn = GetDlgItem(hDlg, IDOK);
                if (hBtn)
                {
                    BOOL wasEnabled = IsWindowEnabled(hBtn);
                    EnableWindow(hBtn, allFound ? TRUE : FALSE);

                    HWND hSkipBtn = GetDlgItem(hDlg, kSkipId);
                    if (hSkipBtn)
                        EnableWindow(hSkipBtn, allFound ? FALSE : TRUE);

                    if (allFound && !wasEnabled)
                    {
                        // Update message
                        HWND hLabel = GetDlgItem(hDlg, kMsgId);
                        if (hLabel)
                        {
                            SetWindowTextW(hLabel,
                                L"Dispositivo/i MIDI riconnesso/i.\n\n"
                                L"Premere OK per continuare.");
                        }
                        // Update status text
                        HWND hStatus = GetDlgItem(hDlg, kStatusId);
                        if (hStatus)
                        {
                            SetWindowTextW(hStatus, L"\u25CF  Dispositivo/i riconnesso/i");
                            InvalidateRect(hStatus, nullptr, TRUE);
                        }
                        // Swap icon to IDI_INFORMATION
                        HWND hIconCtrl = GetDlgItem(hDlg, kIconId);
                        if (hIconCtrl)
                            SendMessageW(hIconCtrl, STM_SETICON,
                                (WPARAM)LoadIconW(nullptr, IDI_INFORMATION), 0);
                        SetWindowTextW(hDlg, L"AhlbornBridge \u2014 Dispositivo riconnesso");
                    }
                }

                // Repaint the status label so its colour updates (blink or green)
                HWND hStatus = GetDlgItem(hDlg, kStatusId);
                if (hStatus) InvalidateRect(hStatus, nullptr, TRUE);

                return TRUE;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == IDOK)
                {
                    KillTimer(hDlg, 1);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                if (LOWORD(wp) == kSkipId)
                {
                    KillTimer(hDlg, 1);
                    EndDialog(hDlg, kSkipId);
                    return TRUE;
                }
                break;
            case WM_CLOSE:
                KillTimer(hDlg, 1);
                EndDialog(hDlg, kSkipId);
                return TRUE;
            case WM_DESTROY:
            {
                auto* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
                if (data)
                {
                    if (data->hFontNormal) { DeleteObject(data->hFontNormal); data->hFontNormal = nullptr; }
                    if (data->hFontBold)   { DeleteObject(data->hFontBold);   data->hFontBold = nullptr; }
                    if (data->hWhiteBrush) { DeleteObject(data->hWhiteBrush); data->hWhiteBrush = nullptr; }
                }
                break;
            }
            }
            return FALSE;
        };

        // Build a DLGTEMPLATE in memory (no resource file needed).
        #pragma pack(push, 4)
        struct { DLGTEMPLATE dt; WORD menu; WORD cls; WORD title; } dlgTpl = {};
        #pragma pack(pop)
        dlgTpl.dt.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        dlgTpl.dt.cx = 240;
        dlgTpl.dt.cy = 145;

        INT_PTR dlgResult = DialogBoxIndirectParamW(nullptr,
            reinterpret_cast<LPCDLGTEMPLATEW>(&dlgTpl),
            nullptr, dlgProc, reinterpret_cast<LPARAM>(&dlgData));

        // Dialog closed — clear the notification state so a future
        // disconnection will be detected again.
        for (const auto& dc : checks)
        {
            if (!dc.name.empty())
                s_alreadyNotified.erase(dc.name);
        }

        if (dlgResult == kSkipBtnId)
        {
            // SKIP pressed — open Settings window to change device
            ShowSettingsWindow(GetModuleHandle(nullptr), nullptr);
        }
        else
        {
            // Reopen MIDI ports after reconnection so they are not left closed.
            UINT idx = 0;
            if (!inputName.empty() && FindMidiInputDeviceIndex(inputName, idx))
            {
                printf("Reopening MIDI input device 01: [%S]\n", inputName.c_str());
                SwitchMidiInputDevice(idx);
            }
            if (!input2Name.empty() && FindMidiInputDeviceIndex(input2Name, idx))
            {
                printf("Reopening MIDI input device 02: [%S]\n", input2Name.c_str());
                SwitchMidiInput2Device(idx);
            }
            if (!outputName.empty() && FindMidiOutputDeviceIndex(outputName, idx))
            {
                printf("Reopening MIDI output device 01: [%S]\n", outputName.c_str());
                SwitchMidiOutputDevice(idx);
            }
            if (!output2Name.empty() && FindMidiOutputDeviceIndex(output2Name, idx))
            {
                printf("Reopening MIDI output device 02: [%S]\n", output2Name.c_str());
                SwitchMidiOutput2Device(idx);
            }
        }

        // Update the snapshot to reflect the current device state after
        // reconnection, so the next diff correctly detects any change.
        s_prevInputs.clear();
        s_prevOutputs.clear();
        {
            UINT n = midiInGetNumDevs();
            for (UINT i = 0; i < n; ++i)
            {
                MIDIINCAPS c = {};
                if (midiInGetDevCaps(i, &c, sizeof(c)) == MMSYSERR_NOERROR)
                    s_prevInputs.emplace_back(c.szPname);
            }
        }
        {
            UINT n = midiOutGetNumDevs();
            for (UINT i = 0; i < n; ++i)
            {
                MIDIOUTCAPS c = {};
                if (midiOutGetDevCaps(i, &c, sizeof(c)) == MMSYSERR_NOERROR)
                    s_prevOutputs.emplace_back(c.szPname);
            }
        }
    }

    LoadMidiRouterEnabled(routerEnabled);
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    bool ok = WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
    printf("RefreshSettingsFile: WriteSettingsXml returned %s\n", ok ? "true" : "false");
}

void ReloadStandbyOrgans()
{
    s_standbyOrgansLoaded = false;
    EnsureStandbyOrgansLoaded();

    // Persist the refreshed cache to Settings.xml so that organs
    // removed from the Hauptwerk config are no longer kept on disk.
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

void ReloadInstalledOrgans()
{
    s_installedOrgansLoaded = false;
    EnsureInstalledOrgansLoaded();

    // Persist the refreshed cache to Settings.xml so that organs
    // added or removed from OrganDefinitions are updated on disk.
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);

    // Notify the Stream Deck plugin that the organ list has changed
    NotifyStreamDeckOrganList();
}

// ---------------------------------------------------------------
// OrganDefinitions folder watcher
// ---------------------------------------------------------------
namespace
{
    HANDLE g_organWatcherThread = nullptr;
    HANDLE g_organWatcherStopEvent = nullptr;

    DWORD WINAPI OrganFolderWatcherThread(LPVOID)
    {
        std::wstring organDir = s_rootHauptwerkSampleSets;
        if (organDir.empty())
        {
            printf("[OrganWatcher] SampleSets path not configured, watcher exiting.\n");
            return 0;
        }
        if (organDir.back() == L'\\' || organDir.back() == L'/')
            organDir.pop_back();
        organDir += L"\\OrganDefinitions";

        DWORD attrs = GetFileAttributesW(organDir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            printf("[OrganWatcher] OrganDefinitions folder not found: %S\n", organDir.c_str());
            return 0;
        }

        printf("[OrganWatcher] Watching: %S\n", organDir.c_str());

        const DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;

        while (true)
        {
            // Create a fresh notification handle each iteration so the
            // baseline is always up-to-date.  FindNextChangeNotification
            // can silently stop working for LAST_WRITE on some systems.
            HANDLE hChange = FindFirstChangeNotificationW(
                organDir.c_str(), FALSE, kFilter);

            if (hChange == INVALID_HANDLE_VALUE)
            {
                printf("[OrganWatcher] FindFirstChangeNotification failed (error %lu), retrying...\n", GetLastError());
                if (WaitForSingleObject(g_organWatcherStopEvent, 3000) == WAIT_OBJECT_0)
                    break;
                continue;
            }

            HANDLE waitHandles[2] = { g_organWatcherStopEvent, hChange };
            DWORD result = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            FindCloseChangeNotification(hChange);

            if (result == WAIT_OBJECT_0)
                break;

            if (result == WAIT_OBJECT_0 + 1)
            {
                // Wait for file operations to settle before rescanning.
                Sleep(1500);

                // Don't reload while an organ is being loaded — Hauptwerk
                // may touch files in OrganDefinitions during loading, which
                // would trigger UpdateStreamDeckProfileTitles and kill SD.
                if (g_isLoadingOrgan.load())
                {
                    printf("[OrganWatcher] Change detected but organ is loading, skipping reload.\n");
                    continue;
                }

                printf("[OrganWatcher] Change detected in OrganDefinitions, reloading...\n");
                ReloadInstalledOrgans();
                printf("[OrganWatcher] InstalledOrgans refreshed.\n");
            }
            else
            {
                printf("[OrganWatcher] WaitForMultipleObjects returned %lu\n", result);
                break;
            }
        }

        printf("[OrganWatcher] Watcher stopped.\n");
        return 0;
    }
}

void StartOrganFolderWatcher()
{
    if (g_organWatcherThread)
        return;

    g_organWatcherStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_organWatcherStopEvent)
        return;

    g_organWatcherThread = CreateThread(nullptr, 0, OrganFolderWatcherThread, nullptr, 0, nullptr);
    if (!g_organWatcherThread)
    {
        CloseHandle(g_organWatcherStopEvent);
        g_organWatcherStopEvent = nullptr;
    }
}

void StopOrganFolderWatcher()
{
    if (!g_organWatcherThread)
        return;

    SetEvent(g_organWatcherStopEvent);
    WaitForSingleObject(g_organWatcherThread, 3000);
    CloseHandle(g_organWatcherThread);
    CloseHandle(g_organWatcherStopEvent);
    g_organWatcherThread = nullptr;
    g_organWatcherStopEvent = nullptr;
}

std::vector<std::wstring> LoadStandbyOrganNames()
{
    std::vector<std::wstring> names(32);
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return names;

    std::wstring section;
    if (!TryGetSection(xml, L"StandbyeOrgans", section))
        return names;

    // Read Standby_Organ01 .. Standby_Organ08
    for (int i = 1; i <= 8; ++i)
    {
        std::wstring tag = L"Standby_Organ0" + std::to_wstring(i);
        std::wstring startTag = L"<" + tag + L">";
        std::wstring endTag = L"</" + tag + L">";
        TryGetTagStringValue(section, startTag, endTag, names[i - 1]);
    }

    // Read sborg09 .. sborg32
    for (int i = 9; i <= 32; ++i)
    {
        std::wstring tag = std::wstring(L"sborg") + (i < 10 ? L"0" : L"") + std::to_wstring(i);
        std::wstring startTag = L"<" + tag + L">";
        std::wstring endTag = L"</" + tag + L">";
        TryGetTagStringValue(section, startTag, endTag, names[i - 1]);
    }

    return names;
}

std::vector<std::wstring> LoadInstalledOrganNames()
{
    std::vector<std::wstring> names;
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return names;

    std::wstring section;
    if (!TryGetSection(xml, L"InstalledOrgans", section))
        return names;

    // Parse <Organ id="NN">name</Organ> entries.
    size_t pos = 0;
    while (true)
    {
        size_t start = section.find(L"<Organ", pos);
        if (start == std::wstring::npos)
            break;
        size_t gt = section.find(L'>', start);
        if (gt == std::wstring::npos)
            break;
        size_t end = section.find(L"</Organ>", gt + 1);
        if (end == std::wstring::npos)
            break;
        names.push_back(section.substr(gt + 1, end - gt - 1));
        pos = end + 8; // skip past "</Organ>"
    }

    return names;
}

bool LoadHauptwerkAppPath(std::wstring& path)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    return TryGetTagStringValue(optionsSection,
        L"<RootFolder_HauptwerkApplication>", L"</RootFolder_HauptwerkApplication>", path)
        && !path.empty();
}

bool SaveSelectedOutput2DeviceId(UINT deviceId)
{
    std::wstring inputName, input2Name, outputName;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, GetMidiOutputDeviceName(deviceId), routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool LoadSelectedOutput2DeviceId(UINT& deviceId)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
        return false;

    std::wstring devicesSection;
    if (!TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;

    std::wstring name;
    if (!TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", name))
        return false;

    return FindMidiOutputDeviceIndex(name, deviceId);
}

bool InitHauptwerkPaths()
{
    if (s_hauptwerkPathsLoaded)
        return true;

    // 1) Try to load from Settings.xml (already configured).
    std::wstring savedAppPath;
    if (LoadHauptwerkAppPath(savedAppPath) && !savedAppPath.empty())
    {
        // Verify the saved path still exists on disk.
        DWORD savedAttrs = GetFileAttributesW(savedAppPath.c_str());
        bool savedExists = (savedAttrs != INVALID_FILE_ATTRIBUTES) && (savedAttrs & FILE_ATTRIBUTE_DIRECTORY);

        if (savedExists)
        {
            printf("InitHauptwerkPaths: Using saved path: %S\n", savedAppPath.c_str());
            s_rootHauptwerkApp = savedAppPath;

            // Also restore the other cached paths from Settings.xml.
            std::wstring xml;
            if (TryReadSettingsXml(xml))
            {
                std::wstring opts;
                if (TryGetSection(xml, L"Options", opts))
                {
                    TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>", L"</RootFolder_HauptwerkUserData>", s_rootHauptwerkUserData);
                    TryGetTagStringValue(opts, L"<RootFolder_HauptwerkSampleSetsAndComponents>", L"</RootFolder_HauptwerkSampleSetsAndComponents>", s_rootHauptwerkSampleSets);
                    TryGetTagStringValue(opts, L"<RootFolder_HauptwerkInternalWorkingFiles>", L"</RootFolder_HauptwerkInternalWorkingFiles>", s_rootHauptwerkWorkingFiles);
                }
            }
            // Invalidate cached organ data so it is re-read with the
            // restored paths (they may have been cached as empty
            // before InitHauptwerkPaths ran).
            s_standbyOrgansLoaded = false;
            s_installedOrgansLoaded = false;
            s_hauptwerkPathsLoaded = true;
            return true;
        }
        else
        {
            printf("InitHauptwerkPaths: Saved path no longer exists: %S\n", savedAppPath.c_str());
        }
    }

    // 2) Check the default installation path.
    const wchar_t* kDefaultPath = L"C:\\Program Files\\Hauptwerk Virtual Pipe Organ";
    DWORD attrs = GetFileAttributesW(kDefaultPath);
    bool defaultExists = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring appPath;
    if (defaultExists)
    {
        printf("InitHauptwerkPaths: Default path found: %S\n", kDefaultPath);
        appPath = kDefaultPath;
    }
    else
    {
        printf("InitHauptwerkPaths: Default path NOT found. Hauptwerk is not installed.\n");
        s_hauptwerkPathsLoaded = true;
        return false;
    }

    // 3) Store the application path and read FileLocations.
    s_rootHauptwerkApp = appPath;
    ReadHauptwerkFileLocations(appPath);

    // Invalidate cached organ data so it is re-read with the
    // now-configured paths (they may have been cached as empty
    // before the paths were available).
    s_standbyOrgansLoaded = false;
    s_installedOrgansLoaded = false;

    // 4) Persist to Settings.xml by triggering a write.
    RefreshSettingsFile();

    s_hauptwerkPathsLoaded = true;
    printf("InitHauptwerkPaths: Configuration complete.\n");
    return true;
}

bool LoadMidiInput1DeviceEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;
    std::wstring midiSection, devicesSection;
    if (!TryGetSection(xml, L"Midi", midiSection) || !TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;
    return TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", enabled);
}

bool LoadMidiInput2DeviceEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;
    std::wstring midiSection, devicesSection;
    if (!TryGetSection(xml, L"Midi", midiSection) || !TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;
    return TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", enabled);
}

bool LoadMidiOutput1DeviceEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;
    std::wstring midiSection, devicesSection;
    if (!TryGetSection(xml, L"Midi", midiSection) || !TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;
    return TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", enabled);
}

bool LoadMidiOutput2DeviceEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;
    std::wstring midiSection, devicesSection;
    if (!TryGetSection(xml, L"Midi", midiSection) || !TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        return false;
    return TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", enabled);
}

bool SaveMidiInput1DeviceEnabled(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    devEnabled.input1 = enabled;
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveMidiInput2DeviceEnabled(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    devEnabled.input2 = enabled;
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveMidiOutput1DeviceEnabled(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    devEnabled.output1 = enabled;
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveMidiOutput2DeviceEnabled(bool enabled)
{
    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    devEnabled.output2 = enabled;
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool SaveStreamDeckSettings(int ccNumber, const std::wstring& midiOut, const std::wstring& midiIn)
{
    s_cachedStreamDeckCC = std::to_wstring(ccNumber);
    s_cachedStreamDeckMidiOut = midiOut;
    s_cachedStreamDeckMidiIn = midiIn;
    s_streamDeckSettingsLoaded = true;

    std::wstring inputName, input2Name, outputName, output2Name;
    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", inputName);
            TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", input2Name);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", outputName);
            TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", output2Name);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }
    bool routerEnabled = false;
    LoadMidiRouterEnabled(routerEnabled);
    bool closeSettingsOnDisconnect = false;
    LoadCloseSettingsOnDisconnect(closeSettingsOnDisconnect);
    bool showDebugConsole = true;
    LoadShowDebugConsole(showDebugConsole);
    bool checkForUpdateOnStart = true;
    LoadCheckForUpdateOnStart(checkForUpdateOnStart);
    return WriteSettingsXml(inputName, input2Name, outputName, output2Name, routerEnabled, closeSettingsOnDisconnect, showDebugConsole, checkForUpdateOnStart, devEnabled);
}

bool LoadStreamDeckSettings(int& ccNumber, std::wstring& midiOut, std::wstring& midiIn)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring section;
    if (!TryGetSection(xml, L"StreamDeck", section))
        return false;

    UINT cc = 81;
    TryGetTagValue(section, L"<CC>", L"</CC>", cc);
    ccNumber = static_cast<int>(cc);
    TryGetTagStringValue(section, L"<MidiOut>", L"</MidiOut>", midiOut);
    TryGetTagStringValue(section, L"<MidiIn>", L"</MidiIn>", midiIn);
    return true;
}
