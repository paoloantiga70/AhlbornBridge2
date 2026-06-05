// MidiAssignmentWindow.cpp
// MIDI device assignment window.
// Two list-pair sections (Inputs / Outputs): Available <-> Assigned.
// The "Assigned Inputs" list shows a live signal LED in column 0.

#include "MidiAssignmentWindow.h"
#include "Midi.h"
#include "Xml.h"
#include "Midi2Endpoint.h"
#include "Hauptwerk.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dbt.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t kMidiAssignClassName[] = L"AhlbornMidiAssignWindow";
constexpr UINT kActivityTimerId   = 1;
constexpr UINT kRefreshListsMsg = WM_APP + 220;
constexpr UINT kActivityIntervalMs = 16; // ~60fps refresh
constexpr DWORD kActivityWindowMs  = 120; // LED stays green for 120 ms after last msg
constexpr int kInternalBridgeSectionHeight = 110;
constexpr int kBridgeToggleHeight = 22;

enum
{
	IDC_TV_AVAIL_IN  = 2003,
	IDC_TV_ASSIGN_IN = 2004,
	IDC_BTN_ADD_IN   = 2005,
	IDC_BTN_REM_IN   = 2006,
	IDC_BTN_UP_IN    = 2007,
	IDC_BTN_DOWN_IN  = 2008,

	IDC_TV_AVAIL_OUT  = 2013,
	IDC_TV_ASSIGN_OUT = 2014,
	IDC_BTN_ADD_OUT   = 2015,
	IDC_BTN_REM_OUT   = 2016,
	IDC_BTN_UP_OUT    = 2017,
	IDC_BTN_DOWN_OUT  = 2018,
	IDC_TV_INTERNAL    = 2019,
	IDC_CHK_BRIDGE_ENABLED = 2022,

	IDC_BTN_APPLY  = 2020,
	IDC_BTN_CANCEL = 2021,
};

static HWND g_assignHwnd = nullptr;
static HWND g_internalBridgeLv = nullptr;
static std::vector<std::wstring> g_initialAssignedInputs;
static std::vector<std::wstring> g_initialAssignedOutputs;
static std::wstring g_fixedInputName;
static std::wstring g_fixedOutputName;

// Shared ImageList used for LED icons in the "Assigned Inputs" listview
static HIMAGELIST g_ledImgList = nullptr; // indices: 0=grey, 1=green

// ---------------------------------------------------------------------------
// LED ImageList helpers
// ---------------------------------------------------------------------------
static HBITMAP MakeLedBitmap(HDC hdc, COLORREF col, int sz)
{
	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth       = sz;
	bmi.bmiHeader.biHeight      = -sz;
	bmi.bmiHeader.biPlanes      = 1;
	bmi.bmiHeader.biBitCount    = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!hBmp) return nullptr;

	HDC memDC = CreateCompatibleDC(hdc);
	HBITMAP old = (HBITMAP)SelectObject(memDC, hBmp);
	RECT rc = { 0, 0, sz, sz };
	FillRect(memDC, &rc, (HBRUSH)(COLOR_WINDOW + 1));
	HBRUSH br = CreateSolidBrush(col);
	HPEN   pn = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
	SelectObject(memDC, br);
	SelectObject(memDC, pn);
	Ellipse(memDC, 2, 2, sz - 2, sz - 2);
	DeleteObject(br);
	DeleteObject(pn);
	SelectObject(memDC, old);
	DeleteDC(memDC);
	return hBmp;
}

static void EnsureLedImageList(HWND hWnd)
{
	if (g_ledImgList) return;
	const int sz = 12;
	g_ledImgList = ImageList_Create(sz, sz, ILC_COLOR32, 2, 0);
	HDC hdc = GetDC(hWnd);
	HBITMAP bmpGrey  = MakeLedBitmap(hdc, RGB(100, 100, 100), sz);
	HBITMAP bmpGreen = MakeLedBitmap(hdc, RGB(0, 200, 0), sz);
	ReleaseDC(hWnd, hdc);
	ImageList_Add(g_ledImgList, bmpGrey,  nullptr); // index 0 = grey
	ImageList_Add(g_ledImgList, bmpGreen, nullptr); // index 1 = green
	DeleteObject(bmpGrey);
	DeleteObject(bmpGreen);
}

// ---------------------------------------------------------------------------
// Listview helpers
// ---------------------------------------------------------------------------

