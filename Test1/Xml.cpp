#include "Xml.h"
#include "Midi.h"
#include "TrayIcon.h"
#include "StreamDeck.h"
#include "Hauptwerk.h"

#include <string>
#include <map>
#include <set>
#include <vector>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// Returns the path to the AhlbornBridge2 settings directory for the current user.
std::wstring GetSettingsDirPath()
{
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)))
    {
        std::wstring dir(buf);
        dir += L"\\AhlbornBridge2";
        return dir;
    }
    return std::wstring(L"C:\\Users\\Default\\AppData\\Roaming\\AhlbornBridge2");
}

namespace
{
    // Settings are stored per-user in the Roaming AppData folder.
    // Path: %APPDATA%\AhlbornBridge2\Settings.xml

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
    bool WriteSettingsXml(const std::wstring& inputDeviceName, const std::wstring& input2DeviceName, const std::wstring& outputDeviceName, const std::wstring& output2DeviceName, bool routerEnabled, bool closeSettingsOnDisconnect, bool showDebugConsole, bool checkForUpdateOnStart, const DeviceEnabledStates& devEnabled, bool bootstrapDefaults = false);
    std::wstring ReadHauptwerkStandbyOrgans();
    std::wstring ReadHauptwerkInstalledOrgans();
    void EnsureAudioSettingsLoaded();

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
    std::wstring s_cachedFixedHauptwerkOutput;
    std::wstring s_cachedOrganSwitchStates;
    // True once s_cachedOrganSwitchStates has been explicitly set (even to "").
    // Prevents WriteSettingsXml from reloading a stale on-disk value after
    // FlushOrganSwitchStatesToDisk has intentionally cleared the cache.
    bool s_organSwitchStatesCacheValid = false;
    bool s_cachedStreamDeckPipeServerEnabled = true;
    bool s_streamDeckSettingsLoaded = false;

    std::wstring s_cachedBidulePath;
    bool s_cachedBiduleCloseOnUnload = true;
    std::wstring s_cachedLastSeenAppVersion;

    std::wstring s_cachedHauptwerkIdlePriority   = L"HIGH";
    std::wstring s_cachedHauptwerkLoadedPriority = L"REALTIME";
    bool s_hauptwerkPriorityLoaded = false;

    // Cached audio output devices (all entries from AudioOutputDevice in Hauptwerk config,
    // persisted to <Audio><Devices> in Settings.xml).
    // Each entry: "<Device id=\"NNN\">name</Device>"
    std::wstring s_cachedAudioDevices;  // raw XML fragment
    std::wstring s_cachedAsioDevId;     // id of first ASIO: device (convenience)
    std::wstring s_cachedAsioDevName;   // name of first ASIO: device
    bool s_audioSettingsLoaded = false;

