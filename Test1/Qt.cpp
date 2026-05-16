#include <windows.h>
#include <oleacc.h>
#include <iostream>
#include <string>
#include <vector>
#include "Qt.h"
#include "Hauptwerk.h"
#include "StreamDeck.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Oleacc.lib")

// ============================================================
// Trova la finestra reale di Hauptwerk (QWidget con titolo)
// La finestra wrapper (HWND principale) non contiene i menu.
// La finestra Qt reale e' una top-level QWidget visibile con
// titolo contenente "Hauptwerk".
// ============================================================
HWND FindHauptwerkRealWindow()
{
    struct SearchData {
        HWND result;
    } data = { nullptr };

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;

        wchar_t cls[256] = {};
        GetClassNameW(h, cls, 256);

        // La finestra Qt reale ha classe "QWidget"
        if (wcsstr(cls, L"QWidget") == nullptr)
            return TRUE;

        // Verifica via MSAA se ha un MenuBar con figli
        IAccessible* pAcc = nullptr;
        HRESULT hr = AccessibleObjectFromWindow(
            h, OBJID_CLIENT, IID_IAccessible, (void**)&pAcc);
        if (FAILED(hr) || !pAcc)
            return TRUE;

        long childCount = 0;
        pAcc->get_accChildCount(&childCount);

        // La finestra reale di Hauptwerk ha 69 figli (menu + toolbar + ...)
        if (childCount > 10)
        {
            reinterpret_cast<SearchData*>(lp)->result = h;
            pAcc->Release();
            return FALSE;
        }

        pAcc->Release();
        return TRUE;
        }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}

// ============================================================
// Trova un IAccessible figlio per ruolo e nome (ricorsivo)
// ============================================================
IAccessible* FindChildByRoleAndName(
    IAccessible* parent,
    long targetRole,
    const wchar_t* targetName,
    int maxDepth ,
    int depth )
{
    if (!parent || depth > maxDepth)
        return nullptr;

    long childCount = 0;
    parent->get_accChildCount(&childCount);
    if (childCount <= 0 || childCount > 500)
        return nullptr;

    std::vector<VARIANT> children(childCount);
    for (auto& v : children) VariantInit(&v);

    long obtained = 0;
    HRESULT hr = AccessibleChildren(parent, 0, childCount, children.data(), &obtained);
    if (FAILED(hr))
        return nullptr;

    for (long i = 0; i < obtained; i++)
    {
        if (children[i].vt == VT_DISPATCH && children[i].pdispVal)
        {
            IAccessible* childAcc = nullptr;
            children[i].pdispVal->QueryInterface(IID_IAccessible, (void**)&childAcc);
            if (childAcc)
            {
                VARIANT varSelf;
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                VARIANT varRole;
                VariantInit(&varRole);
                childAcc->get_accRole(varSelf, &varRole);
                long role = (varRole.vt == VT_I4) ? varRole.lVal : 0;
                VariantClear(&varRole);

                BSTR bstrName = nullptr;
                childAcc->get_accName(varSelf, &bstrName);

                bool roleMatch = (role == targetRole);
                bool nameMatch = false;
                if (targetName == nullptr)
                    nameMatch = true;
                else if (bstrName && _wcsicmp(bstrName, targetName) == 0)
                    nameMatch = true;

                SysFreeString(bstrName);

                if (roleMatch && nameMatch)
                {
                    // Trovato! Pulisci il resto e ritorna
                    for (long j = i + 1; j < obtained; j++)
                        VariantClear(&children[j]);
                    // Non rilasciare children[i] perche' stiamo ritornando childAcc
                    VariantClear(&children[i]); // rilascia il dispatch, teniamo childAcc
                    return childAcc;
                }

                // Cerca ricorsivamente
                IAccessible* found = FindChildByRoleAndName(
                    childAcc, targetRole, targetName, maxDepth, depth + 1);
                childAcc->Release();

                if (found)
                {
                    for (long j = i + 1; j < obtained; j++)
                        VariantClear(&children[j]);
                    return found;
                }
            }
        }
        VariantClear(&children[i]);
    }

    return nullptr;
}