// Setup a report-mode listview with a LED column (col 0, 20px) and a name
// column (col 1, fills the rest).
static void LvSetupReportMode(HWND hLv, bool withLed)
{
	if (!withLed)
	{
		// Simple single-column report for "Assigned Outputs"
		LVCOLUMNW col = {};
		col.mask = LVCF_WIDTH | LVCF_TEXT;
		col.cx   = 300;
		col.pszText = const_cast<wchar_t*>(L"Device");
		ListView_InsertColumn(hLv, 0, &col);
		return;
	}
	EnsureLedImageList(hLv);
	ListView_SetImageList(hLv, g_ledImgList, LVSIL_SMALL);

	LVCOLUMNW col = {};
	col.mask    = LVCF_WIDTH | LVCF_TEXT;
	col.cx      = 20;
	col.pszText = const_cast<wchar_t*>(L"");
	ListView_InsertColumn(hLv, 0, &col);

	col.cx      = 200;
	col.pszText = const_cast<wchar_t*>(L"Device");
	ListView_InsertColumn(hLv, 1, &col);

	col.cx      = 180;
	col.pszText = const_cast<wchar_t*>(L"Role");
	ListView_InsertColumn(hLv, 2, &col);
}

// Add an item to a plain list-mode or single-column report listview
static void LvAddItem(HWND hLv, const wchar_t* text)
{
	LVITEMW lvi = {};
	lvi.mask    = LVIF_TEXT;
	lvi.iItem   = ListView_GetItemCount(hLv);
	lvi.pszText = const_cast<wchar_t*>(text);
	ListView_InsertItem(hLv, &lvi);
}

// Add item to a report listview with LED column + name column.
static void LvAddLedItem(HWND hLv, const wchar_t* text)
{
	int idx = ListView_GetItemCount(hLv);
	LVITEMW lvi = {};
	lvi.mask     = LVIF_TEXT | LVIF_IMAGE;
	lvi.iItem    = idx;
	lvi.iSubItem = 0;
	lvi.pszText  = const_cast<wchar_t*>(L"");
	lvi.iImage   = 0; // grey
	ListView_InsertItem(hLv, &lvi);
	ListView_SetItemText(hLv, idx, 1, const_cast<wchar_t*>(text));
}

// Get the name of an item (column 1 for report with LED, column 0 otherwise)
static std::wstring LvGetItemName(HWND hLv, int index, bool hasLedCol)
{
	wchar_t buf[256] = {};
	LVITEMW lvi = {};
	lvi.mask       = LVIF_TEXT;
	lvi.iItem      = index;
	lvi.iSubItem   = hasLedCol ? 1 : 0;
	lvi.pszText    = buf;
	lvi.cchTextMax = 255;
	ListView_GetItem(hLv, &lvi);
	return buf;
}

static int LvGetSelected(HWND hLv)
{
	return ListView_GetNextItem(hLv, -1, LVNI_SELECTED);
}

static std::vector<std::wstring> LvCollectNames(HWND hLv, bool hasLedCol)
{
	std::vector<std::wstring> v;
	int n = ListView_GetItemCount(hLv);
	for (int i = 0; i < n; ++i)
		v.push_back(LvGetItemName(hLv, i, hasLedCol));
	return v;
}

static void LvMoveItem(HWND hLv, bool up, bool hasLedCol)
{
	int sel = LvGetSelected(hLv);
	if (sel < 0) return;
	int tgt = up ? sel - 1 : sel + 1;
	int n   = ListView_GetItemCount(hLv);
	if (tgt < 0 || tgt >= n) return;

	std::wstring a = LvGetItemName(hLv, sel, hasLedCol);
	std::wstring b = LvGetItemName(hLv, tgt, hasLedCol);

	if (hasLedCol)
	{
		ListView_SetItemText(hLv, sel, 1, b.data());
		ListView_SetItemText(hLv, tgt, 1, a.data());
	}
	else
	{
		LVITEMW lvi = {};
		lvi.mask  = LVIF_TEXT;
		lvi.iItem = sel; lvi.pszText = b.data(); ListView_SetItem(hLv, &lvi);
		lvi.iItem = tgt; lvi.pszText = a.data(); ListView_SetItem(hLv, &lvi);
	}

	ListView_SetItemState(hLv, sel, 0,                          LVIS_SELECTED|LVIS_FOCUSED);
	ListView_SetItemState(hLv, tgt, LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);
	ListView_EnsureVisible(hLv, tgt, FALSE);
}

static void MoveSelected(HWND hSrc, bool srcHasLed, HWND hDst, bool dstHasLed)
{
	int sel = LvGetSelected(hSrc);
	if (sel < 0) return;
	std::wstring text = LvGetItemName(hSrc, sel, srcHasLed);
	if (dstHasLed) LvAddLedItem(hDst, text.c_str());
	else           LvAddItem(hDst, text.c_str());
	ListView_DeleteItem(hSrc, sel);
	int nc = ListView_GetItemCount(hSrc);
	if (nc > 0)
	{
		int ns = (sel < nc) ? sel : nc - 1;
		ListView_SetItemState(hSrc, ns, LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);
	}
}

// ---------------------------------------------------------------------------
// Populate
// ---------------------------------------------------------------------------
// Internal AhlbornBridge ports that must not appear as selectable inputs.
static bool IsInternalMidiPort(const std::wstring& name)
{
	return name == L"AhlbornBridge Virtual Port"
		|| name == L"AhlbornBridge Virtual Port (B)"
		|| name == L"Hauptwerk Virtual (A)"
		|| name == L"Hauptwerk Virtual (B)";
}