    const wchar_t* kDefaultAhlbornSwitchesXml =
        LR"(<o><nam>Principale 16'</nam><h>1</h><c>70</c><d>65</d><e>1</e></o>
<o><nam>Subbasso 16'</nam><h>1</h><c>70</c><d>66</d><e>2</e></o>
<o><nam>Ottave 8'</nam><h>1</h><c>70</c><d>67</d><e>3</e></o>
<o><nam>Bordone 8'</nam><h>1</h><c>70</c><d>68</d><e>4</e></o>
<o><nam>Decima Quinta 4'</nam><h>1</h><c>70</c><d>69</d><e>5</e></o>
<o><nam>Ripieno IV</nam><h>1</h><c>70</c><d>72</d><e>8</e></o>
<o><nam>Controfagotto 16'</nam><h>1</h><c>70</c><d>74</d><e>10</e></o>
<o><nam>Regale 4'</nam><h>1</h><c>70</c><d>76</d><e>12</e></o>
<o><nam>Unione I-Ped</nam><h>1</h><c>70</c><d>77</d><e>13</e></o>
<o><nam>Unione II-Ped</nam><h>1</h><c>70</c><d>78</d><e>14</e></o>
<o><nam>Quintadena 16'</nam><h>2</h><c>70</c><d>66</d><e>2</e></o>
<o><nam>Principale 8'</nam><h>2</h><c>70</c><d>67</d><e>3</e></o>
<o><nam>Flauto a camino 8'</nam><h>2</h><c>70</c><d>64</d><e>0</e></o>
<o><nam>Ottava 4'</nam><h>2</h><c>70</c><d>68</d><e>4</e></o>
<o><nam>Flauto a cuspide 4'</nam><h>2</h><c>70</c><d>65</d><e>1</e></o>
<o><nam>Decima quinta 2'</nam><h>2</h><c>70</c><d>69</d><e>5</e></o>
<o><nam>Ripieno 4 File</nam><h>2</h><c>70</c><d>73</d><e>9</e></o>
<o><nam>Voce umana 8'</nam><h>2</h><c>70</c><d>77</d><e>13</e></o>
<o><nam>Tromba 8'</nam><h>2</h><c>70</c><d>75</d><e>11</e></o>
<o><nam>Ripieno G.O.</nam><h>2</h><c>70</c><d>79</d><e>15</e></o>
<o><nam>Unione II-I'</nam><h>2</h><c>70</c><d>80</d><e>16</e></o>
<o><nam>Bordone 8'</nam><h>3</h><c>70</c><d>65</d><e>1</e></o>
<o><nam>Viola da gamba 8'</nam><h>3</h><c>70</c><d>74</d><e>10</e></o>
<o><nam>Principale 4'</nam><h>3</h><c>70</c><d>69</d><e>5</e></o>
<o><nam>Flauto a camino 4'</nam><h>3</h><c>70</c><d>66</d><e>2</e></o>
<o><nam>Nazardo 2 2/3'</nam><h>3</h><c>70</c><d>72</d><e>8</e></o>
<o><nam>Flautino 2'</nam><h>3</h><c>70</c><d>68</d><e>4</e></o>
<o><nam>Cimbalo 3 file</nam><h>3</h><c>70</c><d>76</d><e>12</e></o>
<o><nam>Sesquialtera 2 file</nam><h>3</h><c>70</c><d>84</d><e>20</e></o>
<o><nam>Cromorno 8'</nam><h>3</h><c>70</c><d>81</d><e>17</e></o>
<o><nam>Ripieno Rec.</nam><h>3</h><c>70</c><d>83</d><e>19</e></o>
<o><nam>Tremolo</nam><h>3</h><c>92</c><d>127</d><e>0</e></o>
<o><nam>Ance</nam><h>16</h><c>71</c><d>67</d><e>3</e></o>
<o><nam>Tutti</nam><h>16</h><c>71</c><d>66</d><e>2</e></o>
<o><nam>Pedale Auto</nam><h>16</h><c>71</c><d>65</d><e>1</e></o>
<o><nam>Fissatore</nam><h>16</h><c>71</c><d>70</d><e>6</e><m>1</m></o>
<o><nam>Annullatore</nam><h>16</h><c>71</c><d>0</d><e>0</e></o>)";

    // Cached assigned MIDI device name lists (new dynamic multi-device model).
    std::vector<std::wstring> s_assignedInputNames;
    std::vector<std::wstring> s_assignedOutputNames;
    bool s_assignedDevicesLoaded = false;

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
                if (!WriteSettingsXml(L"", L"", L"", L"", true, true, false, true, DeviceEnabledStates{}, true))
                    return false;
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

		// Trim leading/trailing spaces and line breaks to avoid preserving
		// blank lines inside sections such as <AhlbornSwitches>.
		size_t first = value.find_first_not_of(L" \t\r\n");
		if (first == std::wstring::npos)
		{
			value.clear();
		}
		else
		{
			size_t last = value.find_last_not_of(L" \t\r\n");
			value = value.substr(first, last - first + 1);
		}

		return true;
	}

	void EnsureHauptwerkPriorityLoaded()
	{
		if (s_hauptwerkPriorityLoaded) return;
		s_hauptwerkPriorityLoaded = true;
		std::wstring xml;
		if (!TryReadSettingsXml(xml)) return;
		std::wstring optionsSection;
		if (!TryGetSection(xml, L"Options", optionsSection)) return;
		std::wstring tmp;
		if (TryGetTagStringValue(optionsSection, L"<HauptwerkIdlePriority>", L"</HauptwerkIdlePriority>", tmp) && !tmp.empty())
			s_cachedHauptwerkIdlePriority = tmp;
		if (TryGetTagStringValue(optionsSection, L"<HauptwerkLoadedPriority>", L"</HauptwerkLoadedPriority>", tmp) && !tmp.empty())
			s_cachedHauptwerkLoadedPriority = tmp;
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

    std::wstring GetConfiguredHauptwerkUserDataRoot()
    {
        if (!s_rootHauptwerkUserData.empty())
            return s_rootHauptwerkUserData;

        std::wstring xml;
        std::wstring userDataRoot;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                    L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }

        return userDataRoot;
    }

    std::vector<std::wstring> ExtractHauptwerkEnabledPortNames(const std::wstring& hwXml, const std::wstring& objectType)
    {
        std::vector<std::wstring> names;
        const std::wstring kOpen = L"<ObjectList ObjectType=\"" + objectType + L"\">";
        size_t secStart = hwXml.find(kOpen);
        if (secStart == std::wstring::npos)
            return names;

        size_t contentStart = secStart + kOpen.size();
        size_t nextOL = hwXml.find(L"<ObjectList", contentStart);
        size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
        size_t contentEnd;
        if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
            contentEnd = closeOL;
        else if (nextOL != std::wstring::npos)
            contentEnd = nextOL;
        else
            contentEnd = hwXml.size();

        std::wstring section = hwXml.substr(contentStart, contentEnd - contentStart);
        size_t pos = 0;
        while (pos < section.size())
        {
            size_t oStart = section.find(L"<o>", pos);
            if (oStart == std::wstring::npos) break;
            size_t oEnd = section.find(L"</o>", oStart);
            if (oEnd == std::wstring::npos) break;

            std::wstring node = section.substr(oStart + 3, oEnd - oStart - 3);
            std::wstring name;
            TryGetTagStringValue(node, L"<nam>", L"</nam>", name);
            if (!name.empty())
                names.push_back(name);

            pos = oEnd + 4;
        }

        return names;
    }

    void ReadActualHauptwerkMidiPortsImpl(std::vector<std::wstring>& inputNames, std::vector<std::wstring>& outputNames)
    {
        inputNames.clear();
        outputNames.clear();

        std::wstring userDataRoot = GetConfiguredHauptwerkUserDataRoot();
        if (userDataRoot.empty())
            return;

        if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
            userDataRoot.pop_back();

        std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";
        std::wstring hwXml = ReadFileToWString(configPath);
        if (hwXml.empty())
            return;

        inputNames = ExtractHauptwerkEnabledPortNames(hwXml, L"EnabledMIDIInputPort");
        outputNames = ExtractHauptwerkEnabledPortNames(hwXml, L"EnabledMIDIOutputPort");
    }

    std::wstring BuildDeviceListSectionXml(const std::vector<std::wstring>& names)
    {
        std::wstring section;
        for (const auto& name : names)
            section += L"    <Device>" + name + L"</Device>\r\n";
        return section;
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
        EnsureAudioSettingsLoaded();

        auto findAudioDeviceIdByName = [](const std::wstring& fragment, const std::wstring& deviceName) -> std::wstring
        {
            size_t pos = 0;
            while (true)
            {
                size_t deviceStart = fragment.find(L"<Device", pos);
                if (deviceStart == std::wstring::npos)
                    break;

                size_t tagEnd = fragment.find(L">", deviceStart);
                if (tagEnd == std::wstring::npos)
                    break;

                size_t closeStart = fragment.find(L"</Device>", tagEnd);
                if (closeStart == std::wstring::npos)
                    break;

                std::wstring openTag = fragment.substr(deviceStart, tagEnd - deviceStart + 1);
                std::wstring value = fragment.substr(tagEnd + 1, closeStart - tagEnd - 1);
                if (value == deviceName)
                {
                    size_t idPos = openTag.find(L"id=\"");
                    if (idPos != std::wstring::npos)
                    {
                        idPos += 4;
                        size_t idEnd = openTag.find(L'"', idPos);
                        if (idEnd != std::wstring::npos)
                            return openTag.substr(idPos, idEnd - idPos);
                    }
                }

                pos = closeStart + 9;
            }

            return {};
        };

        const std::wstring hauptwerkVstLinkDeviceId = findAudioDeviceIdByName(s_cachedAudioDevices, L"Hauptwerk VST Link");

        std::map<std::wstring, std::wstring> existingOutputDeviceByUniqueId;
        std::map<std::wstring, std::wstring> existingBiduleProfileByUniqueId;
        {
            std::wstring existingSettingsXml;
            std::wstring installedOrgansSection;
            if (TryReadSettingsXml(existingSettingsXml) && TryGetSection(existingSettingsXml, L"InstalledOrgans", installedOrgansSection))
            {
                size_t pos = 0;
                while (true)
                {
                    size_t organStart = installedOrgansSection.find(L"<Organ", pos);
                    if (organStart == std::wstring::npos)
                        break;

                    size_t openEnd = installedOrgansSection.find(L'>', organStart);
                    size_t closeStart = installedOrgansSection.find(L"</Organ>", openEnd);
                    if (openEnd == std::wstring::npos || closeStart == std::wstring::npos)
                        break;

                    std::wstring organContent = installedOrgansSection.substr(openEnd + 1, closeStart - openEnd - 1);
                    std::wstring existingUniqueId;
                    std::wstring existingOutputDevice;
                    std::wstring existingBiduleProfile;
                    TryGetTagStringValue(organContent, L"<Identification_UniqueOrganID>", L"</Identification_UniqueOrganID>", existingUniqueId);
                    TryGetTagStringValue(organContent, L"<Output_Device>", L"</Output_Device>", existingOutputDevice);
                    TryGetTagStringValue(organContent, L"<BiduleProfile>", L"</BiduleProfile>", existingBiduleProfile);
                    if (!existingUniqueId.empty())
                    {
                        existingOutputDeviceByUniqueId[existingUniqueId] = existingOutputDevice;
                        existingBiduleProfileByUniqueId[existingUniqueId] = existingBiduleProfile;
                    }

                    pos = closeStart + 8;
                }
            }
        }

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

            printf("\nReadHauptwerkInstalledOrgans: analyzing %S\n", name.c_str());

				// Read the entire organ definition file once
				std::wstring displayName, uniqueOrganID, outputDevice;
				int numberChannels = 0;
				{
					std::wstring organFilePath = sampleSetsRoot + L"\\OrganDefinitions\\" + std::wstring(fd.cFileName);
					HANDLE fh = CreateFileW(organFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (fh != INVALID_HANDLE_VALUE)
					{
						DWORD fileSize = GetFileSize(fh, nullptr);
						if (fileSize != INVALID_FILE_SIZE && fileSize > 0)
						{
							std::string raw(fileSize, '\0');
							DWORD bytesRead = 0;
							if (ReadFile(fh, raw.data(), fileSize, &bytesRead, nullptr) && bytesRead > 0)
							{
								int wideSize = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
									static_cast<int>(bytesRead), nullptr, 0);
								if (wideSize > 0)
								{
									std::wstring hwXml(wideSize, L'\0');
									MultiByteToWideChar(CP_UTF8, 0, raw.data(),
										static_cast<int>(bytesRead), hwXml.data(), wideSize);

									TryGetTagStringValue(hwXml, L"<Identification_Name>", L"</Identification_Name>", displayName);
									TryGetTagStringValue(hwXml, L"<Identification_UniqueOrganID>", L"</Identification_UniqueOrganID>", uniqueOrganID);

                                    // Compute Number_Channels from Stop + Rank:
                                    // 1) read the first <b> of the first <o> inside ObjectList ObjectType="Stop"
                                    // 2) count Rank child nodes whose <b> is either exactly the same string
                                    //    or ends with " - <stopName>"
                                    // 3) channels = matchCount * 2
                                    auto extractObjectListSection = [&](const std::wstring& objectType, std::wstring& section) -> bool
                                    {
                                        const std::wstring openTag = L"<ObjectList ObjectType=\"" + objectType + L"\">";
                                        size_t start = hwXml.find(openTag);
                                        if (start == std::wstring::npos)
                                            return false;
                                        start += openTag.size();
                                        size_t end = hwXml.find(L"</ObjectList>", start);
                                        if (end == std::wstring::npos)
                                            return false;
                                        section = hwXml.substr(start, end - start);
                                        return true;
                                    };

                                    std::wstring stopSection;
                                    std::wstring rankSection;
                                    if (extractObjectListSection(L"Stop", stopSection) && extractObjectListSection(L"Rank", rankSection))
                                    {
                                        auto normalizeForRankMatch = [](const std::wstring& text) -> std::wstring
                                        {
                                            std::wstring noParens;
                                            bool inParens = false;
                                            for (wchar_t ch : text)
                                            {
                                                if (ch == L'(')
                                                {
                                                    inParens = true;
                                                    continue;
                                                }
                                                if (ch == L')')
                                                {
                                                    inParens = false;
                                                    continue;
                                                }
                                                if (!inParens)
                                                    noParens.push_back(ch);
                                            }

                                            std::wstring normalized;
                                            bool lastWasSpace = true;
                                            for (wchar_t ch : noParens)
                                            {
                                                wchar_t lower = static_cast<wchar_t>(towlower(ch));
                                                if ((lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9'))
                                                {
                                                    normalized.push_back(lower);
                                                    lastWasSpace = false;
                                                }
                                                else if (!lastWasSpace)
                                                {
                                                    normalized.push_back(L' ');
                                                    lastWasSpace = true;
                                                }
                                            }

                                            while (!normalized.empty() && normalized.back() == L' ')
                                                normalized.pop_back();
                                            return normalized;
                                        };

                                        std::wstring targetStopName;
                                        size_t firstO = stopSection.find(L"<o>");
                                        if (firstO != std::wstring::npos)
                                        {
                                            size_t firstOEnd = stopSection.find(L"</o>", firstO);
                                            if (firstOEnd != std::wstring::npos)
                                            {
                                                std::wstring firstStopNode = stopSection.substr(firstO + 3, firstOEnd - firstO - 3);
                                                TryGetTagStringValue(firstStopNode, L"<b>", L"</b>", targetStopName);
                                            }
                                        }

                                        if (!targetStopName.empty())
                                        {
                                            int matchCount = 0;
                                            std::wstring normalizedTargetStopName = normalizeForRankMatch(targetStopName);
                                            const std::wstring suffix = L" - " + targetStopName;
                                            size_t pos2 = 0;
                                            while (true)
                                            {
                                                size_t oPos = rankSection.find(L"<o>", pos2);
                                                if (oPos == std::wstring::npos)
                                                    break;
                                                size_t oEnd = rankSection.find(L"</o>", oPos);
                                                if (oEnd == std::wstring::npos)
                                                    break;

                                                std::wstring rankNode = rankSection.substr(oPos + 3, oEnd - oPos - 3);
                                                std::wstring rankName;
                                                if (TryGetTagStringValue(rankNode, L"<b>", L"</b>", rankName))
                                                {
                                                    std::wstring normalizedRankName = normalizeForRankMatch(rankName);
                                                    if (rankName == targetStopName ||
                                                        (rankName.size() > suffix.size() &&
                                                        rankName.compare(rankName.size() - suffix.size(), suffix.size(), suffix) == 0) ||
                                                        (!normalizedTargetStopName.empty() &&
                                                         normalizedRankName.find(normalizedTargetStopName) != std::wstring::npos))
                                                    {
                                                        ++matchCount;
                                                        printf("ReadHauptwerkInstalledOrgans: rank match '%S' for stop '%S'\n",
                                                            rankName.c_str(), targetStopName.c_str());
                                                    }
                                                }

                                                pos2 = oEnd + 4;
                                            }

                                            numberChannels = matchCount * 2;
                                            printf("ReadHauptwerkInstalledOrgans: stop '%S' -> %d rank match(es) -> %d channel(s)\n",
                                                targetStopName.c_str(), matchCount, numberChannels);
                                        }
                                    }
								}
							}
						}
						CloseHandle(fh);
					}
				}

                auto existingIt = existingOutputDeviceByUniqueId.find(uniqueOrganID);
                if (existingIt != existingOutputDeviceByUniqueId.end() && !existingIt->second.empty())
                {
                    outputDevice = existingIt->second;
                }
                else
                {
                    if (numberChannels == 2)
                        outputDevice = hauptwerkVstLinkDeviceId;
                    else if (numberChannels > 2)
                        outputDevice = s_cachedAsioDevId;
                }

				std::wstring idStr = (id < 10 ? L"0" : L"") + std::to_wstring(id);
				std::wstring displayAttr = displayName.empty() ? L"" : L" displayName=\"" + displayName + L"\"";
				std::wstring biduleProfile;
				auto biduleIt = existingBiduleProfileByUniqueId.find(uniqueOrganID);
				if (biduleIt != existingBiduleProfileByUniqueId.end())
					biduleProfile = biduleIt->second;
				result += L"    <Organ id=\"" + idStr + L"\"" + displayAttr + L">" + name + L"\r\n";
				result += L"       <o><Identification_UniqueOrganID>" + uniqueOrganID + L"</Identification_UniqueOrganID></o>\r\n";
				result += L"       <o><Number_Channels>" + (numberChannels > 0 ? std::to_wstring(numberChannels) : L"") + L"</Number_Channels></o>\r\n";
				result += L"       <o><Output_Device>" + outputDevice + L"</Output_Device></o>\r\n";
				result += L"       <o><BiduleProfile>" + biduleProfile + L"</BiduleProfile></o>\r\n";
				result += L"    </Organ>\r\n";
				printf("ReadHauptwerkInstalledOrgans: [%02d] %S (displayName: %S, channels: %d)\n", id, name.c_str(),
					displayName.empty() ? L"(none)" : displayName.c_str(), numberChannels);
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
		TryGetTagStringValue(section, L"<FixedHauptwerkOutput>", L"</FixedHauptwerkOutput>", s_cachedFixedHauptwerkOutput);
		{
			std::wstring pipeServerEnabledStr;
			if (TryGetTagStringValue(section, L"<PipeServerEnabled>", L"</PipeServerEnabled>", pipeServerEnabledStr))
				s_cachedStreamDeckPipeServerEnabled = (pipeServerEnabledStr != L"0");
		}
		std::wstring switchStatesSection;
		if (TryGetSection(section, L"OrganSwitchStates", switchStatesSection))
		{
			// Trim leading/trailing whitespace so we don't accumulate blank lines on each save.
			size_t first = switchStatesSection.find_first_not_of(L" \t\r\n");
			if (first == std::wstring::npos)
			{
				s_cachedOrganSwitchStates.clear();
				s_organSwitchStatesCacheValid = true;
			}
			else
			{
				size_t last = switchStatesSection.find_last_not_of(L" \t\r\n");
				s_cachedOrganSwitchStates = switchStatesSection.substr(first, last - first + 1) + L"\r\n";
				s_organSwitchStatesCacheValid = true;
			}
		}
	}

	void EnsureAudioSettingsLoaded()
	{
		if (s_audioSettingsLoaded) return;
		s_audioSettingsLoaded = true;
		std::wstring xml;
		if (!TryReadSettingsXml(xml)) return;
		std::wstring section;
		if (!TryGetSection(xml, L"Audio", section)) return;
		TryGetTagStringValue(section, L"<AsioDevId>",   L"</AsioDevId>",   s_cachedAsioDevId);
		TryGetTagStringValue(section, L"<AsioDevName>", L"</AsioDevName>", s_cachedAsioDevName);
		// Rebuild the cached devices fragment from <Devices>...</Devices>
		std::wstring devSection;
		if (TryGetSection(section, L"Devices", devSection))
		{
			// Strip leading/trailing whitespace so we don't preserve blank tail lines
			// inside <Devices> when rewriting Settings.xml.
			size_t first = devSection.find_first_not_of(L" \t\r\n");
			size_t last  = devSection.find_last_not_of(L" \t\r\n");
			if (first != std::wstring::npos)
				s_cachedAudioDevices = devSection.substr(first, last - first + 1) + L"\r\n";
			else
				s_cachedAudioDevices.clear();
		}
	}

	std::wstring NormalizeAhlbornSwitchesSection(const std::wstring& section)
	{
		std::wstring normalized;
		size_t pos = 0;
		while (true)
		{
			size_t oStart = section.find(L"<o>", pos);
			if (oStart == std::wstring::npos)
				break;
			size_t oEnd = section.find(L"</o>", oStart);
			if (oEnd == std::wstring::npos)
				break;

			if (!normalized.empty())
				normalized += L"\r\n";
			normalized += section.substr(oStart, oEnd - oStart + 4);
			pos = oEnd + 4;
		}
		return normalized;
	}

	// Parse a <AssignedMidi*s> section: each <Device>name</Device> child.
	std::vector<std::wstring> ParseDeviceListSection(const std::wstring& section)
	{
		std::vector<std::wstring> result;
		const std::wstring startTag = L"<Device>";
		const std::wstring endTag = L"</Device>";
		size_t pos = 0;
		while (true)
		{
			size_t s = section.find(startTag, pos);
			if (s == std::wstring::npos) break;
			s += startTag.size();
			size_t e = section.find(endTag, s);
			if (e == std::wstring::npos) break;
			std::wstring name = section.substr(s, e - s);
			if (!name.empty())
				result.push_back(name);
			pos = e + endTag.size();
		}
		return result;
	}

	void EnsureAssignedDevicesLoaded()
	{
		if (s_assignedDevicesLoaded) return;
		s_assignedDevicesLoaded = true;

		std::wstring xml;
		if (!TryReadSettingsXml(xml)) return;

		// Try new schema first: <AssignedMidiInputs> / <AssignedMidiOutputs>
		std::wstring section;
		if (TryGetSection(xml, L"AssignedMidiInputs", section))
		{
			s_assignedInputNames = ParseDeviceListSection(section);
		}
		if (TryGetSection(xml, L"AssignedMidiOutputs", section))
		{
			s_assignedOutputNames = ParseDeviceListSection(section);
		}

		// Fall back to old SettingsDevices schema for upgrade compatibility
		if (s_assignedInputNames.empty() && s_assignedOutputNames.empty())
		{
			std::wstring midiSection, devicesSection;
			if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
			{
				std::wstring name1, name2;
				TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", name1);
				TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", name2);
				if (!name1.empty()) s_assignedInputNames.push_back(name1);
				if (!name2.empty()) s_assignedInputNames.push_back(name2);

				std::wstring out1, out2;
				TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", out1);
				TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", out2);
				if (!out1.empty()) s_assignedOutputNames.push_back(out1);
				if (!out2.empty()) s_assignedOutputNames.push_back(out2);
			}
		}
	}

    bool WriteSettingsXml(const std::wstring& inputDeviceName, const std::wstring& input2DeviceName, const std::wstring& outputDeviceName, const std::wstring& output2DeviceName, bool routerEnabled, bool closeSettingsOnDisconnect, bool showDebugConsole, bool checkForUpdateOnStart, const DeviceEnabledStates& devEnabled, bool bootstrapDefaults)
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
		CreateDirectoryW((settingsDir + L"\\Icons").c_str(), nullptr);
		CreateDirectoryW((settingsDir + L"\\BiduleProfiles").c_str(), nullptr);

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

		std::wstring standbyOrgans;
		std::wstring installedOrgans;
		std::wstring ahlbornSwitches = kDefaultAhlbornSwitchesXml;

		if (!bootstrapDefaults)
		{
			// Ensure standby organ names are loaded (cached on first call).
			EnsureStandbyOrgansLoaded();
			standbyOrgans = s_cachedStandbyOrgans;

			// Ensure installed organs are loaded (cached on first call).
			EnsureInstalledOrgansLoaded();
			installedOrgans = s_cachedInstalledOrgans;

			// Ensure Stream Deck settings are loaded (cached on first call).
			EnsureStreamDeckSettingsLoaded();

			// Ensure audio settings are loaded (cached on first call).
			EnsureAudioSettingsLoaded();

			// Preserve data from existing Settings.xml.
			std::wstring existingXml;
			if (TryReadSettingsXml(existingXml))
			{
				std::wstring optionsSection;
					if (TryGetSection(existingXml, L"Options", optionsSection))
					{
						TryGetTagStringValue(optionsSection, L"<LastSeenAppVersion>", L"</LastSeenAppVersion>", s_cachedLastSeenAppVersion);
							// Priority is managed by EnsureHauptwerkPriorityLoaded / SaveHauptwerkPrioritySettings.
					}

				// Preserve the existing AhlbornSwitches table if it is already present in Settings.xml.
				std::wstring streamDeckSection;
				if (TryGetSection(existingXml, L"StreamDeck", streamDeckSection))
				{
					std::wstring switchesSection;
					if (TryGetSection(streamDeckSection, L"AhlbornSwitches", switchesSection) && !switchesSection.empty())
					{
						std::wstring normalizedSwitches = NormalizeAhlbornSwitchesSection(switchesSection);
						if (!normalizedSwitches.empty())
							ahlbornSwitches = normalizedSwitches;
					}
					std::wstring savedSwitchStates;
					if (TryGetSection(streamDeckSection, L"OrganSwitchStates", savedSwitchStates) && !s_organSwitchStatesCacheValid)
					{
						// Only load from file if the cache has never been explicitly set.
						// An empty cache after a flush is intentional (all stops OFF) and
						// must NOT be overwritten with the stale on-disk value.
						size_t first = savedSwitchStates.find_first_not_of(L" \t\r\n");
						if (first == std::wstring::npos)
							savedSwitchStates.clear();
						else
						{
							size_t last = savedSwitchStates.find_last_not_of(L" \t\r\n");
							savedSwitchStates = savedSwitchStates.substr(first, last - first + 1) + L"\r\n";
						}
						s_cachedOrganSwitchStates = savedSwitchStates;
						s_organSwitchStatesCacheValid = true;
					}
				}
			}
		}

        // Build the assigned device list sections and the actual Hauptwerk snapshot.
        std::wstring assignedInputSection = BuildDeviceListSectionXml(s_assignedInputNames);
        std::wstring assignedOutputSection = BuildDeviceListSectionXml(s_assignedOutputNames);
        std::vector<std::wstring> actualHauptwerkInputs;
        std::vector<std::wstring> actualHauptwerkOutputs;
        if (!bootstrapDefaults)
            ReadActualHauptwerkMidiPorts(actualHauptwerkInputs, actualHauptwerkOutputs);
        std::wstring actualHauptwerkInputSection = BuildDeviceListSectionXml(actualHauptwerkInputs);
        std::wstring actualHauptwerkOutputSection = BuildDeviceListSectionXml(actualHauptwerkOutputs);

		std::wstring xml = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
			L"<Settings>\r\n"
			L"  <Midi>\r\n"
			L"    <MidiOptions>\r\n"
			L"      <MidiRouterEnabled>" + std::to_wstring(routerEnabled ? 1 : 0) + L"</MidiRouterEnabled>\r\n"
			L"    </MidiOptions>\r\n"
			L"    <CurrentMidiInputDevices>\r\n" + inputDevices +
			L"    </CurrentMidiInputDevices>\r\n"
			L"    <CurrentMidiOutputDevices>\r\n" + outputDevices +
			L"    </CurrentMidiOutputDevices>\r\n"
			L"    <AssignedMidiInputs>\r\n" + assignedInputSection +
			L"    </AssignedMidiInputs>\r\n"
			L"    <HauptwerkMidiInputsActual>\r\n" + actualHauptwerkInputSection +
			L"    </HauptwerkMidiInputsActual>\r\n"
			L"    <HauptwerkMidiOutputsActual>\r\n" + actualHauptwerkOutputSection +
			L"    </HauptwerkMidiOutputsActual>\r\n"
			L"  </Midi>\r\n"
			L"  <Options>\r\n"
			L"    <CloseSettingsOnDisconnect>" + std::to_wstring(closeSettingsOnDisconnect ? 1 : 0) + L"</CloseSettingsOnDisconnect>\r\n"
			L"    <ShowDebugConsole>" + std::to_wstring(showDebugConsole ? 1 : 0) + L"</ShowDebugConsole>\r\n"
			L"    <CheckForUpdateOnStart>" + std::to_wstring(checkForUpdateOnStart ? 1 : 0) + L"</CheckForUpdateOnStart>\r\n"
			L"    <LastSeenAppVersion>" + s_cachedLastSeenAppVersion + L"</LastSeenAppVersion>\r\n"
			L"    <HauptwerkIdlePriority>" + s_cachedHauptwerkIdlePriority + L"</HauptwerkIdlePriority>\r\n"
			L"    <HauptwerkLoadedPriority>" + s_cachedHauptwerkLoadedPriority + L"</HauptwerkLoadedPriority>\r\n"
			L"    <BidulePath>" + s_cachedBidulePath + L"</BidulePath>\r\n"
            L"    <BiduleCloseOnUnload>" + std::to_wstring(s_cachedBiduleCloseOnUnload ? 1 : 0) + L"</BiduleCloseOnUnload>\r\n"
			L"    <ActiveSensingEnabled>" + std::to_wstring(g_activeSensingEnabled.load() ? 1 : 0) + L"</ActiveSensingEnabled>\r\n"
			L"    <ActiveSensingOutput>" + g_activeSensingOutputName + L"</ActiveSensingOutput>\r\n"
			L"    <RootFolder_HauptwerkApplication>" + s_rootHauptwerkApp + L"</RootFolder_HauptwerkApplication>\r\n"
			L"    <RootFolder_HauptwerkUserData>" + s_rootHauptwerkUserData + L"</RootFolder_HauptwerkUserData>\r\n"
			L"    <RootFolder_HauptwerkSampleSetsAndComponents>" + s_rootHauptwerkSampleSets + L"</RootFolder_HauptwerkSampleSetsAndComponents>\r\n"
			L"    <RootFolder_HauptwerkInternalWorkingFiles>" + s_rootHauptwerkWorkingFiles + L"</RootFolder_HauptwerkInternalWorkingFiles>\r\n"
			L"    <AppDataPath>" + GetSettingsDirPath() + L"</AppDataPath>\r\n"
			L"  </Options>\r\n"
			L"  <StandbyeOrgans>\r\n" + standbyOrgans +
			L"  </StandbyeOrgans>\r\n"
			L"  <InstalledOrgans>\r\n" + installedOrgans +
			L"  </InstalledOrgans>\r\n"
			L"  <StreamDeck>\r\n"
				L"    <CC>" + s_cachedStreamDeckCC + L"</CC>\r\n"
				L"    <MidiOut>" + s_cachedStreamDeckMidiOut + L"</MidiOut>\r\n"
				L"    <MidiIn>" + s_cachedStreamDeckMidiIn + L"</MidiIn>\r\n"
				L"    <PipeServerEnabled>" + std::to_wstring(s_cachedStreamDeckPipeServerEnabled ? 1 : 0) + L"</PipeServerEnabled>\r\n"
			L"    <FixedHauptwerkOutput>" + s_cachedFixedHauptwerkOutput + L"</FixedHauptwerkOutput>\r\n"
			L"    <AhlbornSwitches>\r\n"
				+ ahlbornSwitches + L"\r\n"
			L"    </AhlbornSwitches>\r\n"
			L"    <OrganSwitchStates>\r\n" + s_cachedOrganSwitchStates +
			L"    </OrganSwitchStates>\r\n"
			L"  </StreamDeck>\r\n"
			L"  <Audio>\r\n"
			L"    <AsioDevId>" + s_cachedAsioDevId + L"</AsioDevId>\r\n"
			L"    <AsioDevName>" + s_cachedAsioDevName + L"</AsioDevName>\r\n"
			L"    <Devices>\r\n" + s_cachedAudioDevices +
			L"    </Devices>\r\n"
			L"  </Audio>\r\n"
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

// ---------- Public: dynamic multi-device assignment API ----------

std::vector<std::wstring> LoadAssignedMidiInputNames()
{
	EnsureAssignedDevicesLoaded();
	return s_assignedInputNames;
}

std::vector<std::wstring> LoadAssignedMidiOutputNames()
{
	EnsureAssignedDevicesLoaded();
	return s_assignedOutputNames;
}

bool SaveAssignedMidiInputNames(const std::vector<std::wstring>& names)
{
	EnsureAssignedDevicesLoaded();
	s_assignedInputNames = names;
	// Persist: re-read other settings and write the full XML.
	bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
	bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
	bool showConsole = true; LoadShowDebugConsole(showConsole);
	bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
	// Build legacy compat names from slot 0/1.
	std::wstring in1 = names.size() > 0 ? names[0] : L"";
	std::wstring in2 = names.size() > 1 ? names[1] : L"";
	std::wstring out1, out2;
	if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
	if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];
	DeviceEnabledStates devEnabled;
	return WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
}

bool SaveFixedHauptwerkOutputName(const std::wstring& name)
{
    EnsureStreamDeckSettingsLoaded();
    s_cachedFixedHauptwerkOutput = name;

    bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
    bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
    bool showConsole = true; LoadShowDebugConsole(showConsole);
    bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
    std::wstring in1, in2;
    if (!s_assignedInputNames.empty()) in1 = s_assignedInputNames[0];
    if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
    std::wstring out1, out2;
    if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
    if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];
    DeviceEnabledStates devEnabled;
    return WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
}

