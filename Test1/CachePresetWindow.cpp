// CachePresetWindow.cpp
// Hauptwerk organ cache preset manager.
// Cache files are several GB, so presets are created by RENAMING the active
// cache file (OrganIDnnnnnn.Cache_Hauptwerk_hbb) to a numbered preset name
// (OrganIDnnnnnn.PresetNN.Cache_Hauptwerk_hbb). Restoring swaps the files back.
// When the targeted organ is currently loaded, the window asks for confirmation,
// then automatically unloads it (and reloads it after a restore).

#include "CachePresetWindow.h"
#include "Xml.h"
#include "Midi.h"

#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

static const wchar_t kCachePresetClassName[] = L"AhlbornCachePresetWindow";

enum
{
	IDC_ORGAN_COMBO = 3101,
	IDC_PRESET_LIST = 3102,
	IDC_BTN_SAVE    = 3103,
	IDC_BTN_LOAD    = 3104,
	IDC_BTN_RENAME  = 3105,
	IDC_BTN_DELETE  = 3106,
	IDC_BTN_REFRESH = 3107,
	IDC_BTN_CLOSE   = 3108,
	IDC_STATUS      = 3109,
};

static HWND g_cacheHwnd = nullptr;
static HWND g_organCombo = nullptr;
static HWND g_presetList = nullptr;
static HWND g_statusLabel = nullptr;
static std::vector<InstalledOrganInfo> g_organs;

// --------- Simple text-input dialog for the descriptive preset name ----------
struct NamePromptData
{
	const wchar_t* prompt;
	std::wstring   value;
	bool           ok;
};

static INT_PTR CALLBACK NamePromptProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static NamePromptData* s_data = nullptr;
	switch (msg)
	{
	case WM_INITDIALOG:
		s_data = reinterpret_cast<NamePromptData*>(lParam);
		SetWindowTextW(hDlg, L"Preset name");
		SetDlgItemTextW(hDlg, 1001, s_data->prompt);
		SetDlgItemTextW(hDlg, 1002, s_data->value.c_str());
		SendDlgItemMessageW(hDlg, 1002, EM_SETSEL, 0, -1);
		SetFocus(GetDlgItem(hDlg, 1002));
		return FALSE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK)
		{
			wchar_t buf[256] = {};
			GetDlgItemTextW(hDlg, 1002, buf, 255);
			if (s_data) { s_data->value = buf; s_data->ok = true; }
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		if (LOWORD(wParam) == IDCANCEL)
		{
			if (s_data) s_data->ok = false;
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

// Builds an in-memory dialog template so we don't need an .rc resource.
static bool PromptForName(HWND owner, const wchar_t* prompt, std::wstring& value)
{
	// DLGTEMPLATE must be DWORD aligned; build it in a buffer.
	std::vector<BYTE> buffer(1024, 0);
	BYTE* p = buffer.data();

	auto align = [&](BYTE*& ptr) { ptr = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(ptr) + 3) & ~static_cast<ULONG_PTR>(3)); };
	auto writeWord = [&](BYTE*& ptr, WORD w) { *reinterpret_cast<WORD*>(ptr) = w; ptr += sizeof(WORD); };
	auto writeDword = [&](BYTE*& ptr, DWORD d) { *reinterpret_cast<DWORD*>(ptr) = d; ptr += sizeof(DWORD); };
	auto writeStr = [&](BYTE*& ptr, const wchar_t* s) { while (*s) { writeWord(ptr, *s++); } writeWord(ptr, 0); };

	DLGTEMPLATE* dlg = reinterpret_cast<DLGTEMPLATE*>(p);
	dlg->style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
	dlg->dwExtendedStyle = 0;
	dlg->cdit = 4;
	dlg->x = 0; dlg->y = 0; dlg->cx = 220; dlg->cy = 80;
	p += sizeof(DLGTEMPLATE);
	writeWord(p, 0); // no menu
	writeWord(p, 0); // default class
	writeStr(p, L"Preset name"); // title
	writeWord(p, 8); // font size
	writeStr(p, L"MS Shell Dlg");

	// Control 1: static label
	align(p);
	{ DLGITEMTEMPLATE* it = reinterpret_cast<DLGITEMTEMPLATE*>(p); it->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
	  it->dwExtendedStyle = 0; it->x = 8; it->y = 8; it->cx = 204; it->cy = 10; it->id = 1001; }
	p += sizeof(DLGITEMTEMPLATE);
	writeWord(p, 0xFFFF); writeWord(p, 0x0082); // STATIC
	writeStr(p, L""); writeWord(p, 0);

	// Control 2: edit box
	align(p);
	{ DLGITEMTEMPLATE* it = reinterpret_cast<DLGITEMTEMPLATE*>(p); it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
	  it->dwExtendedStyle = 0; it->x = 8; it->y = 22; it->cx = 204; it->cy = 14; it->id = 1002; }
	p += sizeof(DLGITEMTEMPLATE);
	writeWord(p, 0xFFFF); writeWord(p, 0x0081); // EDIT
	writeStr(p, L""); writeWord(p, 0);

	// Control 3: OK button
	align(p);
	{ DLGITEMTEMPLATE* it = reinterpret_cast<DLGITEMTEMPLATE*>(p); it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
	  it->dwExtendedStyle = 0; it->x = 104; it->y = 50; it->cx = 50; it->cy = 16; it->id = IDOK; }
	p += sizeof(DLGITEMTEMPLATE);
	writeWord(p, 0xFFFF); writeWord(p, 0x0080); // BUTTON
	writeStr(p, L"OK"); writeWord(p, 0);

	// Control 4: Cancel button
	align(p);
	{ DLGITEMTEMPLATE* it = reinterpret_cast<DLGITEMTEMPLATE*>(p); it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
	  it->dwExtendedStyle = 0; it->x = 160; it->y = 50; it->cx = 50; it->cy = 16; it->id = IDCANCEL; }
	p += sizeof(DLGITEMTEMPLATE);
	writeWord(p, 0xFFFF); writeWord(p, 0x0080); // BUTTON
	writeStr(p, L"Cancel"); writeWord(p, 0);

	NamePromptData data{ prompt, value, false };
	INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dlg, owner, NamePromptProc,
										reinterpret_cast<LPARAM>(&data));
	if (r == IDOK && data.ok)
	{
		value = data.value;
		return true;
	}
	return false;
}

