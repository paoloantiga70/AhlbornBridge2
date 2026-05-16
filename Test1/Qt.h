#pragma once

#define mUNLOAD_ORGAN L"Organ", L"Unload organ"
#define mLOAD_FAVORITE_ORGAN_1 L"Organ", L"Load favorite organ", L"1: "
#define mLOAD_FAVORITE_ORGAN_2 L"Organ", L"Load favorite organ", L"2: "
#define mLOAD_FAVORITE_ORGAN_3 L"Organ", L"Load favorite organ", L"3: "
#define mLOAD_FAVORITE_ORGAN_4 L"Organ", L"Load favorite organ", L"4: "
#define mLOAD_FAVORITE_ORGAN_5 L"Organ", L"Load favorite organ", L"5: "
#define mLOAD_FAVORITE_ORGAN_6 L"Organ", L"Load favorite organ", L"6: "
#define mLOAD_FAVORITE_ORGAN_7 L"Organ", L"Load favorite organ", L"7: "
#define mLOAD_FAVORITE_ORGAN_8 L"Organ", L"Load favorite organ", L"8: "

#include <windows.h>
#include <oleacc.h>
#include <vector>
#include <iostream>

// Trova la finestra reale di Hauptwerk (QWidget con titolo)
HWND FindHauptwerkRealWindow();

// Trova un IAccessible figlio per ruolo e nome (ricorsivo)
IAccessible* FindChildByRoleAndName(
    IAccessible* parent,
    long targetRole,
    const wchar_t* targetName,
    int maxDepth=3,
    int depth=0);

// Enumera e stampa i figli di un IAccessible (un livello)
void ListChildren(IAccessible* parent);

// Invoca accDoDefaultAction su un MenuItem trovato per nome
bool InvokeMenuItemByName(IAccessible* parent, const wchar_t* itemName, bool prefixMatch = false);

// Cerca il popup menu Qt piu' recente
HWND FindPopupHwnd();

// Apre un popup menu e invoca una voce al suo interno
bool FindAndInvokeInPopup(HWND hwndReal, const wchar_t* menuName, const wchar_t* itemName);

// Naviga: Menu -> Sottomenu -> Voce (2 livelli)
bool ClickMenu(HWND hwndReal, const wchar_t* menuName, const wchar_t* subItemName);

// Chiude i menu popup aperti inviando Escape
void DismissMenus(HWND hwnd);

// Naviga un percorso di menu a profondita' arbitraria
bool ClickMenuPath(HWND hwndReal, const std::vector<const wchar_t*>& path);

// Apre la finestra "Load organ ..." e seleziona l'organo per nome
bool LoadOrganByName(HWND hwndReal, const std::wstring& organName);