// ============================================================
// Enumera e stampa i figli di un IAccessible (un livello)
// ============================================================
void ListChildren(IAccessible* parent)
{
    long childCount = 0;
    parent->get_accChildCount(&childCount);
    if (childCount <= 0) return;

    std::vector<VARIANT> children(childCount);
    for (auto& v : children) VariantInit(&v);

    long obtained = 0;
    AccessibleChildren(parent, 0, childCount, children.data(), &obtained);

    for (long i = 0; i < obtained; i++)
    {
        if (children[i].vt == VT_DISPATCH && children[i].pdispVal)
        {
            IAccessible* childAcc = nullptr;
            children[i].pdispVal->QueryInterface(IID_IAccessible, (void**)&childAcc);
            if (childAcc)
            {
                VARIANT varSelf;
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                BSTR name = nullptr;
                childAcc->get_accName(varSelf, &name);

                VARIANT varRole;
                VariantInit(&varRole);
                childAcc->get_accRole(varSelf, &varRole);
                long role = (varRole.vt == VT_I4) ? varRole.lVal : 0;
                VariantClear(&varRole);

                if (name && wcslen(name) > 0)
                {
                    const wchar_t* roleStr =
                        (role == ROLE_SYSTEM_MENUITEM) ? L"MenuItem" :
                        (role == ROLE_SYSTEM_MENUPOPUP) ? L"MenuPopup" :
                        (role == ROLE_SYSTEM_MENUBAR) ? L"MenuBar" :
                        (role == ROLE_SYSTEM_SEPARATOR) ? L"Separator" :
                        L"Other";

                    std::wcout << L"    [" << roleStr << L"] \"" << name << L"\"\n";
                }
                SysFreeString(name);
                childAcc->Release();
            }
        }
        else if (children[i].vt == VT_I4)
        {
            VARIANT varChild;
            varChild.vt = VT_I4;
            varChild.lVal = children[i].lVal;

            BSTR name = nullptr;
            parent->get_accName(varChild, &name);

            VARIANT cRole;
            VariantInit(&cRole);
            parent->get_accRole(varChild, &cRole);
            long role = (cRole.vt == VT_I4) ? cRole.lVal : 0;
            VariantClear(&cRole);

            if (name && wcslen(name) > 0)
            {
                const wchar_t* roleStr =
                    (role == ROLE_SYSTEM_MENUITEM) ? L"MenuItem" :
                    (role == ROLE_SYSTEM_SEPARATOR) ? L"---" :
                    L"Other";
                std::wcout << L"    [" << roleStr << L"] \"" << name << L"\"\n";
            }
            SysFreeString(name);
        }
        VariantClear(&children[i]);
    }
}

// ============================================================
// Invoca accDoDefaultAction su un MenuItem trovato per nome
// dentro un parent IAccessible
// ============================================================
bool InvokeMenuItemByName(IAccessible* parent, const wchar_t* itemName, bool prefixMatch)
{
    long childCount = 0;
    parent->get_accChildCount(&childCount);
    if (childCount <= 0) return false;

    std::vector<VARIANT> children(childCount);
    for (auto& v : children) VariantInit(&v);

    long obtained = 0;
    AccessibleChildren(parent, 0, childCount, children.data(), &obtained);

    for (long i = 0; i < obtained; i++)
    {
        if (children[i].vt == VT_DISPATCH && children[i].pdispVal)
        {
            IAccessible* childAcc = nullptr;
            children[i].pdispVal->QueryInterface(IID_IAccessible, (void**)&childAcc);
            if (childAcc)
            {
                VARIANT varSelf;
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                BSTR name = nullptr;
                childAcc->get_accName(varSelf, &name);

                bool match = false;
                if (name)
                {
                    if (prefixMatch)
                        match = (_wcsnicmp(name, itemName, wcslen(itemName)) == 0);
                    else
                        match = (_wcsicmp(name, itemName) == 0);
                }

                if (match)
                {
                    if (prefixMatch && _wcsicmp(name, itemName) == 0)
                    {
                        std::wcout << L"  -> No organ loaded in this slot\n";
                        SendUnloadOrganMidiMessage();
                        SysFreeString(name);
                        childAcc->Release();
                        for (long j = i + 1; j < obtained; j++)
                            VariantClear(&children[j]);
                        return false;
                    }

                    std::wcout << L"  -> Trovato \"" << name << L"\", eseguo accDoDefaultAction...\n";
                    HRESULT hr = childAcc->accDoDefaultAction(varSelf);
                    std::wcout << L"  -> Risultato: " << (SUCCEEDED(hr) ? L"OK" : L"FALLITO")
                        << L" (HRESULT=0x" << std::hex << hr << std::dec << L")\n";

                    SysFreeString(name);
                    childAcc->Release();
                    for (long j = i + 1; j < obtained; j++)
                        VariantClear(&children[j]);
                    return SUCCEEDED(hr);
                }
                SysFreeString(name);
                childAcc->Release();
            }
        }
        else if (children[i].vt == VT_I4)
        {
            VARIANT varChild;
            varChild.vt = VT_I4;
            varChild.lVal = children[i].lVal;

            BSTR name = nullptr;
            parent->get_accName(varChild, &name);

            bool match2 = false;
            if (name)
            {
                if (prefixMatch)
                    match2 = (_wcsnicmp(name, itemName, wcslen(itemName)) == 0);
                else
                    match2 = (_wcsicmp(name, itemName) == 0);
            }

            if (match2)
            {
                if (prefixMatch && _wcsicmp(name, itemName) == 0)
                {
                    std::wcout << L"  -> No organ loaded in this slot\n";
                    SendUnloadOrganMidiMessage();
                    SysFreeString(name);
                    for (long j = i + 1; j < obtained; j++)
                        VariantClear(&children[j]);
                    return false;
                }

                std::wcout << L"  -> Trovato \"" << name << L"\" (simple child), eseguo accDoDefaultAction...\n";
                HRESULT hr = parent->accDoDefaultAction(varChild);
                std::wcout << L"  -> Risultato: " << (SUCCEEDED(hr) ? L"OK" : L"FALLITO")
                    << L" (HRESULT=0x" << std::hex << hr << std::dec << L")\n";

                SysFreeString(name);
                for (long j = i + 1; j < obtained; j++)
                    VariantClear(&children[j]);
                return SUCCEEDED(hr);
            }
            SysFreeString(name);
        }
        VariantClear(&children[i]);
    }

    return false;
}