static void SetStatus(const std::wstring& text)
{
	if (g_statusLabel)
		SetWindowTextW(g_statusLabel, text.c_str());
}

static int SelectedOrganIndex()
{
	if (!g_organCombo)
		return -1;
	return static_cast<int>(SendMessageW(g_organCombo, CB_GETCURSEL, 0, 0));
}

static int SelectedPresetNumber()
{
	if (!g_presetList)
		return -1;
	int sel = ListView_GetNextItem(g_presetList, -1, LVNI_SELECTED);
	if (sel < 0)
		return -1;
	LVITEMW item = {};
	item.mask = LVIF_PARAM;
	item.iItem = sel;
	if (!ListView_GetItem(g_presetList, &item))
		return -1;
	return static_cast<int>(item.lParam);
}

static void RefreshPresetList()
{
	if (!g_presetList)
		return;
	ListView_DeleteAllItems(g_presetList);

	int organSel = SelectedOrganIndex();
	if (organSel < 0 || organSel >= static_cast<int>(g_organs.size()))
		return;

	const std::wstring& uid = g_organs[organSel].uniqueOrganId;
	bool activeExists = false;
	std::vector<HauptwerkCachePreset> presets = EnumerateCachePresets(uid, activeExists);

	int row = 0;

	// First row: the active cache (the unsuffixed file the organ actually uses).
	// It is NOT a preset, so it carries lParam = 0 and cannot be loaded/renamed/deleted.
	if (activeExists)
	{
		std::wstring activeLabel = L"\u25CF Active cache";
		LVITEMW aitem = {};
		aitem.mask = LVIF_TEXT | LVIF_PARAM;
		aitem.iItem = row;
		aitem.lParam = 0;
		aitem.pszText = const_cast<LPWSTR>(activeLabel.c_str());
		ListView_InsertItem(g_presetList, &aitem);
		std::wstring activeName = GetActiveCacheName(uid);
		if (activeName.empty())
			activeName = L"(currently used by the organ)";
		else
			activeName += L" (currently used by the organ)";
		ListView_SetItemText(g_presetList, row, 1, const_cast<LPWSTR>(activeName.c_str()));
		ListView_SetItemText(g_presetList, row, 2, const_cast<LPWSTR>(L"\u2013"));
		++row;
	}

	for (const auto& preset : presets)
	{
		std::wstring numStr = std::to_wstring(preset.number);
		LVITEMW item = {};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = row;
		item.lParam = preset.number;
		item.pszText = const_cast<LPWSTR>(numStr.c_str());
		ListView_InsertItem(g_presetList, &item);
		std::wstring displayName = preset.name.empty() ? L"(no name \u2013 use Rename)" : preset.name;
		std::wstring displayDate = preset.date.empty() ? L"\u2013" : preset.date;
		ListView_SetItemText(g_presetList, row, 1, const_cast<LPWSTR>(displayName.c_str()));
		ListView_SetItemText(g_presetList, row, 2, const_cast<LPWSTR>(displayDate.c_str()));
		++row;
	}

	std::wstring status = activeExists
		? L"Active cache present. "
		: L"No active cache (will be generated on next load). ";
	status += std::to_wstring(presets.size()) + L" preset(s).";
	SetStatus(status);
}