static bool IsFixedAssignedOutputName(const std::wstring& name)
{
	return name == L"AhlbornBridge Virtual Port"
		|| (!g_fixedOutputName.empty() && name == g_fixedOutputName)
		|| (!g_fixedOutputName.empty() && name == g_fixedOutputName + L" [fixed]");
}

static bool IsFixedAssignedInputName(const std::wstring& name)
{
	return !g_fixedInputName.empty()
		&& (name == g_fixedInputName || name == g_fixedInputName + L" [fixed]");
}

static std::wstring GetFixedInputDisplayName()
{
	return g_fixedInputName.empty() ? std::wstring{} : g_fixedInputName + L" [fixed]";
}

static std::wstring GetFixedOutputDisplayName()
{
	return g_fixedOutputName.empty() ? std::wstring{} : g_fixedOutputName + L" [fixed]";
}

static std::vector<std::wstring> CollectAssignedInputNames(HWND hLv)
{
	std::vector<std::wstring> names;
	int n = ListView_GetItemCount(hLv);
	for (int i = 0; i < n; ++i)
	{
		std::wstring name = LvGetItemName(hLv, i, true);
		if (name.empty())
			continue;
		names.push_back(name);
	}
	return names;
}

static std::vector<std::wstring> CollectPersistedOutputNames(HWND hLv)
{
	std::vector<std::wstring> names;
	// Always yield AhlbornBridge Virtual Port as the first/must-have output.
	names.push_back(L"AhlbornBridge Virtual Port");

	int n = ListView_GetItemCount(hLv);
	for (int i = 0; i < n; ++i)
	{
		std::wstring name = LvGetItemName(hLv, i, true);
		if (name.empty() || IsInternalMidiPort(name) || name == L"AhlbornBridge Virtual Port")
			continue;
		names.push_back(name);
	}

	return names;
}