// ============================================================
// Cerca il popup menu Qt piu' recente (finestra top-level
// QWidget con ruolo ROLE_SYSTEM_MENUPOPUP)
// ============================================================
HWND FindPopupHwnd()
{
    struct PopupData { HWND popup; } pd = { nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;

        wchar_t cls[256] = {};
        GetClassNameW(h, cls, 256);
        if (wcsstr(cls, L"QWidget") == nullptr) return TRUE;

        IAccessible* pAcc = nullptr;
        HRESULT hr = AccessibleObjectFromWindow(
            h, OBJID_CLIENT, IID_IAccessible, (void**)&pAcc);
        if (FAILED(hr) || !pAcc) return TRUE;

        VARIANT varSelf;
        varSelf.vt = VT_I4;
        varSelf.lVal = CHILDID_SELF;
        VARIANT varRole;
        VariantInit(&varRole);
        pAcc->get_accRole(varSelf, &varRole);

        if (varRole.vt == VT_I4 && varRole.lVal == ROLE_SYSTEM_MENUPOPUP)
        {
            reinterpret_cast<PopupData*>(lp)->popup = h;
            VariantClear(&varRole);
            pAcc->Release();
            return FALSE;
        }

        VariantClear(&varRole);
        pAcc->Release();
        return TRUE;
        }, reinterpret_cast<LPARAM>(&pd));

    return pd.popup;
}