std::wstring LoadFixedHauptwerkOutputName()
{
    EnsureStreamDeckSettingsLoaded();
    return s_cachedFixedHauptwerkOutput;
}

std::vector<InstalledOrganInfo> LoadInstalledOrganInfos()
{
    EnsureInstalledOrgansLoaded();

    std::vector<InstalledOrganInfo> result;
    const std::wstring& xml = s_cachedInstalledOrgans;
    size_t pos = 0;
    while (true)
    {
        size_t organStart = xml.find(L"<Organ", pos);
        if (organStart == std::wstring::npos)
            break;

        size_t openEnd = xml.find(L'>', organStart);
        size_t closeStart = xml.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || closeStart == std::wstring::npos)
            break;

        std::wstring openTag = xml.substr(organStart, openEnd - organStart + 1);
        std::wstring organContent = xml.substr(openEnd + 1, closeStart - openEnd - 1);

        InstalledOrganInfo info;
        size_t idPos = openTag.find(L"id=\"");
        if (idPos != std::wstring::npos)
        {
            idPos += 4;
            size_t idEnd = openTag.find(L'"', idPos);
            if (idEnd != std::wstring::npos)
                info.id = openTag.substr(idPos, idEnd - idPos);
        }

        size_t displayPos = openTag.find(L"displayName=\"");
        if (displayPos != std::wstring::npos)
        {
            displayPos += 13;
            size_t displayEnd = openTag.find(L'"', displayPos);
            if (displayEnd != std::wstring::npos)
                info.displayName = openTag.substr(displayPos, displayEnd - displayPos);
        }

        size_t firstChild = organContent.find(L"<o>");
        std::wstring organNameBlock = firstChild == std::wstring::npos ? organContent : organContent.substr(0, firstChild);
        size_t nameFirst = organNameBlock.find_first_not_of(L" \t\r\n");
        size_t nameLast = organNameBlock.find_last_not_of(L" \t\r\n");
        if (nameFirst != std::wstring::npos)
            info.name = organNameBlock.substr(nameFirst, nameLast - nameFirst + 1);

        TryGetTagStringValue(organContent, L"<Identification_UniqueOrganID>", L"</Identification_UniqueOrganID>", info.uniqueOrganId);
        TryGetTagStringValue(organContent, L"<Output_Device>", L"</Output_Device>", info.outputDeviceId);
        TryGetTagStringValue(organContent, L"<BiduleProfile>", L"</BiduleProfile>", info.biduleProfile);
        UINT channels = 0;
        if (TryGetTagValue(organContent, L"<Number_Channels>", L"</Number_Channels>", channels))
            info.numberChannels = static_cast<int>(channels);

        result.push_back(info);
        pos = closeStart + 8;
    }

    return result;
}