static std::vector<std::wstring> EnumerateCurrentInputNames()
{
	std::vector<std::wstring> names;
	UINT n = midiInGetNumDevs();
	for (UINT i = 0; i < n; ++i)
	{
		MIDIINCAPS caps = {};
		if (midiInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		names.emplace_back(caps.szPname);
	}
	return names;
}

static std::vector<std::wstring> EnumerateCurrentOutputNames()
{
	std::vector<std::wstring> names;
	UINT n = midiOutGetNumDevs();
	for (UINT i = 0; i < n; ++i)
	{
		MIDIOUTCAPS caps = {};
		if (midiOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		names.emplace_back(caps.szPname);
	}
	return names;
}

static void PruneDisconnectedAssignedDevices()
{
	auto currentInputs = EnumerateCurrentInputNames();
	auto currentOutputs = EnumerateCurrentOutputNames();
	auto assignedInputs = LoadAssignedMidiInputNames();
	auto assignedOutputs = LoadAssignedMidiOutputNames();

	std::vector<std::wstring> filteredInputs;
	for (const auto& name : assignedInputs)
	{
		if (std::find(currentInputs.begin(), currentInputs.end(), name) != currentInputs.end())
			filteredInputs.push_back(name);
	}

	std::vector<std::wstring> filteredOutputs;
	for (const auto& name : assignedOutputs)
	{
		if (IsInternalMidiPort(name) || std::find(currentOutputs.begin(), currentOutputs.end(), name) != currentOutputs.end())
			filteredOutputs.push_back(name);
	}

	if (filteredInputs != assignedInputs)
	{
		SaveAssignedMidiInputNames(filteredInputs);
		SetAssignedMidiInputNames(filteredInputs);
	}

	if (filteredOutputs != assignedOutputs)
	{
		SaveAssignedMidiOutputNames(filteredOutputs);
		SetAssignedMidiOutputNames(filteredOutputs);
	}

	if (filteredInputs != assignedInputs || filteredOutputs != assignedOutputs)
		WriteHauptwerkMidiConfig(filteredInputs, filteredOutputs);
}

static void PopulateInputViews(HWND hAvail, HWND hAssign)
{
	ListView_DeleteAllItems(hAvail);
	ListView_DeleteAllItems(hAssign);

	auto assigned = LoadAssignedMidiInputNames();
	UINT n = midiInGetNumDevs();
	for (UINT i = 0; i < n; ++i)
	{
		MIDIINCAPS caps = {};
		if (midiInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		std::wstring name = caps.szPname;
		if (IsInternalMidiPort(name)) continue;
		if (std::find(assigned.begin(), assigned.end(), name) == assigned.end())
			LvAddItem(hAvail, name.c_str());
	}
	for (auto& name : assigned)
	{
		LvAddLedItem(hAssign, name.c_str());
	}
}

static void PopulateAvailableOutputView(
	HWND hAvail,
	const std::vector<std::wstring>& assignedOutputs,
	const std::vector<std::wstring>& assignedInputs)
{
	(void)assignedInputs;
	ListView_DeleteAllItems(hAvail);

	UINT n = midiOutGetNumDevs();
	for (UINT i = 0; i < n; ++i)
	{
		MIDIOUTCAPS caps = {};
		if (midiOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		std::wstring name = caps.szPname;
		if (IsInternalMidiPort(name)) continue;
		if (std::find(assignedOutputs.begin(), assignedOutputs.end(), name) != assignedOutputs.end()) continue;
		LvAddItem(hAvail, name.c_str());
	}
}

static std::vector<std::wstring> CollectAssignedOutputNames(HWND hLv)
{
	std::vector<std::wstring> names;
	int n = ListView_GetItemCount(hLv);
	for (int i = 0; i < n; ++i)
	{
		std::wstring name = LvGetItemName(hLv, i, true);
		if (name.empty() || IsInternalMidiPort(name))
			continue;
		names.push_back(name);
	}
	return names;
}

static void PopulateOutputViews(HWND hAvail, HWND hAssign)
{
	ListView_DeleteAllItems(hAssign);

	std::vector<std::wstring> hwActualInputs, hwActualOutputs;
	ReadActualHauptwerkMidiPorts(hwActualInputs, hwActualOutputs);

	auto assignedInputs  = LoadAssignedMidiInputNames();
	PopulateAvailableOutputView(hAvail, hwActualOutputs, assignedInputs);
	for (auto& name : hwActualOutputs)
	{
		if (IsInternalMidiPort(name))
			continue;
		LvAddLedItem(hAssign, name.c_str());
	}
}

static void PopulateInternalBridgePortsView(HWND hLv)
{
	ListView_DeleteAllItems(hLv);
	std::vector<std::wstring> ports;

	auto appendUnique = [&](const std::wstring& name)
	{
		if (!IsInternalMidiPort(name))
			return;
		if (std::find(ports.begin(), ports.end(), name) != ports.end())
			return;
		ports.push_back(name);
	};

	UINT inCount = midiInGetNumDevs();
	for (UINT i = 0; i < inCount; ++i)
	{
		MIDIINCAPS caps = {};
		if (midiInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		appendUnique(caps.szPname);
	}

	UINT outCount = midiOutGetNumDevs();
	for (UINT i = 0; i < outCount; ++i)
	{
		MIDIOUTCAPS caps = {};
		if (midiOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
		appendUnique(caps.szPname);
	}

	for (const auto& name : ports)
	{
		LvAddLedItem(hLv, name.c_str());
		int idx = ListView_GetItemCount(hLv) - 1;
		const wchar_t* role = L"";
		if (name == L"AhlbornBridge Virtual Port")
			role = L"AhlbornBridge MIDI Output (BRIDGE)";
		else if (name == L"AhlbornBridge Virtual Port (B)")
			role = L"Hauptwerk MIDI Input (BRIDGE)";
		else if (name == L"Hauptwerk Virtual (A)")
			role = L"Hauptwerk MIDI Output monitor (A)";
		else if (name == L"Hauptwerk Virtual (B)")
			role = L"Hauptwerk MIDI Output monitor (B)";
		ListView_SetItemText(hLv, idx, 2, const_cast<wchar_t*>(role));
	}
}

static void RefreshInternalBridgePortLeds(HWND hLv)
{
	if (!g_ledImgList || !hLv) return;
	int n = ListView_GetItemCount(hLv);
	DWORD now = GetTickCount();
	const bool bridgeEnabled = g_midiRouterEnabled.load();
	for (int i = 0; i < n; ++i)
	{
		std::wstring name = LvGetItemName(hLv, i, true);
		DWORD lastIn  = GetMidiInputLastMsgByDeviceName(name);
		DWORD lastOut = GetMidiOutputLastMsgByDeviceName(name);
		DWORD last    = (lastIn > lastOut) ? lastIn : lastOut;
		if (name == L"AhlbornBridge Virtual Port")
		{
			DWORD fwd = GetMidi2EndpointLastMsgTime();
			if (fwd > last) last = fwd;
		}
		if (bridgeEnabled && name == L"AhlbornBridge Virtual Port (B)")
		{
			DWORD mirrored = GetMidiOutputLastMsgByDeviceName(L"AhlbornBridge Virtual Port");
			if (mirrored > last)
				last = mirrored;
		}
		// Hauptwerk Virtual (A) and (B) share the same physical traffic:
		// Hauptwerk writes to (A) and the bridge reads from (B).
		// Both LEDs light up together using the (B) receive timestamp.
		if (name == L"Hauptwerk Virtual (A)" || name == L"Hauptwerk Virtual (B)")
		{
			DWORD hwTick = GetHauptwerkVirtualBLastMsgTime();
			if (hwTick > last) last = hwTick;
		}
		int imgIdx = (last != 0 && (now - last) < kActivityWindowMs) ? 1 : 0;
		LVITEMW lvi = {};
		lvi.mask = LVIF_IMAGE;
		lvi.iItem = i;
		lvi.iSubItem = 0;
		lvi.iImage = imgIdx;
		ListView_SetItem(hLv, &lvi);
	}
}

static void SyncOutputAvailabilityFromCurrentUi()
{
	if (!g_assignHwnd || !IsWindow(g_assignHwnd)) return;

	HWND hAvailOut  = GetDlgItem(g_assignHwnd, IDC_TV_AVAIL_OUT);
	HWND hAssignIn  = GetDlgItem(g_assignHwnd, IDC_TV_ASSIGN_IN);
	HWND hAssignOut = GetDlgItem(g_assignHwnd, IDC_TV_ASSIGN_OUT);
	if (!hAvailOut || !hAssignIn || !hAssignOut) return;

	auto assignedInputs  = LvCollectNames(hAssignIn, true);
	auto assignedOutputs = CollectAssignedOutputNames(hAssignOut);
	PopulateAvailableOutputView(hAvailOut, assignedOutputs, assignedInputs);
}

static void UpdateApplyButtonState(HWND hWnd)
{
	HWND hAssignIn  = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
	HWND hAssignOut = GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT);
	HWND hApply     = GetDlgItem(hWnd, IDC_BTN_APPLY);
	HWND hCancel    = GetDlgItem(hWnd, IDC_BTN_CANCEL);
	if (!hAssignIn || !hAssignOut || !hApply || !hCancel) return;

	auto currentInputs  = CollectAssignedInputNames(hAssignIn);
	auto currentOutputs = CollectPersistedOutputNames(hAssignOut);
	bool hasChanges = currentInputs != g_initialAssignedInputs
		|| currentOutputs != g_initialAssignedOutputs;
	EnableWindow(hApply, hasChanges ? TRUE : FALSE);
	SetWindowTextW(hCancel, hasChanges ? L"Cancel" : L"Close");
}

static void ApplyCurrentInputAssignments(HWND hWnd)
{
	HWND hAssignIn = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
	if (!hAssignIn) return;

	auto inputs = CollectAssignedInputNames(hAssignIn);
	auto outputs = LoadAssignedMidiOutputNames();
	SaveAssignedMidiInputNames(inputs);
	SetAssignedMidiInputNames(inputs);
	WriteHauptwerkMidiConfig(inputs, outputs);
	g_initialAssignedInputs = inputs;
	UpdateApplyButtonState(hWnd);
	SyncOutputAvailabilityFromCurrentUi();
}

static void ApplyCurrentOutputAssignments(HWND hWnd)
{
	HWND hAssignIn = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
	HWND hAssignOut = GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT);
	if (!hAssignIn || !hAssignOut) return;

	if (IsProcessRunningByName(L"Hauptwerk.exe") || IsProcessRunningByName(L"hauptwerk.exe"))
	{
		MessageBoxW(hWnd, 
			L"Hauptwerk is currently running!\n\nPlease close Hauptwerk before changing and applying MIDI output port assignments. Otherwise, Hauptwerk will overwrite the configuration file upon closing.", 
			L"Hauptwerk Running", 
			MB_OK | MB_ICONWARNING);

		// Force refresh to restore previous state from Hauptwerk configuration
		PostMessageW(hWnd, kRefreshListsMsg, 0, 0);
		return;
	}

	auto inputs = CollectAssignedInputNames(hAssignIn);
	auto outputs = CollectPersistedOutputNames(hAssignOut);
	WriteHauptwerkMidiConfig(inputs, outputs);
	g_initialAssignedOutputs = outputs;
	UpdateApplyButtonState(hWnd);
	SyncOutputAvailabilityFromCurrentUi();
}

static void UpdateBridgeCheckboxText(HWND hWnd)
{
	HWND hCheck = GetDlgItem(hWnd, IDC_CHK_BRIDGE_ENABLED);
	if (!hCheck) return;
	SetWindowTextW(hCheck,
		g_midiRouterEnabled.load()
		? L"Bridge enabled (MIDI flows to Hauptwerk)"
		: L"Bridge disabled (MIDI flow interrupted)");
}

// ---------------------------------------------------------------------------
// Activity LED refresh
// ---------------------------------------------------------------------------
static void RefreshInputLeds(HWND hAssignIn)
{
	if (!g_ledImgList) return;
	int n = ListView_GetItemCount(hAssignIn);
	DWORD now = GetTickCount();
	for (int i = 0; i < n; ++i)
	{
		DWORD last = GetMidiInputSlotLastMsg(i);
		int imgIdx = (last != 0 && (now - last) < kActivityWindowMs) ? 1 : 0;
		LVITEMW lvi = {};
		lvi.mask     = LVIF_IMAGE;
		lvi.iItem    = i;
		lvi.iSubItem = 0;
		lvi.iImage   = imgIdx;
		ListView_SetItem(hAssignIn, &lvi);
	}
}

static void RefreshOutputLeds(HWND hAssignOut)
{
	if (!g_ledImgList) return;
	int n = ListView_GetItemCount(hAssignOut);
	DWORD now = GetTickCount();
	for (int i = 0; i < n; ++i)
	{
		// Output assigned devices are owned by Hauptwerk, not the bridge.
		// The bridge only opens AhlbornBridge Virtual Port as its output.
		// We have no activity timestamp for Hauptwerk-owned devices,
		// so all LEDs in this list are kept off to avoid misleading the user.
		int imgIdx = 0;
		LVITEMW lvi = {};
		lvi.mask     = LVIF_IMAGE;
		lvi.iItem    = i;
		lvi.iSubItem = 0;
		lvi.iImage   = imgIdx;
		ListView_SetItem(hAssignOut, &lvi);
	}
}

static HWND g_lvs[4] = {}; // avail_in, assign_in, avail_out, assign_out

static bool IsFixedAssignedOutputItem(HWND hLv, int index, bool hasLedCol)
{
	return hLv == g_lvs[3] && index >= 0 && IsFixedAssignedOutputName(LvGetItemName(hLv, index, hasLedCol));
}

static bool IsFixedAssignedInputItem(HWND hLv, int index, bool hasLedCol)
{
	return hLv == g_lvs[1] && index >= 0 && IsFixedAssignedInputName(LvGetItemName(hLv, index, hasLedCol));
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MidiAssignWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		HINSTANCE hInst = reinterpret_cast<CREATESTRUCT*>(lParam)->hInstance;

		const int margin     = 12;
		const int colW       = 190;
		const int tvH        = 160;
		const int btnW       = 72;
		const int btnH       = 24;
		const int arrowW     = 28;
		const int sectionGap = 24;

		// Helper: create one section (available list + arrow buttons + assigned list + up/down)
		// isInput=true adds LED column to the assigned list.
		auto makeSection = [&](int iy, UINT idAvail, UINT idAssign,
								UINT idAdd, UINT idRem, UINT idUp, UINT idDn,
								const wchar_t* label, bool isInput) -> int
		{
			CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFT,
				margin, iy, 400, 18, hWnd, nullptr, hInst, nullptr);
			iy += 22;

			int xA  = margin;
			int xAr = xA + colW + 8;
			int xB  = xAr + arrowW + 8;
			int xUd = xB + colW + 6;

			CreateWindowW(L"STATIC", L"Available", WS_CHILD | WS_VISIBLE | SS_CENTER,
				xA, iy, colW, 16, hWnd, nullptr, hInst, nullptr);
			CreateWindowW(L"STATIC", L"Assigned (order matters)", WS_CHILD | WS_VISIBLE | SS_CENTER,
				xB, iy, colW + 40, 16, hWnd, nullptr, hInst, nullptr);
			iy += 18;

			// Available: simple list mode
			HWND hAvail = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
				WS_CHILD | WS_VISIBLE | LVS_LIST | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
				xA, iy, colW, tvH, hWnd, (HMENU)(UINT_PTR)idAvail, hInst, nullptr);

			int ay = iy + tvH / 2 - btnH - 4;
			CreateWindowW(L"BUTTON", L">", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				xAr, ay,        arrowW, btnH, hWnd, (HMENU)(UINT_PTR)idAdd, hInst, nullptr);
			CreateWindowW(L"BUTTON", L"<", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				xAr, ay+btnH+6, arrowW, btnH, hWnd, (HMENU)(UINT_PTR)idRem, hInst, nullptr);

			// Assigned: report mode (with or without LED column)
			HWND hAssign = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
				WS_CHILD | WS_VISIBLE |
				LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
				xB, iy, colW, tvH, hWnd, (HMENU)(UINT_PTR)idAssign, hInst, nullptr);
			ListView_SetExtendedListViewStyleEx(hAssign,
				LVS_EX_FULLROWSELECT,
				LVS_EX_FULLROWSELECT);

			LvSetupReportMode(hAssign, isInput);

			CreateWindowW(L"BUTTON", L"\u25b2", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				xUd, iy,        arrowW, btnH, hWnd, (HMENU)(UINT_PTR)idUp, hInst, nullptr);
			CreateWindowW(L"BUTTON", L"\u25bc", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				xUd, iy+btnH+4, arrowW, btnH, hWnd, (HMENU)(UINT_PTR)idDn, hInst, nullptr);

			return iy + tvH;
		};

		EnsureLedImageList(hWnd);

		int iy = margin;
		iy = makeSection(iy, IDC_TV_AVAIL_IN,  IDC_TV_ASSIGN_IN,
						 IDC_BTN_ADD_IN, IDC_BTN_REM_IN, IDC_BTN_UP_IN, IDC_BTN_DOWN_IN,
						 L"AhlbornBridge MIDI Input Ports", true);
		iy += sectionGap;
		iy = makeSection(iy, IDC_TV_AVAIL_OUT, IDC_TV_ASSIGN_OUT,
					 IDC_BTN_ADD_OUT, IDC_BTN_REM_OUT, IDC_BTN_UP_OUT, IDC_BTN_DOWN_OUT,
					 L"Hauptwerk MIDI Output Ports", true);
		iy += sectionGap;

		int totalW = margin + colW + 8 + arrowW + 8 + colW + 6 + arrowW + margin;

		CreateWindowW(L"STATIC", L"Internal bridge ports", WS_CHILD | WS_VISIBLE | SS_LEFT,
			margin, iy, 400, 18, hWnd, nullptr, hInst, nullptr);
		iy += 22;
		HWND hInternal = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			margin, iy, totalW - margin * 2, kInternalBridgeSectionHeight, hWnd, (HMENU)(UINT_PTR)IDC_TV_INTERNAL, hInst, nullptr);
		ListView_SetExtendedListViewStyleEx(hInternal, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
		LvSetupReportMode(hInternal, true);
		iy += kInternalBridgeSectionHeight + sectionGap;

		HWND hBridgeCheck = CreateWindowW(L"BUTTON", L"",
			WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
			margin, iy, totalW - margin * 2, kBridgeToggleHeight,
			hWnd, (HMENU)IDC_CHK_BRIDGE_ENABLED, hInst, nullptr);
		Button_SetCheck(hBridgeCheck, g_midiRouterEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		UpdateBridgeCheckboxText(hWnd);
		iy += kBridgeToggleHeight + sectionGap;

		CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
			totalW - btnW*2 - 10, iy, btnW, btnH, hWnd, (HMENU)IDC_BTN_APPLY,  hInst, nullptr);
		CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			totalW - btnW - 4,    iy, btnW, btnH, hWnd, (HMENU)IDC_BTN_CANCEL, hInst, nullptr);

		g_lvs[0] = GetDlgItem(hWnd, IDC_TV_AVAIL_IN);
		g_lvs[1] = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
		g_lvs[2] = GetDlgItem(hWnd, IDC_TV_AVAIL_OUT);
		g_lvs[3] = GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT);
		g_internalBridgeLv = hInternal;
		auto assignedInputs = LoadAssignedMidiInputNames();
		g_fixedInputName = std::wstring{};
		g_fixedOutputName = std::wstring{};

		PopulateInputViews (g_lvs[0], g_lvs[1]);
		PopulateOutputViews(g_lvs[2], g_lvs[3]);
		PopulateInternalBridgePortsView(hInternal);
		g_initialAssignedInputs  = CollectAssignedInputNames(g_lvs[1]);
		g_initialAssignedOutputs = CollectPersistedOutputNames(g_lvs[3]);
		UpdateApplyButtonState(hWnd);

		SetTimer(hWnd, kActivityTimerId, kActivityIntervalMs, nullptr);
		return 0;
	}

	case WM_TIMER:
		if (wParam == kActivityTimerId)
		{
			RefreshInputLeds(GetDlgItem(hWnd, IDC_TV_ASSIGN_IN));
			RefreshOutputLeds(GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT));
			RefreshInternalBridgePortLeds(g_internalBridgeLv);
		}
		return 0;

	case WM_DEVICECHANGE:
		PostMessageW(hWnd, kRefreshListsMsg, 0, 0);
		return 0;

	case kRefreshListsMsg:
	{
		PruneDisconnectedAssignedDevices();
		HWND hAvailIn  = GetDlgItem(hWnd, IDC_TV_AVAIL_IN);
		HWND hAssignIn = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
		HWND hAvailOut = GetDlgItem(hWnd, IDC_TV_AVAIL_OUT);
		HWND hAssignOut= GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT);
		if (hAvailIn && hAssignIn)
			PopulateInputViews(hAvailIn, hAssignIn);
		if (hAvailOut && hAssignOut)
			PopulateOutputViews(hAvailOut, hAssignOut);
		if (g_internalBridgeLv)
			PopulateInternalBridgePortsView(g_internalBridgeLv);
		g_initialAssignedInputs  = CollectAssignedInputNames(hAssignIn);
		g_initialAssignedOutputs = CollectPersistedOutputNames(hAssignOut);
		UpdateApplyButtonState(hWnd);
		return 0;
	}

	case WM_COMMAND:
	{
		UINT id = LOWORD(wParam);
		HWND hAvailIn  = GetDlgItem(hWnd, IDC_TV_AVAIL_IN);
		HWND hAssignIn = GetDlgItem(hWnd, IDC_TV_ASSIGN_IN);
		HWND hAvailOut = GetDlgItem(hWnd, IDC_TV_AVAIL_OUT);
		HWND hAssignOut= GetDlgItem(hWnd, IDC_TV_ASSIGN_OUT);

		switch (id)
		{
		case IDC_BTN_ADD_IN:
			MoveSelected(hAvailIn, false, hAssignIn, true);
			ApplyCurrentInputAssignments(hWnd);
			return 0;
		case IDC_BTN_REM_IN:
		{
			MoveSelected(hAssignIn, true, hAvailIn, false);
			ApplyCurrentInputAssignments(hWnd);
			return 0;
		}
		case IDC_BTN_UP_IN:
		{
			LvMoveItem(hAssignIn, true, true);
			ApplyCurrentInputAssignments(hWnd);
			return 0;
		}
		case IDC_BTN_DOWN_IN:
		{
			LvMoveItem(hAssignIn, false, true);
			ApplyCurrentInputAssignments(hWnd);
			return 0;
		}
		case IDC_BTN_ADD_OUT:
			MoveSelected(hAvailOut, false, hAssignOut, true);
			ApplyCurrentOutputAssignments(hWnd);
			return 0;
		case IDC_BTN_REM_OUT:
		{
			MoveSelected(hAssignOut, true, hAvailOut, false);
			ApplyCurrentOutputAssignments(hWnd);
			return 0;
		}
		case IDC_BTN_UP_OUT:
		{
			LvMoveItem(hAssignOut, true, true);
			ApplyCurrentOutputAssignments(hWnd);
			return 0;
		}
		case IDC_BTN_DOWN_OUT:
		{
			LvMoveItem(hAssignOut, false, true);
			ApplyCurrentOutputAssignments(hWnd);
			return 0;
		}
		case IDC_CHK_BRIDGE_ENABLED:
		{
			bool enabled = Button_GetCheck(reinterpret_cast<HWND>(lParam)) == BST_CHECKED;
			g_midiRouterEnabled = enabled;
			SaveMidiRouterEnabled(enabled);
			UpdateBridgeCheckboxText(hWnd);
			RefreshInternalBridgePortLeds(g_internalBridgeLv);
			return 0;
		}
		case IDC_BTN_APPLY:
		{
			auto inputs  = CollectAssignedInputNames(hAssignIn);
			auto outputs = CollectPersistedOutputNames(hAssignOut);

			bool outputsChanged = (outputs != g_initialAssignedOutputs);
			if (outputsChanged)
			{
				if (IsProcessRunningByName(L"Hauptwerk.exe") || IsProcessRunningByName(L"hauptwerk.exe"))
				{
					MessageBoxW(hWnd, 
						L"Hauptwerk is currently running!\n\nPlease close Hauptwerk before changing and applying MIDI output port assignments. Otherwise, Hauptwerk will overwrite the configuration file upon closing.", 
						L"Hauptwerk Running", 
						MB_OK | MB_ICONWARNING);
					return 0;
				}
			}

			SaveAssignedMidiInputNames(inputs);
			SetAssignedMidiInputNames(inputs);
			WriteHauptwerkMidiConfig(inputs, outputs);
			g_initialAssignedInputs  = inputs;
			g_initialAssignedOutputs = outputs;
			UpdateApplyButtonState(hWnd);
			return 0;
		}
		case IDC_BTN_CANCEL:
			DestroyWindow(hWnd);
			return 0;
		}
		break;
	}

	case WM_DESTROY:
		KillTimer(hWnd, kActivityTimerId);
		g_initialAssignedInputs.clear();
		g_initialAssignedOutputs.clear();
		g_internalBridgeLv = nullptr;
		g_fixedInputName.clear();
		g_fixedOutputName.clear();
		for (int i = 0; i < 4; ++i)
		{
			g_lvs[i] = nullptr;
		}
		if (g_ledImgList) { ImageList_Destroy(g_ledImgList); g_ledImgList = nullptr; }
		g_assignHwnd = nullptr;
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
void RefreshMidiAssignmentWindowIfOpen()
{
	if (g_assignHwnd && IsWindow(g_assignHwnd))
		PostMessageW(g_assignHwnd, kRefreshListsMsg, 0, 0);
}