// ============================================================
// Apre un popup menu e invoca una voce al suo interno.
// Ritorna true se la voce e' stata trovata e invocata.
// ============================================================
bool FindAndInvokeInPopup(HWND hwndReal, const wchar_t* menuName, const wchar_t* itemName)
{
    // Cerca popup come finestra separata
    HWND popupHwnd = FindPopupHwnd();
    if (popupHwnd)
    {
        std::wcout << L"\n  Popup trovato (HWND=0x" << std::hex << (uintptr_t)popupHwnd
            << std::dec << L"). Voci disponibili:\n";

        IAccessible* popupAcc = nullptr;
        AccessibleObjectFromWindow(popupHwnd, OBJID_CLIENT, IID_IAccessible, (void**)&popupAcc);
        if (popupAcc)
        {
            ListChildren(popupAcc);
            std::wcout << L"\n  Cerco \"" << itemName << L"\"...\n";
            bool ok = InvokeMenuItemByName(popupAcc, itemName);
            popupAcc->Release();
            if (ok) return true;
        }
    }

    // Fallback: cerca il popup come figlio del MenuItem stesso
    std::wcout << L"\n  Fallback: cerco sottomenu come figlio del MenuItem...\n";

    IAccessible* pRoot = nullptr;
    AccessibleObjectFromWindow(hwndReal, OBJID_CLIENT, IID_IAccessible, (void**)&pRoot);
    if (pRoot)
    {
        IAccessible* menuItem = FindChildByRoleAndName(
            pRoot, ROLE_SYSTEM_MENUITEM, menuName, 3);
        if (menuItem)
        {
            IAccessible* subMenu = FindChildByRoleAndName(
                menuItem, ROLE_SYSTEM_MENUPOPUP, nullptr, 2);
            if (subMenu)
            {
                std::wcout << L"  Sottomenu trovato! Voci:\n";
                ListChildren(subMenu);
                std::wcout << L"\n  Cerco \"" << itemName << L"\"...\n";
                bool ok = InvokeMenuItemByName(subMenu, itemName);
                subMenu->Release();
                menuItem->Release();
                pRoot->Release();
                if (ok) return true;
            }
            else
            {
                menuItem->Release();
            }
        }
        pRoot->Release();
    }

    return false;
}

// ============================================================
// Naviga: Menu -> Sottomenu -> Voce (2 livelli, versione originale)
// Es: ClickMenu(hwnd, L"Organ", L"Unload organ")
// ============================================================
bool ClickMenu(HWND hwndReal, const wchar_t* menuName, const wchar_t* subItemName)
{
    if (!g_midiRouterEnabled)
    {
        
        return false;
	}
    
    IAccessible* pRoot = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(
        hwndReal, OBJID_CLIENT, IID_IAccessible, (void**)&pRoot);
    if (FAILED(hr) || !pRoot)
    {
        std::wcout << L"ERRORE: impossibile ottenere IAccessible dalla finestra Qt.\n";
        SendUnloadOrganMidiMessage();
        return false;
    }

    IAccessible* menuBar = FindChildByRoleAndName(pRoot, ROLE_SYSTEM_MENUBAR, nullptr, 2);
    pRoot->Release();

    if (!menuBar)
    {
        std::wcout << L"ERRORE: MenuBar non trovata.\n";
        return false;
    }

    long mbChildren = 0;
    menuBar->get_accChildCount(&mbChildren);
    if (mbChildren == 0)
    {
        menuBar->Release();
        std::wcout << L"ERRORE: MenuBar trovata ma senza figli.\n";
        return false;
    }

    std::wcout << L"\n[1] MenuBar trovata con " << mbChildren << L" voci. Cerco \"" << menuName << L"\"...\n";

    if (!InvokeMenuItemByName(menuBar, menuName))
    {
        menuBar->Release();
        std::wcout << L"ERRORE: voce \"" << menuName << L"\" non trovata nella MenuBar.\n";
        return false;
    }
    menuBar->Release();

    if (subItemName == nullptr)
    {
        std::wcout << L"[OK] Menu \"" << menuName << L"\" aperto.\n";
        return true;
    }

    Sleep(400);

    if (FindAndInvokeInPopup(hwndReal, menuName, subItemName))
    {
        std::wcout << L"\n[OK] \"" << menuName << L"\" -> \"" << subItemName << L"\" eseguito!\n";
        return true;
    }

    std::wcout << L"\nERRORE: impossibile trovare/invocare \"" << subItemName << L"\".\n";
    return false;
}