std::vector<AudioDeviceInfo> LoadAudioOutputDevices()
{
    EnsureAudioSettingsLoaded();

    std::vector<AudioDeviceInfo> result;
    const std::wstring& xml = s_cachedAudioDevices;
    size_t pos = 0;
    while (true)
    {
        size_t deviceStart = xml.find(L"<Device", pos);
        if (deviceStart == std::wstring::npos)
            break;

        size_t tagEnd = xml.find(L'>', deviceStart);
        size_t closeStart = xml.find(L"</Device>", tagEnd);
        if (tagEnd == std::wstring::npos || closeStart == std::wstring::npos)
            break;

        AudioDeviceInfo info;
        std::wstring openTag = xml.substr(deviceStart, tagEnd - deviceStart + 1);
        size_t idPos = openTag.find(L"id=\"");
        if (idPos != std::wstring::npos)
        {
            idPos += 4;
            size_t idEnd = openTag.find(L'"', idPos);
            if (idEnd != std::wstring::npos)
                info.id = openTag.substr(idPos, idEnd - idPos);
        }

        info.name = xml.substr(tagEnd + 1, closeStart - tagEnd - 1);
        result.push_back(info);
        pos = closeStart + 9;
    }

    return result;
}

bool SaveInstalledOrganOutputDevice(const std::wstring& uniqueOrganId, const std::wstring& outputDeviceId)
{
    if (uniqueOrganId.empty())
        return false;

    EnsureInstalledOrgansLoaded();
    std::wstring xml = s_cachedInstalledOrgans;
    size_t pos = 0;
    while (true)
    {
        size_t organStart = xml.find(L"<Organ", pos);
        if (organStart == std::wstring::npos)
            break;

        size_t openEnd = xml.find(L'>', organStart);
        size_t closeStart = xml.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || closeStart == std::wstring::npos)
            break;

        std::wstring organContent = xml.substr(openEnd + 1, closeStart - openEnd - 1);
        std::wstring currentUniqueId;
        TryGetTagStringValue(organContent, L"<Identification_UniqueOrganID>", L"</Identification_UniqueOrganID>", currentUniqueId);
        if (currentUniqueId == uniqueOrganId)
        {
            const std::wstring startTag = L"<Output_Device>";
            const std::wstring endTag = L"</Output_Device>";
            size_t valueStart = organContent.find(startTag);
            if (valueStart == std::wstring::npos)
                return false;
            valueStart += startTag.size();
            size_t valueEnd = organContent.find(endTag, valueStart);
            if (valueEnd == std::wstring::npos)
                return false;

            organContent = organContent.substr(0, valueStart) + outputDeviceId + organContent.substr(valueEnd);
            xml = xml.substr(0, openEnd + 1) + organContent + xml.substr(closeStart);
            s_cachedInstalledOrgans = xml;

            bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
            bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
            bool showConsole = true; LoadShowDebugConsole(showConsole);
            bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
            std::wstring in1, in2;
            if (!s_assignedInputNames.empty()) in1 = s_assignedInputNames[0];
            if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
            std::wstring out1, out2;
            if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
            if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];
            DeviceEnabledStates devEnabled;
            return WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
        }

        pos = closeStart + 8;
    }

    return false;
}

bool SaveInstalledOrganBiduleProfile(const std::wstring& uniqueOrganId, const std::wstring& biduleProfile)
{
    if (uniqueOrganId.empty())
        return false;

    // Always store only the filename (relative to BiduleProfiles folder),
    // never an absolute path, so the setting works on any user account or PC.
    std::wstring profileToSave = biduleProfile;
    size_t sep = profileToSave.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
        profileToSave = profileToSave.substr(sep + 1);

    EnsureInstalledOrgansLoaded();
    std::wstring xml = s_cachedInstalledOrgans;
    size_t pos = 0;
    while (true)
    {
        size_t organStart = xml.find(L"<Organ", pos);
        if (organStart == std::wstring::npos)
            break;

        size_t openEnd = xml.find(L'>', organStart);
        size_t closeStart = xml.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || closeStart == std::wstring::npos)
            break;

        std::wstring organContent = xml.substr(openEnd + 1, closeStart - openEnd - 1);
        std::wstring currentUniqueId;
        TryGetTagStringValue(organContent, L"<Identification_UniqueOrganID>", L"</Identification_UniqueOrganID>", currentUniqueId);
        if (currentUniqueId == uniqueOrganId)
        {
            const std::wstring startTag = L"<BiduleProfile>";
            const std::wstring endTag = L"</BiduleProfile>";
            size_t valueStart = organContent.find(startTag);
            if (valueStart == std::wstring::npos)
                return false;
            valueStart += startTag.size();
            size_t valueEnd = organContent.find(endTag, valueStart);
            if (valueEnd == std::wstring::npos)
                return false;

            organContent = organContent.substr(0, valueStart) + biduleProfile + organContent.substr(valueEnd);
            xml = xml.substr(0, openEnd + 1) + organContent + xml.substr(closeStart);
            s_cachedInstalledOrgans = xml;

            std::wstring targetDir = GetSettingsDirPath() + L"\\BiduleProfiles";
            CreateDirectoryW(targetDir.c_str(), nullptr);

            bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
            bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
            bool showConsole = true; LoadShowDebugConsole(showConsole);
            bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
            std::wstring in1, in2;
            if (!s_assignedInputNames.empty()) in1 = s_assignedInputNames[0];
            if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
            std::wstring out1, out2;
            if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
            if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];
            DeviceEnabledStates devEnabled;
            return WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
        }

        pos = closeStart + 8;
    }

    return false;
}

bool IsHauptwerkAudioOutputDeviceIdAligned(const std::wstring& outputDeviceId)
{
    if (outputDeviceId.empty())
        return false;

    std::wstring userDataRoot = s_rootHauptwerkUserData;
    if (userDataRoot.empty())
    {
        std::wstring xml;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                    L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }
    }
    if (userDataRoot.empty())
        return false;
    if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
        userDataRoot.pop_back();

    std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";
    std::wstring hwXml = ReadFileToWString(configPath);
    if (hwXml.empty())
        return false;

    const std::wstring kOpen = L"<ObjectList ObjectType=\"AudioOutputUnit\">";
    size_t secStart = hwXml.find(kOpen);
    if (secStart == std::wstring::npos)
        return false;

    size_t contentStart = secStart + kOpen.size();
    size_t nextOL = hwXml.find(L"<ObjectList", contentStart);
    size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
    size_t contentEnd;
    if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
        contentEnd = closeOL;
    else if (nextOL != std::wstring::npos)
        contentEnd = nextOL;
    else
        contentEnd = hwXml.size();

    std::wstring content = hwXml.substr(contentStart, contentEnd - contentStart);
    size_t firstDevStart = content.find(L"<dev>");
    if (firstDevStart == std::wstring::npos)
        return false;
    firstDevStart += 5;
    size_t firstDevEnd = content.find(L"</dev>", firstDevStart);
    if (firstDevEnd == std::wstring::npos)
        return false;

    std::wstring currentFirstDev = content.substr(firstDevStart, firstDevEnd - firstDevStart);
    printf("[AudioPreload] Alignment check: desired=%S currentFirstDev=%S\n", outputDeviceId.c_str(), currentFirstDev.c_str());
    return currentFirstDev == outputDeviceId;
}