// Returns true if the given organ (by installed index, 1-based) is the one
// currently loaded in Hauptwerk.
static bool IsOrganCurrentlyLoaded(int installedIndex1Based)
{
	return g_currentLoadedInstalledOrganIndex.load() == installedIndex1Based;
}

// If the organ is loaded, ask for confirmation and unload it, waiting until the
// unload completes. Returns false if the user declined or the unload failed.
static bool EnsureOrganUnloaded(int installedIndex1Based, const wchar_t* actionDescription)
{
	if (!IsOrganCurrentlyLoaded(installedIndex1Based))
		return true;

	std::wstring msg = L"This organ is currently loaded in Hauptwerk.\n\n";
	msg += actionDescription;
	msg += L"\n\nThe organ must be unloaded first. Unload it now?";
	if (MessageBoxW(g_cacheHwnd, msg.c_str(), L"Cache Presets",
					MB_ICONQUESTION | MB_YESNO) != IDYES)
		return false;

	SetStatus(L"Unloading organ...");
	EnqueueUnloadOrgan();

	// Wait (bounded) for the deferred unload to finish while pumping messages.
	for (int i = 0; i < 200 && g_currentLoadedInstalledOrganIndex.load() == installedIndex1Based; ++i)
	{
		MSG m;
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		Sleep(50);
	}

	if (g_currentLoadedInstalledOrganIndex.load() == installedIndex1Based)
	{
		SetStatus(L"Timed out waiting for the organ to unload.");
		return false;
	}
	return true;
}

static void DoSavePreset()
{
	int organSel = SelectedOrganIndex();
	if (organSel < 0 || organSel >= static_cast<int>(g_organs.size()))
		return;
	const std::wstring& uid = g_organs[organSel].uniqueOrganId;

	if (!EnsureOrganUnloaded(organSel + 1, L"Saving a preset will archive the current active cache."))
		return;

	std::wstring name = L"Preset " + std::to_wstring(FindNextFreeCachePresetNumber(uid));
	if (!PromptForName(g_cacheHwnd, L"Enter a descriptive name for this preset:", name))
		return;

	int number = 0;
	std::wstring err;
	if (SaveActiveCacheAsPreset(uid, name, number, err))
	{
		SetStatus(L"Saved as Preset" + std::to_wstring(number) + L".");
		RefreshPresetList();
	}
	else
	{
		MessageBoxW(g_cacheHwnd, err.c_str(), L"Save preset failed", MB_ICONWARNING | MB_OK);
		SetStatus(L"Save failed: " + err);
	}
}