// ============================================================
// Chiude i menu popup aperti inviando Escape
// ============================================================
void DismissMenus(HWND hwnd)
{
    for (int i = 0; i < 5; i++)
    {
        PostMessage(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessage(hwnd, WM_KEYUP, VK_ESCAPE, 0);
        Sleep(50);
    }
}

// ============================================================
// Naviga un percorso di menu a profondita' arbitraria
// navigando l'albero MSAA (con fallback popup window).
//
// Es: ClickMenuPath(hwnd, { L"Engine", L"Advanced use", L"Stop audio/MIDI" })
//
// Strategia: trova ogni MenuItem nell'albero MSAA come figlio
// del MenuPopup precedente, invoca accDoDefaultAction, poi
// cerca il suo MenuPopup figlio per il livello successivo.
// ============================================================
bool ClickMenuPath(HWND hwndReal, const std::vector<const wchar_t*>& path)
{
    if (!g_midiRouterEnabled)
    {

        return false;
    }
    if (path.empty())
        return false;

    // Stampa percorso
    std::wcout << L"\n[Percorso] ";
    for (size_t k = 0; k < path.size(); k++)
    {
        if (k > 0) std::wcout << L" -> ";
        std::wcout << L"\"" << path[k] << L"\"";
    }
    std::wcout << std::endl;

    // Se solo 1 o 2 livelli, usa ClickMenu originale con fallback
    if (path.size() <= 2)
    {
        return ClickMenu(hwndReal,
            path[0],
            path.size() > 1 ? path[1] : nullptr);
    }

    // 3+ livelli: naviga l'albero MSAA passo per passo
    IAccessible* pRoot = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(
        hwndReal, OBJID_CLIENT, IID_IAccessible, (void**)&pRoot);
    if (FAILED(hr) || !pRoot)
    {
        std::wcout << L"ERRORE: impossibile ottenere IAccessible.\n";
        SendUnloadOrganMidiMessage();
        return false;
    }

    // Step 1: clicca la prima voce nella MenuBar
    IAccessible* menuBar = FindChildByRoleAndName(pRoot, ROLE_SYSTEM_MENUBAR, nullptr, 2);
    if (!menuBar)
    {
        pRoot->Release();
        std::wcout << L"ERRORE: MenuBar non trovata.\n";
        return false;
    }

    std::wcout << L"\n[1] Cerco \"" << path[0] << L"\" nella MenuBar...\n";
    if (!InvokeMenuItemByName(menuBar, path[0]))
    {
        menuBar->Release();
        pRoot->Release();
        std::wcout << L"ERRORE: voce \"" << path[0] << L"\" non trovata.\n";
        return false;
    }
    menuBar->Release();
    Sleep(400);

    // Step 2..N: naviga l'albero MSAA
    // Trova il MenuItem appena cliccato, poi il suo MenuPopup figlio
    IAccessible* currentParent = pRoot; // non rilasciare, lo gestiamo
    // pRoot verra' rilasciato alla fine

    for (size_t step = 1; step < path.size(); step++)
    {
        // Trova il MenuItem del livello precedente nell'albero
        IAccessible* menuItem = FindChildByRoleAndName(
            currentParent, ROLE_SYSTEM_MENUITEM, path[step - 1], 5);

        if (!menuItem)
        {
            std::wcout << L"ERRORE: MenuItem \"" << path[step - 1]
                << L"\" non trovato nell'albero MSAA.\n";
            DismissMenus(hwndReal);
            currentParent->Release();
            return false;
        }

        // Trova il MenuPopup figlio di questo MenuItem
        IAccessible* subPopup = FindChildByRoleAndName(
            menuItem, ROLE_SYSTEM_MENUPOPUP, nullptr, 2);
        menuItem->Release();

        if (!subPopup)
        {
            std::wcout << L"ERRORE: sottomenu di \"" << path[step - 1]
                << L"\" non trovato.\n";
            DismissMenus(hwndReal);
            currentParent->Release();
            return false;
        }

        std::wcout << L"\n[" << (step + 1) << L"] Sottomenu di \""
            << path[step - 1] << L"\". Voci disponibili:\n";
        ListChildren(subPopup);

        // Invoca la voce corrente
        // Usa prefix match solo se il livello precedente e' "Load favorite organ"
        bool usePrefix = (_wcsicmp(path[step - 1], L"Load favorite organ") == 0);
        std::wcout << L"\n    Cerco \"" << path[step] << L"\""
            << (usePrefix ? L" (prefix match)" : L"") << L"...\n";
        if (!InvokeMenuItemByName(subPopup, path[step], usePrefix))
        {
            DismissMenus(hwndReal);
            subPopup->Release();
            currentParent->Release();
            std::wcout << L"ERRORE: voce \"" << path[step]
                << L"\" non trovata nel sottomenu.\n";
            return false;
        }

        // Prepara per il livello successivo
        if (step + 1 < path.size())
        {
            Sleep(400);
            // Il prossimo parent e' questo subPopup
            if (currentParent != pRoot)
                currentParent->Release();
            currentParent = subPopup;
            // Non rilasciare subPopup, lo usiamo come parent
        }
        else
        {
            subPopup->Release();
        }
    }

    if (currentParent != pRoot)
        currentParent->Release();
    pRoot->Release();

    std::wcout << L"\n[OK] Comando eseguito con successo!\n";
    return true;
}
// ============================================================
// Apre la finestra "Load organ ..." e seleziona l'organo
// per nome tramite IAccessible.
// ============================================================
static bool FindAndInvokeAccessibleByName(IAccessible* pAcc, const std::wstring& targetName, int depth = 0, RECT* outLocation = nullptr)
{
    if (!pAcc || depth > 10) return false;

    VARIANT varSelf;
    varSelf.vt = VT_I4;
    varSelf.lVal = CHILDID_SELF;

    BSTR bstrName = nullptr;
    if (SUCCEEDED(pAcc->get_accName(varSelf, &bstrName)) && bstrName)
    {
        std::wstring name(bstrName);
        SysFreeString(bstrName);
        if (_wcsicmp(name.c_str(), targetName.c_str()) == 0)
        {
            std::wcout << L"[LoadOrgan] Trovato \"" << name << L"\", seleziono...\n";
            // accDoDefaultAction returns S_FALSE for Qt list items and does nothing.
            // Use accSelect to focus and select the item; the caller will double-click it.
            HRESULT hrSel = pAcc->accSelect(SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION, varSelf);
            HRESULT hrDef = pAcc->accDoDefaultAction(varSelf);
            std::wcout << L"[LoadOrgan] accSelect=0x" << std::hex << hrSel
                << L" accDoDefaultAction=0x" << hrDef << std::dec << L"\n";
            // Retrieve screen location so the caller can simulate a double-click
            if (outLocation)
            {
                long x = 0, y = 0, w = 0, h = 0;
                if (SUCCEEDED(pAcc->accLocation(&x, &y, &w, &h, varSelf)))
                    *outLocation = { x, y, x + w, y + h };
                else
                    SetRectEmpty(outLocation);
            }
            return true; // item found and selection attempted
        }
    }

    long childCount = 0;
    pAcc->get_accChildCount(&childCount);
    if (childCount <= 0 || childCount > 2000) return false;

    std::vector<VARIANT> ch(childCount);
    for (auto& v : ch) VariantInit(&v);

    long obtained = 0;
    if (FAILED(AccessibleChildren(pAcc, 0, childCount, ch.data(), &obtained)))
        return false;

    for (long i = 0; i < obtained; ++i)
    {
        if (ch[i].vt == VT_DISPATCH && ch[i].pdispVal)
        {
            IAccessible* pChild = nullptr;
            if (SUCCEEDED(ch[i].pdispVal->QueryInterface(IID_IAccessible, (void**)&pChild)))
            {
                bool found = FindAndInvokeAccessibleByName(pChild, targetName, depth + 1, outLocation);
                pChild->Release();
                ch[i].pdispVal->Release();
                if (found)
                {
                    for (long j = i + 1; j < obtained; ++j) VariantClear(&ch[j]);
                    return true;
                }
            }
            else
            {
                ch[i].pdispVal->Release();
            }
        }
        else
        {
            VariantClear(&ch[i]);
        }
    }
    return false;
}

bool LoadOrganByName(HWND hwndReal, const std::wstring& organName)
{
    if (!g_midiRouterEnabled)
        return false;

    // 1) Snapshot delle finestre top-level esistenti
    std::vector<HWND> before;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&before));

    // 2) Apre la finestra tramite il menu "Organ" -> "Load organ ..."
    std::wcout << L"[LoadOrgan] Apro 'Load organ ...' per \"" << organName << L"\"\n";
    if (!ClickMenu(hwndReal, L"Organ", L"Load organ ..."))
    {
        std::wcout << L"[LoadOrgan] Impossibile aprire 'Load organ ...'\n";
        return false;
    }

    // 3) Attende fino a 3s la comparsa di una nuova finestra top-level visibile
    HWND hDialog = nullptr;
    for (int i = 0; i < 30 && !hDialog; ++i)
    {
        Sleep(100);
        struct Search { std::vector<HWND>* before; HWND* result; };
        Search s{ &before, &hDialog };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            if (!IsWindowVisible(h)) return TRUE;
            for (HWND bh : *s->before)
                if (bh == h) return TRUE;
            *s->result = h;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&s));
    }

    if (!hDialog)
    {
        std::wcout << L"[LoadOrgan] Finestra di dialogo non trovata\n";
        return false;
    }

    wchar_t dlgTitle[256] = {};
    GetWindowTextW(hDialog, dlgTitle, static_cast<int>(_countof(dlgTitle)));
    std::wcout << L"[LoadOrgan] Dialogo trovato: HWND=0x" << std::hex << (uintptr_t)hDialog
        << std::dec << L" \"" << dlgTitle << L"\"\n";

    // 4) Naviga l'albero IAccessible del dialogo e invoca l'organo per nome.
    // Invece di un'attesa fissa, riprova subito e poi ogni 100ms fino a 5 volte
    // (max 500ms) per attendere che Qt popoli la lista IAccessible.
    bool found = false;
    RECT itemRect = {};
    for (int attempt = 0; attempt < 5 && !found; ++attempt)
    {
        if (attempt > 0)
            Sleep(100);
        IAccessible* pRoot = nullptr;
        if (SUCCEEDED(AccessibleObjectFromWindow(hDialog, OBJID_CLIENT,
                IID_IAccessible, (void**)&pRoot)) && pRoot)
        {
            found = FindAndInvokeAccessibleByName(pRoot, organName, 0, &itemRect);
            pRoot->Release();
        }
    }

    if (found)
    {
        // The item has already been focused and selected via accSelect.
        // Send ENTER to confirm the selection — this is more reliable
        // than simulating a double-click on Qt dialogs because it
        // doesn't depend on cursor position or window hierarchy.
        Sleep(100);
        SetForegroundWindow(hDialog);
        Sleep(100);

        INPUT key[4] = {};
        key[0].type = INPUT_KEYBOARD;
        key[0].ki.wVk = VK_RETURN;
        key[1].type = INPUT_KEYBOARD;
        key[1].ki.wVk = VK_RETURN;
        key[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, key, sizeof(INPUT));
        std::wcout << L"[LoadOrgan] SendInput VK_RETURN inviato\n";
    }
    else
    {
        std::wcout << L"[LoadOrgan] \"" << organName << L"\" non trovato nel dialogo\n";
        // Chiude il dialogo senza caricare
        PostMessageW(hDialog, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(hDialog, WM_KEYUP,   VK_ESCAPE, 0);
    }

    return found;
}