bool EnsureHauptwerkAudioOutputDeviceId(const std::wstring& outputDeviceId, bool* changed)
{
    if (outputDeviceId.empty())
    {
        printf("[AudioPreload] Requested outputDeviceId is empty.\n");
        return false;
    }

    if (changed)
        *changed = false;

    std::wstring userDataRoot = s_rootHauptwerkUserData;
    if (userDataRoot.empty())
    {
        std::wstring xml;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                    L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }
    }
    if (userDataRoot.empty())
    {
        printf("[AudioPreload] RootFolder_HauptwerkUserData is empty.\n");
        return false;
    }
    if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
        userDataRoot.pop_back();

    std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";
    printf("[AudioPreload] Target config: %S\n", configPath.c_str());
    printf("[AudioPreload] Desired dev: %S\n", outputDeviceId.c_str());

    std::wstring hwXml = ReadFileToWString(configPath);
    if (hwXml.empty())
    {
        printf("[AudioPreload] Config read failed or empty.\n");
        return false;
    }

    const std::wstring kOpen = L"<ObjectList ObjectType=\"AudioOutputUnit\">";
    size_t secStart = hwXml.find(kOpen);
    if (secStart == std::wstring::npos)
    {
        printf("[AudioPreload] AudioOutputUnit section not found.\n");
        return false;
    }

    size_t contentStart = secStart + kOpen.size();
    size_t nextOL = hwXml.find(L"<ObjectList", contentStart);
    size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
    size_t contentEnd;
    if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
        contentEnd = closeOL;
    else if (nextOL != std::wstring::npos)
        contentEnd = nextOL;
    else
        contentEnd = hwXml.size();

    std::wstring content = hwXml.substr(contentStart, contentEnd - contentStart);
    size_t firstDevStart = content.find(L"<dev>");
    if (firstDevStart == std::wstring::npos)
    {
        printf("[AudioPreload] No <dev> tag found in AudioOutputUnit.\n");
        return false;
    }
    firstDevStart += 5;
    size_t firstDevEnd = content.find(L"</dev>", firstDevStart);
    if (firstDevEnd == std::wstring::npos)
    {
        printf("[AudioPreload] Malformed first <dev> tag in AudioOutputUnit.\n");
        return false;
    }

    std::wstring currentFirstDev = content.substr(firstDevStart, firstDevEnd - firstDevStart);
    printf("[AudioPreload] Current first dev: %S\n", currentFirstDev.c_str());

    if (currentFirstDev == outputDeviceId)
    {
        printf("[AudioPreload] First <dev> already matches desired value. No rewrite needed.\n");
        return true;
    }

    size_t pos = 0;
    int replacedCount = 0;
    while (true)
    {
        size_t devStart = content.find(L"<dev>", pos);
        if (devStart == std::wstring::npos)
            break;
        devStart += 5;
        size_t devEnd = content.find(L"</dev>", devStart);
        if (devEnd == std::wstring::npos)
            break;
        content = content.substr(0, devStart) + outputDeviceId + content.substr(devEnd);
        devEnd = devStart + outputDeviceId.size();
        pos = devEnd + 6;
        ++replacedCount;
    }

    printf("[AudioPreload] First <dev> differs. Rewriting %d <dev> node(s) to %S\n",
        replacedCount, outputDeviceId.c_str());

    hwXml = hwXml.substr(0, contentStart) + content + hwXml.substr(contentEnd);

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
        return false;
    std::string utf8(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, utf8.data(), utf8Size, nullptr, nullptr);

    HANDLE fh = CreateFileW(configPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE)
    {
        printf("[AudioPreload] Cannot open config for write (error %lu).\n", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(fh, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    if (ok != FALSE)
        FlushFileBuffers(fh);
    CloseHandle(fh);
    if (ok != FALSE && written == utf8.size() && changed)
        *changed = true;
    printf("[AudioPreload] Write result: ok=%d bytes=%lu expected=%zu changed=%d\n",
        ok != FALSE ? 1 : 0, written, utf8.size(), changed && *changed ? 1 : 0);
    return ok != FALSE && written == utf8.size();
}

bool SaveAssignedMidiOutputNames(const std::vector<std::wstring>& names)
{
	EnsureAssignedDevicesLoaded();
	s_assignedOutputNames = names;
	bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
	bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
	bool showConsole = true; LoadShowDebugConsole(showConsole);
	bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
	std::wstring in1, in2;
	if (!s_assignedInputNames.empty()) in1 = s_assignedInputNames[0];
	if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
	std::wstring out1 = names.size() > 0 ? names[0] : L"";
	std::wstring out2 = names.size() > 1 ? names[1] : L"";
	DeviceEnabledStates devEnabled;
	return WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
}

void ReadActualHauptwerkMidiPorts(std::vector<std::wstring>& inputNames, std::vector<std::wstring>& outputNames)
{
	ReadActualHauptwerkMidiPortsImpl(inputNames, outputNames);
}

bool WriteHauptwerkMidiConfig(const std::vector<std::wstring>& inputNames,
							  const std::vector<std::wstring>& outputNames)
{
    // Build path to Config.Config_Hauptwerk_xml
    std::wstring userDataRoot = s_rootHauptwerkUserData;
    if (userDataRoot.empty())
    {
        std::wstring xml;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                                     L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }
    }
    if (userDataRoot.empty())
    {
        printf("[WriteHauptwerkMidiConfig] RootFolder_HauptwerkUserData not configured.\n");
        return false;
    }
    if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
        userDataRoot.pop_back();

    std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";

    // Read existing config
    std::wstring hwXml = ReadFileToWString(configPath);
    if (hwXml.empty())
    {
        printf("[WriteHauptwerkMidiConfig] Cannot read config: %S\n", configPath.c_str());
        return false;
    }

    // --- Parse PreviouslySeenDevice to build id maps ---
    // Each device entry looks like: <o><typ>2</typ><id>1234</id><nam>Device Name</nam></o>
    // typ=2 → MIDI input, typ=3 → MIDI output
    // (Audio/other devices may omit <typ>, meaning a different type; we ignore those.)

    struct SeenDevice { std::wstring id; std::wstring name; };
    std::vector<SeenDevice> seenInputs, seenOutputs;

    {
        // Find PreviouslySeenDevice section boundaries
        const std::wstring kPSD = L"<ObjectList ObjectType=\"PreviouslySeenDevice\">";
        size_t psdPos = hwXml.find(kPSD);
        if (psdPos != std::wstring::npos)
        {
            // Section ends at the next <ObjectList or end of file
            size_t psdEnd = hwXml.find(L"<ObjectList", psdPos + kPSD.size());
            if (psdEnd == std::wstring::npos) psdEnd = hwXml.size();
            std::wstring psdSection = hwXml.substr(psdPos + kPSD.size(), psdEnd - psdPos - kPSD.size());

            // Walk all <o>...</o> nodes
            size_t pos = 0;
            while (pos < psdSection.size())
            {
                size_t oStart = psdSection.find(L"<o>", pos);
                if (oStart == std::wstring::npos) break;
                size_t oEnd = psdSection.find(L"</o>", oStart);
                if (oEnd == std::wstring::npos) break;
                std::wstring node = psdSection.substr(oStart + 3, oEnd - oStart - 3);
                pos = oEnd + 4;

                std::wstring typ, id, nam;
                TryGetTagStringValue(node, L"<typ>", L"</typ>", typ);
                TryGetTagStringValue(node, L"<id>",  L"</id>",  id);
                TryGetTagStringValue(node, L"<nam>", L"</nam>", nam);
                if (id.empty() || nam.empty()) continue;

                if (typ == L"2")
                    seenInputs.push_back({ id, nam });
                else if (typ == L"3")
                    seenOutputs.push_back({ id, nam });
            }
        }
    }
    printf("[WriteHauptwerkMidiConfig] PreviouslySeenDevice: %zu inputs, %zu outputs found.\n",
           seenInputs.size(), seenOutputs.size());

    // --- Inject virtual ports into PreviouslySeenDevice if not already present ---
    // Hauptwerk uses this list to decide if a device is "new" and needs a user prompt.
    // If our virtual ports are missing, inject them so Hauptwerk silently accepts them.
    {
        // Generate a stable numeric ID from a device name using djb2 hash, clamped to 5 digits.
        auto makeStableId = [](const std::wstring& name) -> std::wstring
        {
            unsigned long hash = 5381;
            for (wchar_t c : name)
                hash = ((hash << 5) + hash) + (unsigned long)c;
            // Keep in range 10000-99999 for a plausible 5-digit Hauptwerk ID.
            unsigned long id = 10000 + (hash % 90000);
            return std::to_wstring(id);
        };

        // Virtual port names: both pairs must be injected so Hauptwerk does not
        // prompt the user about "new devices" on first startup.
        // Pair 1: AhlbornBridge Virtual Port / (B)  — bridge ↔ Hauptwerk input
        // Pair 2: Hauptwerk Virtual (A) / (B)        — Hauptwerk output monitor
        struct VirtualPort { std::wstring name; std::wstring typ; std::vector<SeenDevice>* list; };
        std::vector<VirtualPort> toCheck = {
            { L"AhlbornBridge Virtual Port",     L"2", &seenInputs  },
            { L"AhlbornBridge Virtual Port",     L"3", &seenOutputs },
            { L"AhlbornBridge Virtual Port (B)", L"2", &seenInputs  },
            { L"AhlbornBridge Virtual Port (B)", L"3", &seenOutputs },
            { L"Hauptwerk Virtual (A)",           L"2", &seenInputs  },
            { L"Hauptwerk Virtual (A)",           L"3", &seenOutputs },
            { L"Hauptwerk Virtual (B)",           L"2", &seenInputs  },
            { L"Hauptwerk Virtual (B)",           L"3", &seenOutputs },
        };

        const std::wstring kPSD = L"<ObjectList ObjectType=\"PreviouslySeenDevice\">";
        size_t psdPos = hwXml.find(kPSD);

        for (auto& vp : toCheck)
        {
            bool found = false;
            for (const auto& dev : *vp.list)
                if (dev.name == vp.name) { found = true; break; }

            if (!found)
            {
                std::wstring id = makeStableId(vp.name + vp.typ);
                std::wstring newNode = L"\r\n<o><typ>" + vp.typ + L"</typ><id>" + id + L"</id><nam>" + vp.name + L"</nam></o>";
                vp.list->push_back({ id, vp.name });

                // Inject into the PreviouslySeenDevice section in the XML.
                if (psdPos != std::wstring::npos)
                {
                    size_t insertAt = psdPos + kPSD.size();
                    hwXml.insert(insertAt, newNode);
                    // Recalculate psdPos since we modified the string.
                    psdPos = hwXml.find(kPSD);
                }
                printf("[WriteHauptwerkMidiConfig] Injected virtual port into PreviouslySeenDevice: '%S' (typ=%S id=%S)\n",
                       vp.name.c_str(), vp.typ.c_str(), id.c_str());
            }
        }
    }

    auto extractSectionPortNames = [&](const std::wstring& objectType) -> std::vector<std::wstring>
    {
        std::vector<std::wstring> names;
        const std::wstring kOpen = L"<ObjectList ObjectType=\"" + objectType + L"\">";
        size_t secStart = hwXml.find(kOpen);
        if (secStart == std::wstring::npos)
            return names;

        size_t contentStart = secStart + kOpen.size();
        size_t nextOL = hwXml.find(L"<ObjectList", contentStart);
        size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
        size_t contentEnd;
        if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
            contentEnd = closeOL;
        else if (nextOL != std::wstring::npos)
            contentEnd = nextOL;
        else
            contentEnd = hwXml.size();

        std::wstring section = hwXml.substr(contentStart, contentEnd - contentStart);
        size_t pos = 0;
        while (pos < section.size())
        {
            size_t oStart = section.find(L"<o>", pos);
            if (oStart == std::wstring::npos) break;
            size_t oEnd = section.find(L"</o>", oStart);
            if (oEnd == std::wstring::npos) break;

            std::wstring node = section.substr(oStart + 3, oEnd - oStart - 3);
            std::wstring name;
            TryGetTagStringValue(node, L"<nam>", L"</nam>", name);
            if (!name.empty())
                names.push_back(name);

            pos = oEnd + 4;
        }

        return names;
    };

    // --- Build new <o> node lists ---
    // EnabledMIDIInputPort  → always "AhlbornBridge Virtual Port (B)" (typ=2 in PreviouslySeenDevice)
    // EnabledMIDIOutputPort → preserve the original physical output from first install,
    //                         then append any newly assigned output devices (typ=3 in PreviouslySeenDevice)
    auto buildNode = [](const std::wstring& name, const std::vector<SeenDevice>& seen) -> std::wstring
    {
        std::wstring id;
        for (const auto& dev : seen)
        {
            if (dev.name == name)
            {
                id = dev.id;
                printf("[WriteHauptwerkMidiConfig] Matched: '%S' id=%S\n",
                       dev.name.c_str(), dev.id.c_str());
                break;
            }
        }
        if (id.empty())
            printf("[WriteHauptwerkMidiConfig] Not in PreviouslySeenDevice, writing with empty id: '%S'\n",
                   name.c_str());
        return L"\r\n<o><id>" + id + L"</id><nam>" + name + L"</nam></o>";
    };

    // Fixed input port for Hauptwerk: AhlbornBridge Virtual Port (B)
    const std::wstring kHWInput = L"AhlbornBridge Virtual Port (B)";
    std::wstring newInputNodes = buildNode(kHWInput, seenInputs);

    // Fixed output port for Hauptwerk: "Hauptwerk Virtual (A)" (A-side of the new loopback).
    // The bridge monitors the B-side ("Hauptwerk Virtual (B)") and mirrors messages
    // to the physical console output.  Any AhlbornBridge virtual ports are excluded.
    const std::wstring kHWOutput = L"Hauptwerk Virtual (A)";

    auto currentEnabledOutputs = extractSectionPortNames(L"EnabledMIDIOutputPort");
    std::vector<std::wstring> mergedOutputs;

    auto appendUniqueOutput = [&](const std::wstring& name)
    {
        if (name.empty())
            return;
        // Exclude the bridge's own virtual ports from Hauptwerk's output list.
        if (name.find(L"AhlbornBridge") != std::wstring::npos)
            return;
        // Exclude the physical console device — Hauptwerk must NOT output there
        // directly; the bridge mirrors Hauptwerk Virtual (B) to it instead.
        if (!s_cachedFixedHauptwerkOutput.empty() && name == s_cachedFixedHauptwerkOutput)
            return;
        // Exclude the Hauptwerk Virtual ports themselves from the appended user outputs.
        if (name == L"Hauptwerk Virtual (A)" || name == L"Hauptwerk Virtual (B)")
            return;
        if (std::find(mergedOutputs.begin(), mergedOutputs.end(), name) != mergedOutputs.end())
            return;
        mergedOutputs.push_back(name);
    };

    // Always include the Hauptwerk Virtual (A) port as the primary output.
    // Push directly — appendUniqueOutput filters out Hauptwerk Virtual names
    // to prevent duplicates from user-assigned lists, so we bypass it here.
    mergedOutputs.push_back(kHWOutput);
    printf("[WriteHauptwerkMidiConfig] Hauptwerk output set to virtual port: '%S'\n",
           kHWOutput.c_str());

    // Also preserve any additional user-assigned outputs (from the UI config),
    // but do NOT add back the old physical device automatically.
    for (const auto& name : outputNames)
        appendUniqueOutput(name);

    std::wstring newOutputNodes;
    for (const auto& name : mergedOutputs)
        newOutputNodes += buildNode(name, seenOutputs);

    // --- Replace EnabledMIDIInputPort section content ---
    auto replaceSection = [&](std::wstring& xml, const std::wstring& objectType,
                              const std::wstring& newNodes) -> bool
    {
        const std::wstring kOpen = L"<ObjectList ObjectType=\"" + objectType + L"\">";
        size_t secStart = xml.find(kOpen);
        if (secStart == std::wstring::npos)
        {
            printf("[WriteHauptwerkMidiConfig] Section '%S' not found.\n", objectType.c_str());
            return false;
        }
        size_t contentStart = secStart + kOpen.size();
        // Content ends at next <ObjectList (or </ObjectList> if present)
        size_t nextOL = xml.find(L"<ObjectList", contentStart);
        // Also check for </ObjectList>
        size_t closeOL = xml.find(L"</ObjectList>", contentStart);
        size_t contentEnd;
        if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
            contentEnd = closeOL;
        else if (nextOL != std::wstring::npos)
            contentEnd = nextOL;
        else
            contentEnd = xml.size();

        // Count existing <o> nodes being removed
        std::wstring oldContent = xml.substr(contentStart, contentEnd - contentStart);
        int oldCount = 0;
        size_t p = 0;
        while ((p = oldContent.find(L"<o>", p)) != std::wstring::npos) { ++oldCount; p += 3; }

        // Count new <o> nodes being written
        int newCount = 0;
        p = 0;
        while ((p = newNodes.find(L"<o>", p)) != std::wstring::npos) { ++newCount; p += 3; }

        printf("[WriteHauptwerkMidiConfig] Section '%S': removed %d old port(s), writing %d new port(s).\n",
               objectType.c_str(), oldCount, newCount);

        xml = xml.substr(0, contentStart) + newNodes + L"\r\n" + xml.substr(contentEnd);
        return true;
    };

    if (!replaceSection(hwXml, L"EnabledMIDIInputPort",  newInputNodes))
        printf("[WriteHauptwerkMidiConfig] Warning: EnabledMIDIInputPort not replaced.\n");
    if (!replaceSection(hwXml, L"EnabledMIDIOutputPort", newOutputNodes))
        printf("[WriteHauptwerkMidiConfig] Warning: EnabledMIDIOutputPort not replaced.\n");

    // --- Write back as UTF-8 ---
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
    {
        printf("[WriteHauptwerkMidiConfig] UTF-8 conversion failed.\n");
        return false;
    }
    std::string utf8(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, utf8.data(), utf8Size, nullptr, nullptr);

    HANDLE fh = CreateFileW(configPath.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE)
    {
        printf("[WriteHauptwerkMidiConfig] Cannot write config (error %lu).\n", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(fh, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(fh);
    printf("[WriteHauptwerkMidiConfig] Written %lu bytes to %S\n", written, configPath.c_str());
    bool success = ok != FALSE && written == utf8.size();
    if (success)
        RefreshSettingsFile();
    return success;
}

bool WriteHauptwerkAudioConfig()
{
    // Resolve config file path
    std::wstring userDataRoot = s_rootHauptwerkUserData;
    if (userDataRoot.empty())
    {
        std::wstring xml;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                                     L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }
    }
    if (userDataRoot.empty())
    {
        printf("[WriteHauptwerkAudioConfig] RootFolder_HauptwerkUserData not configured.\n");
        return false;
    }
    if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
        userDataRoot.pop_back();

    std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";

    std::wstring hwXml = ReadFileToWString(configPath);
    if (hwXml.empty())
    {
        printf("[WriteHauptwerkAudioConfig] Cannot read config: %S\n", configPath.c_str());
        return false;
    }

    // Fixed content for AudioOutputSystem
    const std::wstring kAudioSystemNodes =
        L"\r\n<o><id>1</id><nam>Close</nam></o>"
        L"\r\n<o><id>2</id><nam>Organist</nam></o>"
        L"\r\n<o><id>3</id><nam>Diffuse</nam></o>"
        L"\r\n<o><id>4</id><nam>Rear Surround</nam></o>"
        L"\r\n<o><id>5</id><nam>Rear</nam></o>"
        L"\r\n<o><id>6</id><nam>MultiChannels</nam></o>";

    // Read all audio output devices from AudioOutputDevice section;
    // also identify the first ASIO: device to use as <dev> in AudioOutputUnit.
    std::wstring asioDevId, asioDevName;
    {
        const std::wstring kDevOpen = L"<ObjectList ObjectType=\"AudioOutputDevice\">";
        size_t devSecStart = hwXml.find(kDevOpen);
        if (devSecStart != std::wstring::npos)
        {
            size_t contentStart = devSecStart + kDevOpen.size();
            size_t nextOL  = hwXml.find(L"<ObjectList", contentStart);
            size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
            size_t contentEnd;
            if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
                contentEnd = closeOL;
            else if (nextOL != std::wstring::npos)
                contentEnd = nextOL;
            else
                contentEnd = hwXml.size();

            std::wstring devSection = hwXml.substr(contentStart, contentEnd - contentStart);
            std::wstring allDevFragment; // will be stored in s_cachedAudioDevices
            size_t pos = 0;
            while (pos < devSection.size())
            {
                size_t oStart = devSection.find(L"<o>", pos);
                if (oStart == std::wstring::npos) break;
                size_t oEnd = devSection.find(L"</o>", oStart);
                if (oEnd == std::wstring::npos) break;
                std::wstring node = devSection.substr(oStart + 3, oEnd - oStart - 3);
                std::wstring nam, id;
                TryGetTagStringValue(node, L"<nam>", L"</nam>", nam);
                TryGetTagStringValue(node, L"<id>",  L"</id>",  id);

                if (!id.empty() && !nam.empty())
                {
                    allDevFragment += L"      <Device id=\"" + id + L"\">" + nam + L"</Device>\r\n";
                    printf("[WriteHauptwerkAudioConfig] AudioOutputDevice: id=%S nam=%S\n",
                           id.c_str(), nam.c_str());

                    // First ASIO: device becomes the <dev> for AudioOutputUnit
                    if (asioDevId.empty() && nam.compare(0, 5, L"ASIO:") == 0)
                    {
                        asioDevId   = id;
                        asioDevName = nam;
                        printf("[WriteHauptwerkAudioConfig] Selected ASIO device: id=%S nam=%S\n",
                               id.c_str(), nam.c_str());
                    }
                }
                pos = oEnd + 4;
            }

            // Update cache
            s_cachedAudioDevices  = allDevFragment;
            s_cachedAsioDevId     = asioDevId;
            s_cachedAsioDevName   = asioDevName;
            s_audioSettingsLoaded = true;
            s_installedOrgansLoaded = false;
        }
        if (asioDevId.empty())
            printf("[WriteHauptwerkAudioConfig] Warning: no ASIO device found in AudioOutputDevice, <dev> will be empty.\n");
    }

    // Helper: build a single AudioOutputUnit <dev> tag with the resolved id
    auto devTag = [&]() -> std::wstring { return L"<dev>" + asioDevId + L"</dev>"; };

    // Fixed content for AudioOutputUnit
    std::wstring kAudioUnitNodes;
    kAudioUnitNodes +=
        L"\r\n<o><id>1</id>"  + devTag() + L"<typ>2</typ><ch1dvch>1</ch1dvch><ch2dvch>2</ch2dvch><bufsz>32</bufsz><recfmt>2</recfmt><nam>10). FRONT </nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>2</id>"  + devTag() + L"<typ>2</typ><ch1dvch>3</ch1dvch><ch2dvch>4</ch2dvch><bufsz>32</bufsz><recfmt>2</recfmt><nam>11). SIDE</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>3</id>"  + devTag() + L"<typ>2</typ><ch1dvch>7</ch1dvch><ch2dvch>8</ch2dvch><bufsz>32</bufsz><recfmt>2</recfmt><nam>13). REAR</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>4</id>"  + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>1</grp><ax1out>1</ax1out><nam>A). Close</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>5</id>"  + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>2</grp><ax1out>1</ax1out><nam>B). Organist</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>6</id>"  + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>3</grp><ax1out>2</ax1out><nam>C). Diffuse</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>7</id>"  + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>4</grp><ax1out>2</ax1out><nam>D). Rear Surround</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>8</id>"  + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>5</grp><ax1out>3</ax1out><nam>E). Rear</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>9</id>"  + devTag() + L"<typ>2</typ><ch1dvch>5</ch1dvch><ch2dvch>6</ch2dvch><bufsz>32</bufsz><recfmt>2</recfmt><nam>12). REAR SURROUND</nam></o>";
    kAudioUnitNodes +=
        L"\r\n<o><id>10</id>" + devTag() + L"<bufsz>32</bufsz><recfmt>2</recfmt><grp>6</grp><ax1out>1</ax1out><ax2out>2</ax2out><ax3out>9</ax3out><ax4out>3</ax4out><nam>F). MultiChannels</nam></o>";

    // Helper: clear all <o> children inside an ObjectList section and replace with newNodes
    auto replaceAudioSection = [&](std::wstring& xml, const std::wstring& objectType,
                                   const std::wstring& newNodes) -> bool
    {
        const std::wstring kOpen = L"<ObjectList ObjectType=\"" + objectType + L"\">";
        size_t secStart = xml.find(kOpen);
        if (secStart == std::wstring::npos)
        {
            printf("[WriteHauptwerkAudioConfig] Section '%S' not found.\n", objectType.c_str());
            return false;
        }
        size_t contentStart = secStart + kOpen.size();
        size_t nextOL  = xml.find(L"<ObjectList", contentStart);
        size_t closeOL = xml.find(L"</ObjectList>", contentStart);
        size_t contentEnd;
        if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
            contentEnd = closeOL;
        else if (nextOL != std::wstring::npos)
            contentEnd = nextOL;
        else
            contentEnd = xml.size();

        int oldCount = 0;
        size_t p = contentStart;
        while ((p = xml.find(L"<o>", p)) != std::wstring::npos && p < contentEnd) { ++oldCount; p += 3; }

        int newCount = 0;
        p = 0;
        while ((p = newNodes.find(L"<o>", p)) != std::wstring::npos) { ++newCount; p += 3; }

        printf("[WriteHauptwerkAudioConfig] Section '%S': removed %d old node(s), writing %d new node(s).\n",
               objectType.c_str(), oldCount, newCount);

        xml = xml.substr(0, contentStart) + newNodes + L"\r\n" + xml.substr(contentEnd);
        return true;
    };

    if (!replaceAudioSection(hwXml, L"AudioOutputSystem", kAudioSystemNodes))
        printf("[WriteHauptwerkAudioConfig] Warning: AudioOutputSystem not replaced.\n");
    if (!replaceAudioSection(hwXml, L"AudioOutputUnit", kAudioUnitNodes))
        printf("[WriteHauptwerkAudioConfig] Warning: AudioOutputUnit not replaced.\n");

    // Write back as UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
    {
        printf("[WriteHauptwerkAudioConfig] UTF-8 conversion failed.\n");
        return false;
    }
    std::string utf8(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, hwXml.c_str(), -1, utf8.data(), utf8Size, nullptr, nullptr);

    HANDLE fh = CreateFileW(configPath.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE)
    {
        printf("[WriteHauptwerkAudioConfig] Cannot write config (error %lu).\n", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(fh, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(fh);
    printf("[WriteHauptwerkAudioConfig] Written %lu bytes to %S\n", written, configPath.c_str());
    bool success = ok != FALSE && written == static_cast<DWORD>(utf8.size());
    if (success)
        RefreshSettingsFile(); // persist <Audio> section to Settings.xml
    return success;
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
    // "First launch" detection: we consider the app configured if at least
    // one MIDI input device is assigned in <AssignedMidiInputs>.
    EnsureAssignedDevicesLoaded();
    if (s_assignedInputNames.empty())
        return false;

    // Return the system device index of the first assigned input, if found.
    return FindMidiInputDeviceIndex(s_assignedInputNames[0], deviceId);
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
    // Require the <Midi> section, then <MidiOptions> inside it.
    std::wstring midiSection;
    if (!TryGetSection(xml, L"Midi", midiSection))
    {
        return false;
    }

    std::wstring midiOptions;
    if (!TryGetSection(midiSection, L"MidiOptions", midiOptions))
    {
        // fallback: old schema had <MidiRouterEnabled> directly under <Midi>
        midiOptions = midiSection;
    }

    if (!TryGetTagValue(midiOptions, L"<MidiRouterEnabled>", L"</MidiRouterEnabled>", value))
    {
        return false;
    }

    enabled = value != 0;
    return true;
}

bool LoadBidulePath(std::wstring& path)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    if (!TryGetTagStringValue(optionsSection, L"<BidulePath>", L"</BidulePath>", path))
    {
        path.clear();
        return true;
    }

    s_cachedBidulePath = path;
    return true;
}

bool SaveBidulePath(const std::wstring& path)
{
    s_cachedBidulePath = path;

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

bool LoadBiduleCloseOnUnload(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    UINT value = 1;
    if (!TryGetTagValue(optionsSection, L"<BiduleCloseOnUnload>", L"</BiduleCloseOnUnload>", value))
    {
        enabled = true;
        return true;
    }

    enabled = value != 0;
    s_cachedBiduleCloseOnUnload = enabled;
    return true;
}

bool SaveBiduleCloseOnUnload(bool enabled)
{
    s_cachedBiduleCloseOnUnload = enabled;

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

bool LoadLastSeenAppVersion(std::wstring& version)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    if (!TryGetTagStringValue(optionsSection, L"<LastSeenAppVersion>", L"</LastSeenAppVersion>", version))
    {
        version.clear();
        return true;
    }

    return true;
}

bool SaveLastSeenAppVersion(const std::wstring& version)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    const std::wstring openTag = L"<LastSeenAppVersion>";
    const std::wstring closeTag = L"</LastSeenAppVersion>";
    const std::wstring replacement = openTag + version + closeTag;

    size_t start = xml.find(openTag);
    if (start != std::wstring::npos)
    {
        size_t end = xml.find(closeTag, start + openTag.size());
        if (end == std::wstring::npos)
            return false;
        end += closeTag.size();
        xml.replace(start, end - start, replacement);
    }
    else
    {
        const std::wstring optionsClose = L"</Options>";
        size_t optionsPos = xml.find(optionsClose);
        if (optionsPos == std::wstring::npos)
            return false;

        std::wstring insertion = L"    " + replacement + L"\r\n";
        xml.insert(optionsPos, insertion);
    }

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, xml.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
        return false;

    std::string utf8(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, xml.c_str(), -1, utf8.data(), utf8Size, nullptr, nullptr);

    std::wstring settingsFile = GetSettingsFilePath();
    HANDLE fileHandle = CreateFileW(settingsFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
        return false;

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(fileHandle, utf8.data(), static_cast<DWORD>(utf8.size()), &bytesWritten, nullptr);
    CloseHandle(fileHandle);

    return ok != FALSE && bytesWritten == static_cast<DWORD>(utf8.size());
}

// Reads CHANGELOG.md from the app directory and extracts the bullet points
// for the given version (e.g. "1.0.109"). Returns an empty string if not found.
std::wstring LoadChangelogForVersion(const std::wstring& version)
{
    // Build path: <exeDir>\CHANGELOG.md
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return {};
    *lastSlash = L'\0';
    std::wstring changelogPath = std::wstring(exePath) + L"\\CHANGELOG.md";

    HANDLE hFile = CreateFileW(changelogPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return {};

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return {}; }

    std::string utf8(fileSize, '\0');
    DWORD bytesRead = 0;
    ReadFile(hFile, utf8.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);

    // Convert UTF-8 to wide
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(bytesRead), nullptr, 0);
    if (wLen <= 0) return {};
    std::wstring content(wLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(bytesRead), content.data(), wLen);

    // Find the section header "## <version>"
    std::wstring header = L"## " + version;
    size_t start = content.find(header);
    if (start == std::wstring::npos) return {};
    start = content.find(L'\n', start);
    if (start == std::wstring::npos) return {};
    ++start;

    // Read until next "## " section or end of file
    size_t end = content.find(L"\n## ", start);
    std::wstring section = (end == std::wstring::npos)
        ? content.substr(start)
        : content.substr(start, end - start);

    // Strip leading/trailing whitespace
    size_t first = section.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    size_t last = section.find_last_not_of(L" \t\r\n");
    return section.substr(first, last - first + 1);
}

bool LoadActiveSensingEnabled(bool& enabled)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;

    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;

    UINT value = 0;
    if (!TryGetTagValue(optionsSection, L"<ActiveSensingEnabled>", L"</ActiveSensingEnabled>", value))
    {
        enabled = false;
        return true;
    }

    enabled = value != 0;
    return true;
}