void ShowMidiAssignmentWindow(HINSTANCE hInstance, HWND hOwner)
{
	if (g_assignHwnd && IsWindow(g_assignHwnd))
	{
		SetForegroundWindow(g_assignHwnd);
		return;
	}

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSW wc = {};
	if (!GetClassInfoW(hInstance, kMidiAssignClassName, &wc))
	{
		wc.lpfnWndProc   = MidiAssignWndProc;
		wc.hInstance     = hInstance;
		wc.lpszClassName = kMidiAssignClassName;
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassW(&wc);
	}

	const int tvH = 160, sectionGap = 24, btnH = 24, margin = 12, internalH = kInternalBridgeSectionHeight;
	const int colW = 190, arrowW = 28;
	const int headerPerSection = 22 + 18;
	int clientH = margin
				+ 2 * (headerPerSection + tvH)
				+ 4 * sectionGap
				+ 22 + internalH
				+ kBridgeToggleHeight
				+ btnH + margin + 8;
	int clientW = margin + colW + 8 + arrowW + 8 + colW + 6 + arrowW + margin + 8;

	int winH = clientH + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYBORDER)*2 + 20;
	int winW = clientW + GetSystemMetrics(SM_CXBORDER)*2 + 8;

	g_assignHwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW,
		kMidiAssignClassName,
		L"MIDI Device Assignment",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT,
		winW, winH,
		hOwner, nullptr, hInstance, nullptr);

	if (g_assignHwnd)
	{
		ShowWindow(g_assignHwnd, SW_SHOW);
		UpdateWindow(g_assignHwnd);
	}
}