/*
// ============================================================
// MAIN
// ============================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HWND hwndReal = FindHauptwerkRealWindow();
    if (!hwndReal)
    {
        std::cout << "Finestra Qt reale di Hauptwerk non trovata.\n";
        std::cout << "Assicurati che Hauptwerk sia aperto.\n";
        CoUninitialize();
        return -1;
    }

    wchar_t title[256] = {};
    GetWindowTextW(hwndReal, title, 256);
    std::wcout << L"Finestra Qt reale trovata: HWND=0x" << std::hex << (uintptr_t)hwndReal
        << std::dec << L" \"" << title << L"\"\n";

    // --- Mostra menu disponibili ---
    std::wcout << L"\n========================================\n";
    std::wcout << L"  Menu disponibili nella MenuBar:\n";
    std::wcout << L"========================================\n";

    IAccessible* pRoot = nullptr;
    AccessibleObjectFromWindow(hwndReal, OBJID_CLIENT, IID_IAccessible, (void**)&pRoot);
    if (pRoot)
    {
        IAccessible* menuBar = FindChildByRoleAndName(pRoot, ROLE_SYSTEM_MENUBAR, nullptr, 2);
        if (menuBar)
        {
            long mbChildren = 0;
            menuBar->get_accChildCount(&mbChildren);
            if (mbChildren > 0)
            {
                ListChildren(menuBar);
            }
            menuBar->Release();
        }
        pRoot->Release();
    }

    // --- Esempio: 2 livelli ---
    std::wcout << L"\n========================================\n";
    std::wcout << L"  Organ -> Unload organ\n";
    std::wcout << L"========================================\n";

    // 2 livelli — usa ClickMenu originale con fallback
    ClickMenu(hwndReal, mUNLOAD_ORGAN);

    Sleep(500);

    // --- Esempio: 3 livelli ---
    std::wcout << L"\n========================================\n";
    std::wcout << L"  Engine -> Advanced use -> Stop audio/MIDI\n";
    std::wcout << L"========================================\n";

    // 3+ livelli — usa ClickMenu per i primi 2, poi naviga i popup
    ClickMenuPath(hwndReal, { mLOAD_FAVORITE_ORGAN_1 });

    CoUninitialize();
    return 0;
}
*/