bool SaveActiveSensingEnabled(bool enabled)
{
    g_activeSensingEnabled.store(enabled);
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

bool LoadActiveSensingOutputName(std::wstring& name)
{
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return false;
    std::wstring optionsSection;
    if (!TryGetSection(xml, L"Options", optionsSection))
        return false;
    return TryGetTagStringValue(optionsSection, L"<ActiveSensingOutput>", L"</ActiveSensingOutput>", name);
}

bool SaveActiveSensingOutputName(const std::wstring& name)
{
    SetActiveSensingOutputName(name);

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
            // Physical output devices are owned exclusively by Hauptwerk.
            // The bridge only opens AhlbornBridge Virtual Port as its output.
            // Do not reopen any physical MIDI output devices here.
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

    // Parse <Organ id="NN">name ... </Organ> entries.
    // The organ name is the text immediately after '>' and before the first '\r', '\n' or '<'.
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
        // Extract only the text portion (up to the first newline or '<' child element)
        std::wstring inner = section.substr(gt + 1, end - gt - 1);
        size_t nameEnd = inner.find_first_of(L"\r\n<");
        std::wstring organName = (nameEnd != std::wstring::npos) ? inner.substr(0, nameEnd) : inner;
        names.push_back(organName);
        pos = end + 8; // skip past "</Organ>"
    }

    return names;
}