static void DoLoadPreset()
{
	int organSel = SelectedOrganIndex();
	if (organSel < 0 || organSel >= static_cast<int>(g_organs.size()))
		return;
	int presetNumber = SelectedPresetNumber();
	if (presetNumber <= 0)
	{
		SetStatus(L"Select a preset to load (not the active cache).");
		return;
	}
	const std::wstring& uid = g_organs[organSel].uniqueOrganId;

	bool wasLoaded = IsOrganCurrentlyLoaded(organSel + 1);
	if (!EnsureOrganUnloaded(organSel + 1,
			L"Loading a preset will archive the current active cache and promote the selected preset."))
		return;

	std::wstring err;
	bool biduleProfileRestored = false;
	if (RestoreCachePreset(uid, presetNumber, err, biduleProfileRestored))
	{
		SetStatus(L"Preset restored as the active cache.");
		RefreshPresetList();
		if (wasLoaded)
		{
			// If the preset has no associated Bidule profile, the reload below will
			// fall back to Default.bidule and show the usual tray balloon prompt.
			SetStatus(L"Preset restored. Reloading organ...");
			EnqueueLoadInstalledOrgan(organSel + 1);
		}
	}
	else
	{
		MessageBoxW(g_cacheHwnd, err.c_str(), L"Load preset failed", MB_ICONWARNING | MB_OK);
		SetStatus(L"Load failed: " + err);
	}
}

static void DoRenamePreset()
{
	int organSel = SelectedOrganIndex();
	if (organSel < 0 || organSel >= static_cast<int>(g_organs.size()))
		return;
	int presetNumber = SelectedPresetNumber();
	if (presetNumber <= 0)
	{
		SetStatus(L"Select a preset to rename (not the active cache).");
		return;
	}
	const std::wstring& uid = g_organs[organSel].uniqueOrganId;

	bool activeExists = false;
	std::wstring current;
	for (const auto& p : EnumerateCachePresets(uid, activeExists))
		if (p.number == presetNumber) { current = p.name; break; }

	if (!PromptForName(g_cacheHwnd, L"Enter a new name for this preset:", current))
		return;

	RenameCachePreset(uid, presetNumber, current);
	RefreshPresetList();
	SetStatus(L"Preset renamed.");
}

static void DoDeletePreset()
{
	int organSel = SelectedOrganIndex();
	if (organSel < 0 || organSel >= static_cast<int>(g_organs.size()))
		return;
	int presetNumber = SelectedPresetNumber();
	if (presetNumber <= 0)
	{
		SetStatus(L"Select a preset to delete (not the active cache).");
		return;
	}
	const std::wstring& uid = g_organs[organSel].uniqueOrganId;

	if (MessageBoxW(g_cacheHwnd,
			L"Delete the selected preset cache file permanently?\n\nThis cannot be undone.",
			L"Delete preset", MB_ICONWARNING | MB_YESNO) != IDYES)
		return;

	std::wstring err;
	if (DeleteCachePreset(uid, presetNumber, err))
	{
		RefreshPresetList();
		SetStatus(L"Preset deleted.");
	}
	else
	{
		MessageBoxW(g_cacheHwnd, err.c_str(), L"Delete preset failed", MB_ICONWARNING | MB_OK);
		SetStatus(L"Delete failed: " + err);
	}
}