std::vector<AhlbornSwitchInfo> LoadAhlbornSwitches()
{
    std::vector<AhlbornSwitchInfo> result;
    std::wstring xml;
    if (!TryReadSettingsXml(xml))
        return result;

    std::wstring streamDeckSection;
    if (!TryGetSection(xml, L"StreamDeck", streamDeckSection))
        return result;

    std::wstring switchesSection;
    if (!TryGetSection(streamDeckSection, L"AhlbornSwitches", switchesSection))
        return result;

    size_t pos = 0;
    while (true)
    {
        size_t start = switchesSection.find(L"<o>", pos);
        if (start == std::wstring::npos)
            break;
        size_t end = switchesSection.find(L"</o>", start);
        if (end == std::wstring::npos)
            break;

        std::wstring item = switchesSection.substr(start, end - start + 4);
        AhlbornSwitchInfo info;
        std::wstring tmp;
        UINT val = 0;

        if (!TryGetTagStringValue(item, L"<nam>", L"</nam>", info.name) || info.name.empty())
        {
            pos = end + 4;
            continue;
        }
        if (TryGetTagValue(item, L"<h>", L"</h>", val)) info.channel = static_cast<int>(val);
        if (TryGetTagValue(item, L"<c>", L"</c>", val)) info.controlChange = static_cast<int>(val);
        if (TryGetTagValue(item, L"<d>", L"</d>", val)) info.valueOn = static_cast<int>(val);
        if (TryGetTagValue(item, L"<e>", L"</e>", val)) info.valueOff = static_cast<int>(val);
        if (TryGetTagValue(item, L"<m>", L"</m>", val)) info.momentary = (val != 0);
        result.push_back(info);
        pos = end + 4;
    }

    return result;
}

// ---------- In-memory switch state cache (written to disk only on unload/shutdown) ----------
static std::map<std::wstring, std::map<int, bool>> g_inMemorySwitchStates;
static CRITICAL_SECTION g_switchStateMemLock;
static bool g_switchStateMemLockInit = false;
// When true, UpdateInMemorySwitchState is a no-op so that reset CCs sent
// by the physical console during organ unload do not overwrite the state
// we captured just before the unload was triggered.
static bool g_switchStateFrozen = false;

// Maps switch index -> stop name (populated during UpdateInMemorySwitchState calls).
static std::map<int, std::wstring> g_switchNames;

static void EnsureSwitchStateMemLock()
{
    if (!g_switchStateMemLockInit)
    {
        InitializeCriticalSection(&g_switchStateMemLock);
        g_switchStateMemLockInit = true;
    }
}

void FreezeOrganSwitchStateForFlush()
{
    EnsureSwitchStateMemLock();
    EnterCriticalSection(&g_switchStateMemLock);
    g_switchStateFrozen = true;
    LeaveCriticalSection(&g_switchStateMemLock);
}

void UnfreezeOrganSwitchState()
{
    EnsureSwitchStateMemLock();
    EnterCriticalSection(&g_switchStateMemLock);
    g_switchStateFrozen = false;
    LeaveCriticalSection(&g_switchStateMemLock);
}

void UpdateInMemorySwitchState(const std::wstring& uniqueOrganId, int switchIndex, bool isOn, const std::wstring& switchName)
{
    if (uniqueOrganId.empty() || switchIndex <= 0)
        return;
    EnsureSwitchStateMemLock();
    EnterCriticalSection(&g_switchStateMemLock);
    if (!g_switchStateFrozen)
    {
        g_inMemorySwitchStates[uniqueOrganId][switchIndex] = isOn;
        if (!switchName.empty())
            g_switchNames[switchIndex] = switchName;
    }
    LeaveCriticalSection(&g_switchStateMemLock);
}

bool FlushOrganSwitchStatesToDisk()
{
    // Pre-populate g_switchNames from the configured switch list so that
    // nodes already on disk (written in previous sessions without nam="")
    // also get the name attribute on the next flush.
    {
        std::vector<AhlbornSwitchInfo> switches = LoadAhlbornSwitches();
        EnsureSwitchStateMemLock();
        EnterCriticalSection(&g_switchStateMemLock);
        for (size_t i = 0; i < switches.size(); ++i)
        {
            int idx = static_cast<int>(i) + 1;
            if (g_switchNames.find(idx) == g_switchNames.end() && !switches[i].name.empty())
                g_switchNames[idx] = switches[i].name;
        }
        LeaveCriticalSection(&g_switchStateMemLock);
    }

    EnsureSwitchStateMemLock();
    EnterCriticalSection(&g_switchStateMemLock);
    std::map<std::wstring, std::map<int, bool>> snapshot = g_inMemorySwitchStates;
    LeaveCriticalSection(&g_switchStateMemLock);

    if (snapshot.empty() && g_switchNames.empty())
    {
        // Even with an empty snapshot, we must still write the file if the
        // disk has persisted ON-states (e.g. user turned off all stops while
        // the freeze was active).  Check whether the disk has any Organ nodes.
        std::wstring probeXml;
        bool diskHasOrganNodes = false;
        if (TryReadSettingsXml(probeXml))
        {
            std::wstring sd, sw;
            if (TryGetSection(probeXml, L"StreamDeck", sd) &&
                TryGetSection(sd, L"OrganSwitchStates", sw))
                diskHasOrganNodes = (sw.find(L"<Organ") != std::wstring::npos);
        }
        if (!diskHasOrganNodes)
        {
            printf("[SwitchFlush] Nothing to flush.\n");
            return true;
        }
        printf("[SwitchFlush] Snapshot empty but disk has Organ nodes — clearing them.\n");
        // Fall through with empty snapshot so the merge produces no Organ nodes.
    }

    EnsureStreamDeckSettingsLoaded();

    // Merge snapshot into the existing persisted states.
    // Read directly from disk (not from cache) so that manual edits to
    // Settings.xml (e.g. deleting an <Organ> node) are respected.
    std::map<std::wstring, std::map<int, bool>> allStates;
    std::wstring section;
    {
        std::wstring freshXml;
        if (TryReadSettingsXml(freshXml))
        {
            std::wstring streamDeckSection, switchStatesSection;
            if (TryGetSection(freshXml, L"StreamDeck", streamDeckSection) &&
                TryGetSection(streamDeckSection, L"OrganSwitchStates", switchStatesSection))
            {
                size_t first = switchStatesSection.find(L'<');
                if (first != std::wstring::npos)
                    section = switchStatesSection.substr(first);
            }
        }
    }
    size_t pos = 0;
    while (true)
    {
        size_t start = section.find(L"<Organ", pos);
        if (start == std::wstring::npos) break;
        size_t openEnd = section.find(L'>', start);
        size_t close   = section.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || close == std::wstring::npos) break;

        std::wstring organNode = section.substr(start, close - start + 8);
        size_t idPos = organNode.find(L"id=\"");
        if (idPos != std::wstring::npos)
        {
            idPos += 4;
            size_t idEnd = organNode.find(L'\"', idPos);
            if (idEnd != std::wstring::npos)
            {
                std::wstring oid = organNode.substr(idPos, idEnd - idPos);
                std::map<int, bool> organStates;
                size_t spos = 0;
                while (true)
                {
                    size_t sstart = organNode.find(L"<s", spos);
                    if (sstart == std::wstring::npos) break;
                    size_t send = organNode.find(L"/>", sstart);
                    if (send == std::wstring::npos) break;
                    std::wstring switchNode = organNode.substr(sstart, send - sstart + 2);
                    size_t idxPos2 = switchNode.find(L"idx=\"");
                    size_t onPos  = switchNode.find(L"on=\"");
                    if (idxPos2 != std::wstring::npos && onPos != std::wstring::npos)
                    {
                        idxPos2 += 5; onPos += 4;
                        size_t idxEnd = switchNode.find(L'\"', idxPos2);
                        size_t onEnd  = switchNode.find(L'\"', onPos);
                        if (idxEnd != std::wstring::npos && onEnd != std::wstring::npos)
                        {
                            int idx = _wtoi(switchNode.substr(idxPos2, idxEnd - idxPos2).c_str());
                            int on  = _wtoi(switchNode.substr(onPos, onEnd - onPos).c_str());
                            if (idx > 0) organStates[idx] = (on != 0);
                        }
                    }
                    spos = send + 2;
                }
                allStates[oid] = organStates;
            }
        }
        pos = close + 8;
    }

    // Apply in-memory snapshot on top of persisted data
    for (const auto& [oid, states] : snapshot)
        for (const auto& [idx, on] : states)
            allStates[oid][idx] = on;

    std::wstring serialized;
    for (const auto& [oid, organStates] : allStates)
    {
        std::wstring organBody;
        for (const auto& [idx, on] : organStates)
        {
            if (!on) continue;
            auto nameIt = g_switchNames.find(idx);
            std::wstring namAttr = (nameIt != g_switchNames.end() && !nameIt->second.empty())
                ? L" nam=\"" + nameIt->second + L"\""
                : L"";
            organBody += L"        <s idx=\"" + std::to_wstring(idx) + L"\"" + namAttr + L" on=\"1\"/>\r\n";
        }
        if (!organBody.empty())
            serialized += L"      <Organ id=\"" + oid + L"\">\r\n" + organBody + L"      </Organ>\r\n";
    }

    s_cachedOrganSwitchStates = serialized;
    s_organSwitchStatesCacheValid = true;

    bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
    bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
    bool showConsole = true; LoadShowDebugConsole(showConsole);
    bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);

    EnsureAssignedDevicesLoaded();
    std::wstring in1, in2, out1, out2;
    if (!s_assignedInputNames.empty())  in1  = s_assignedInputNames[0];
    if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
    if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
    if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];

    DeviceEnabledStates devEnabled;
    bool ok = WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
    printf("[SwitchFlush] Flushed %zu organ(s) switch states to disk: %s\n", snapshot.size(), ok ? "OK" : "FAILED");
    // Unfreeze so normal state updates resume after the flush
    g_switchStateFrozen = false;
    return ok;
}

bool SaveOrganSwitchState(const std::wstring& uniqueOrganId, int switchIndex, bool isOn)
{
    if (uniqueOrganId.empty() || switchIndex <= 0)
        return false;

    EnsureStreamDeckSettingsLoaded();

    std::map<int, bool> states = LoadOrganSwitchStates(uniqueOrganId);
    states[switchIndex] = isOn;

    std::map<std::wstring, std::map<int, bool>> allStates;
    std::wstring section = s_cachedOrganSwitchStates;
    size_t pos = 0;
    while (true)
    {
        size_t start = section.find(L"<Organ", pos);
        if (start == std::wstring::npos)
            break;
        size_t openEnd = section.find(L'>', start);
        size_t close = section.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || close == std::wstring::npos)
            break;

        std::wstring organNode = section.substr(start, close - start + 8);
        size_t idPos = organNode.find(L"id=\"");
        if (idPos != std::wstring::npos)
        {
            idPos += 4;
            size_t idEnd = organNode.find(L'\"', idPos);
            if (idEnd != std::wstring::npos)
            {
                std::wstring oid = organNode.substr(idPos, idEnd - idPos);
                std::map<int, bool> organStates;
                size_t spos = 0;
                while (true)
                {
                    size_t sstart = organNode.find(L"<s", spos);
                    if (sstart == std::wstring::npos)
                        break;
                    size_t send = organNode.find(L"/>", sstart);
                    if (send == std::wstring::npos)
                        break;
                    std::wstring switchNode = organNode.substr(sstart, send - sstart + 2);

                    size_t idxPos = switchNode.find(L"idx=\"");
                    size_t onPos = switchNode.find(L"on=\"");
                    if (idxPos != std::wstring::npos && onPos != std::wstring::npos)
                    {
                        idxPos += 5;
                        onPos += 4;
                        size_t idxEnd = switchNode.find(L'\"', idxPos);
                        size_t onEnd = switchNode.find(L'\"', onPos);
                        if (idxEnd != std::wstring::npos && onEnd != std::wstring::npos)
                        {
                            int idx = _wtoi(switchNode.substr(idxPos, idxEnd - idxPos).c_str());
                            int on = _wtoi(switchNode.substr(onPos, onEnd - onPos).c_str());
                            if (idx > 0)
                                organStates[idx] = (on != 0);
                        }
                    }
                    spos = send + 2;
                }
                allStates[oid] = organStates;
            }
        }

        pos = close + 8;
    }

    allStates[uniqueOrganId] = states;

    std::wstring serialized;
    for (const auto& [oid, organStates] : allStates)
    {
        std::wstring organBody;
        for (const auto& [idx, on] : organStates)
        {
            if (!on) continue;
            auto nameIt = g_switchNames.find(idx);
            std::wstring namAttr = (nameIt != g_switchNames.end() && !nameIt->second.empty())
                ? L" nam=\"" + nameIt->second + L"\""
                : L"";
            organBody += L"        <s idx=\"" + std::to_wstring(idx) + L"\"" + namAttr + L" on=\"1\"/>\r\n";
        }
        if (!organBody.empty())
            serialized += L"      <Organ id=\"" + oid + L"\">\r\n" + organBody + L"      </Organ>\r\n";
    }

    s_cachedOrganSwitchStates = serialized;
    s_organSwitchStatesCacheValid = true;

    bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
    bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
    bool showConsole = true; LoadShowDebugConsole(showConsole);
    bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);

    EnsureAssignedDevicesLoaded();
    std::wstring in1, in2;
    if (!s_assignedInputNames.empty()) in1 = s_assignedInputNames[0];
    if (s_assignedInputNames.size() > 1) in2 = s_assignedInputNames[1];
    std::wstring out1, out2;
    if (!s_assignedOutputNames.empty()) out1 = s_assignedOutputNames[0];
    if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];

    DeviceEnabledStates devEnabled;
    std::wstring xml;
    if (TryReadSettingsXml(xml))
    {
        std::wstring midiSection, devicesSection;
        if (TryGetSection(xml, L"Midi", midiSection) && TryGetSection(midiSection, L"SettingsDevices", devicesSection))
        {
            if (in1.empty()) TryGetTagStringValue(devicesSection, L"<MidiInputDevice01>", L"</MidiInputDevice01>", in1);
            if (in2.empty()) TryGetTagStringValue(devicesSection, L"<MidiInputDevice02>", L"</MidiInputDevice02>", in2);
            if (out1.empty()) TryGetTagStringValue(devicesSection, L"<MidiOutputDevice01>", L"</MidiOutputDevice01>", out1);
            if (out2.empty()) TryGetTagStringValue(devicesSection, L"<MidiOutputDevice02>", L"</MidiOutputDevice02>", out2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice01", devEnabled.input1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiInputDevice02", devEnabled.input2);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice01", devEnabled.output1);
            TryGetTagEnabledAttribute(devicesSection, L"MidiOutputDevice02", devEnabled.output2);
        }
    }

    bool writeOk = WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
    printf("[SaveOrganSwitchState] WriteSettingsXml result=%d, organId=%S, switchIdx=%d, isOn=%d\n",
        (int)writeOk, uniqueOrganId.c_str(), switchIndex, (int)isOn);
    return writeOk;
}

std::map<int, bool> LoadOrganSwitchStates(const std::wstring& uniqueOrganId)
{
    std::map<int, bool> result;
    if (uniqueOrganId.empty())
        return result;

    // Always read from disk so the restore uses the last flushed values,
    // not a potentially stale in-memory cache.
    std::wstring section;
    {
        std::wstring freshXml;
        if (TryReadSettingsXml(freshXml))
        {
            std::wstring streamDeckSection, switchStatesSection;
            if (TryGetSection(freshXml, L"StreamDeck", streamDeckSection) &&
                TryGetSection(streamDeckSection, L"OrganSwitchStates", switchStatesSection))
            {
                size_t first = switchStatesSection.find(L'<');
                if (first != std::wstring::npos)
                    section = switchStatesSection.substr(first);
            }
        }
    }
    if (section.empty())
        return result;
    size_t pos = 0;
    while (true)
    {
        size_t start = section.find(L"<Organ", pos);
        if (start == std::wstring::npos)
            break;
        size_t openEnd = section.find(L'>', start);
        size_t close = section.find(L"</Organ>", openEnd);
        if (openEnd == std::wstring::npos || close == std::wstring::npos)
            break;

        std::wstring organNode = section.substr(start, close - start + 8);
        size_t idPos = organNode.find(L"id=\"");
        if (idPos != std::wstring::npos)
        {
            idPos += 4;
            size_t idEnd = organNode.find(L'\"', idPos);
            if (idEnd != std::wstring::npos)
            {
                std::wstring oid = organNode.substr(idPos, idEnd - idPos);
                if (oid == uniqueOrganId)
                {
                    size_t spos = 0;
                    while (true)
                    {
                        size_t sstart = organNode.find(L"<s", spos);
                        if (sstart == std::wstring::npos)
                            break;
                        size_t send = organNode.find(L"/>", sstart);
                        if (send == std::wstring::npos)
                            break;
                        std::wstring switchNode = organNode.substr(sstart, send - sstart + 2);

                        size_t idxPos = switchNode.find(L"idx=\"");
                        size_t onPos = switchNode.find(L"on=\"");
                        if (idxPos != std::wstring::npos && onPos != std::wstring::npos)
                        {
                            idxPos += 5;
                            onPos += 4;
                            size_t idxEnd = switchNode.find(L'\"', idxPos);
                            size_t onEnd = switchNode.find(L'\"', onPos);
                            if (idxEnd != std::wstring::npos && onEnd != std::wstring::npos)
                            {
                                int idx = _wtoi(switchNode.substr(idxPos, idxEnd - idxPos).c_str());
                                int on = _wtoi(switchNode.substr(onPos, onEnd - onPos).c_str());
                                if (idx > 0)
                                    result[idx] = (on != 0);
                            }
                        }
                        spos = send + 2;
                    }
                    return result;
                }
            }
        }

        pos = close + 8;
    }

    return result;
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

std::wstring LoadPrimaryHauptwerkOutputName()
{
    std::wstring fixedOutput = LoadFixedHauptwerkOutputName();
    if (!fixedOutput.empty())
        return fixedOutput;

    std::wstring userDataRoot = s_rootHauptwerkUserData;
    if (userDataRoot.empty())
    {
        std::wstring xml;
        if (TryReadSettingsXml(xml))
        {
            std::wstring opts;
            if (TryGetSection(xml, L"Options", opts))
                TryGetTagStringValue(opts, L"<RootFolder_HauptwerkUserData>",
                                     L"</RootFolder_HauptwerkUserData>", userDataRoot);
        }
    }

    if (userDataRoot.empty())
        return {};

    if (!userDataRoot.empty() && (userDataRoot.back() == L'\\' || userDataRoot.back() == L'/'))
        userDataRoot.pop_back();

    std::wstring configPath = userDataRoot + L"\\Config0-GeneralSettings\\Config.Config_Hauptwerk_xml";
    std::wstring hwXml = ReadFileToWString(configPath);
    if (hwXml.empty())
        return {};

    const std::wstring kOpen = L"<ObjectList ObjectType=\"EnabledMIDIOutputPort\">";
    size_t secStart = hwXml.find(kOpen);
    if (secStart == std::wstring::npos)
        return {};

    size_t contentStart = secStart + kOpen.size();
    size_t nextOL = hwXml.find(L"<ObjectList", contentStart);
    size_t closeOL = hwXml.find(L"</ObjectList>", contentStart);
    size_t contentEnd;
    if (closeOL != std::wstring::npos && (nextOL == std::wstring::npos || closeOL < nextOL))
        contentEnd = closeOL;
    else if (nextOL != std::wstring::npos)
        contentEnd = nextOL;
    else
        contentEnd = hwXml.size();

    std::wstring section = hwXml.substr(contentStart, contentEnd - contentStart);
    size_t pos = 0;
    while (pos < section.size())
    {
        size_t oStart = section.find(L"<o>", pos);
        if (oStart == std::wstring::npos) break;
        size_t oEnd = section.find(L"</o>", oStart);
        if (oEnd == std::wstring::npos) break;

        std::wstring node = section.substr(oStart + 3, oEnd - oStart - 3);
        std::wstring name;
        TryGetTagStringValue(node, L"<nam>", L"</nam>", name);
        if (!name.empty()
            && name != L"AhlbornBridge Virtual Port"
            && name != L"AhlbornBridge Virtual Port (B)"
            && name != L"Hauptwerk Virtual (A)"
            && name != L"Hauptwerk Virtual (B)")
            return name;

        pos = oEnd + 4;
    }

    return {};
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

bool SaveStreamDeckPipeServerEnabled(bool enabled)
{
    s_cachedStreamDeckPipeServerEnabled = enabled;

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

bool LoadStreamDeckPipeServerEnabled(bool& enabled)
{
    EnsureStreamDeckSettingsLoaded();
    enabled = s_cachedStreamDeckPipeServerEnabled;
    return true;
}

bool SaveHauptwerkPrioritySettings(const std::wstring& idlePriority, const std::wstring& loadedPriority)
{
    s_cachedHauptwerkIdlePriority   = idlePriority;
    s_cachedHauptwerkLoadedPriority = loadedPriority;

    // Persist immediately via the normal settings write path so the values
    // survive even if the app exits before the next full WriteSettingsXml call.
    bool routerEnabled = false; LoadMidiRouterEnabled(routerEnabled);
    bool closeOnDisconnect = false; LoadCloseSettingsOnDisconnect(closeOnDisconnect);
    bool showConsole = true; LoadShowDebugConsole(showConsole);
    bool checkUpdate = true; LoadCheckForUpdateOnStart(checkUpdate);
    EnsureAssignedDevicesLoaded();
    std::wstring in1, in2, out1, out2;
    if (!s_assignedInputNames.empty())   in1  = s_assignedInputNames[0];
    if (s_assignedInputNames.size() > 1) in2  = s_assignedInputNames[1];
    if (!s_assignedOutputNames.empty())  out1 = s_assignedOutputNames[0];
    if (s_assignedOutputNames.size() > 1) out2 = s_assignedOutputNames[1];
    DeviceEnabledStates devEnabled;
    bool ok = WriteSettingsXml(in1, in2, out1, out2, routerEnabled, closeOnDisconnect, showConsole, checkUpdate, devEnabled);
    // Apply the new priority to Hauptwerk immediately without waiting for the enforcer cycle
    ApplyHauptwerkPriorityNow();
    return ok;
}

bool LoadHauptwerkPrioritySettings(std::wstring& idlePriority, std::wstring& loadedPriority)
{
    EnsureHauptwerkPriorityLoaded();
    idlePriority   = s_cachedHauptwerkIdlePriority;
    loadedPriority = s_cachedHauptwerkLoadedPriority;
    return true;
}