static void PopulateOrganCombo()
{
	g_organs = LoadInstalledOrganInfos();
	SendMessageW(g_organCombo, CB_RESETCONTENT, 0, 0);
	for (const auto& organ : g_organs)
	{
		std::wstring label = organ.name.empty() ? organ.displayName : organ.name;
		if (label.empty())
			label = L"(organ " + organ.uniqueOrganId + L")";
		SendMessageW(g_organCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
	}

	// Preselect the currently loaded organ, if any.
	int loaded = g_currentLoadedInstalledOrganIndex.load();
	int preselect = 0;
	if (loaded >= 1 && loaded <= static_cast<int>(g_organs.size()))
		preselect = loaded - 1;
	if (!g_organs.empty())
		SendMessageW(g_organCombo, CB_SETCURSEL, preselect, 0);
}

static LRESULT CALLBACK CachePresetWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		CreateWindowW(L"STATIC", L"Organ:", WS_CHILD | WS_VISIBLE,
			12, 14, 50, 18, hWnd, nullptr, nullptr, nullptr);
		g_organCombo = CreateWindowW(L"COMBOBOX", nullptr,
			WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
			66, 10, 380, 300, hWnd, (HMENU)IDC_ORGAN_COMBO, nullptr, nullptr);

		g_presetList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			12, 44, 434, 230, hWnd, (HMENU)IDC_PRESET_LIST, nullptr, nullptr);
		ListView_SetExtendedListViewStyle(g_presetList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		col.pszText = const_cast<LPWSTR>(L"#");    col.cx = 40;  ListView_InsertColumn(g_presetList, 0, &col);
		col.pszText = const_cast<LPWSTR>(L"Name"); col.cx = 250; ListView_InsertColumn(g_presetList, 1, &col);
		col.pszText = const_cast<LPWSTR>(L"Date"); col.cx = 130; ListView_InsertColumn(g_presetList, 2, &col);

		int bx = 456, bw = 130, bh = 26, by = 44, gap = 8;
		CreateWindowW(L"BUTTON", L"Save preset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_SAVE, nullptr, nullptr); by += bh + gap;
		CreateWindowW(L"BUTTON", L"Load preset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_LOAD, nullptr, nullptr); by += bh + gap;
		CreateWindowW(L"BUTTON", L"Rename", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_RENAME, nullptr, nullptr); by += bh + gap;
		CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_DELETE, nullptr, nullptr); by += bh + gap;
		CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_REFRESH, nullptr, nullptr); by += bh + gap;
		CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			bx, by, bw, bh, hWnd, (HMENU)IDC_BTN_CLOSE, nullptr, nullptr);

		g_statusLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS,
			12, 282, 574, 18, hWnd, (HMENU)IDC_STATUS, nullptr, nullptr);

		PopulateOrganCombo();
		RefreshPresetList();
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_ORGAN_COMBO:
			if (HIWORD(wParam) == CBN_SELCHANGE)
				RefreshPresetList();
			return 0;
		case IDC_BTN_SAVE:    DoSavePreset();   return 0;
		case IDC_BTN_LOAD:    DoLoadPreset();   return 0;
		case IDC_BTN_RENAME:  DoRenamePreset(); return 0;
		case IDC_BTN_DELETE:  DoDeletePreset(); return 0;
		case IDC_BTN_REFRESH: PopulateOrganCombo(); RefreshPresetList(); return 0;
		case IDC_BTN_CLOSE:   DestroyWindow(hWnd); return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;
	case WM_DESTROY:
		g_cacheHwnd = nullptr;
		g_organCombo = nullptr;
		g_presetList = nullptr;
		g_statusLabel = nullptr;
		g_organs.clear();
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowCachePresetWindow(HINSTANCE hInstance, HWND hOwner)
{
	if (g_cacheHwnd && IsWindow(g_cacheHwnd))
	{
		SetForegroundWindow(g_cacheHwnd);
		return;
	}

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSW wc = {};
	if (!GetClassInfoW(hInstance, kCachePresetClassName, &wc))
	{
		wc.lpfnWndProc = CachePresetWndProc;
		wc.hInstance = hInstance;
		wc.lpszClassName = kCachePresetClassName;
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassW(&wc);
	}

	int clientW = 600, clientH = 312;
	int winW = clientW + GetSystemMetrics(SM_CXBORDER) * 2 + 8;
	int winH = clientH + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYBORDER) * 2 + 20;

	g_cacheHwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW,
		kCachePresetClassName,
		L"Hauptwerk Cache Presets",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT,
		winW, winH,
		hOwner, nullptr, hInstance, nullptr);

	if (g_cacheHwnd)
	{
		ShowWindow(g_cacheHwnd, SW_SHOW);
		UpdateWindow(g_cacheHwnd);
	}
}
