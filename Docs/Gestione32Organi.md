# Gestione dei 32 Organi Preferiti — AhlbornBridge

---

## 1. Sorgente dati — `Config.Config_Hauptwerk_xml`

Hauptwerk usa **due naming convention** diverse nello stesso file:

| Slot | Tag nel config Hauptwerk | Esempio valore grezzo |
|---|---|---|
| 1 – 8 | `<Standby_Organ01>` … `<Standby_Organ08>` | `Leens Hinsz.Organ_Hauptwerk_xml` |
| 9 – 32 | `<sborg09>` … `<sborg32>` | `StAnnesMoseley.Organ_Hauptwerk_xml` |

**Percorso del file:**
```
{RootFolder_HauptwerkUserData}\Config0-GeneralSettings\Config.Config_Hauptwerk_xml
```

**Navigazione XML:**
```xml
<Hauptwerk>
  <ObjectList>
    <_General ObjectID="1">
      <Standby_Organ01>Leens Hinsz.Organ_Hauptwerk_xml</Standby_Organ01>
      ...
      <Standby_Organ08>...</Standby_Organ08>
      <sborg09>StAnnesMoseley.Organ_Hauptwerk_xml</sborg09>
      ...
      <sborg32>...</sborg32>
    </_General>
  </ObjectList>
</Hauptwerk>
```

---

## 2. Lettura e scrittura — `ReadHauptwerkStandbyOrgans()` → `Settings.xml`

- Naviga `<Hauptwerk>` → `<ObjectList>` → `<_General>`
- **Ciclo 1** (`Xml.cpp`): legge `<Standby_Organ01>` … `<Standby_Organ08>`
- **Ciclo 2** (`Xml.cpp`): legge `<sborg09>` … `<sborg32>`
- Rimuove il suffisso `.Organ_Hauptwerk_xml` da ogni valore (fallback: taglia dall'ultimo `.`)
- Scrive tutto in `Settings.xml` sotto `<StandbyeOrgans>`:

```xml
<StandbyeOrgans>
    <Standby_Organ01>Leens Hinsz</Standby_Organ01>
    ...
    <Standby_Organ08>...</Standby_Organ08>
    <sborg09>StAnnesMoseley</sborg09>
    ...
    <sborg32>...</sborg32>
</StandbyeOrgans>
```

> I slot vuoti (organi non configurati in Hauptwerk) vengono saltati.

---

## 3. Lettura da Settings.xml — `LoadStandbyOrganNames()`

Restituisce un `vector<wstring>` di **32 elementi** (slot vuoti = stringa vuota):

- Slot `[0]`  … `[7]`  → `<Standby_Organ01>` … `<Standby_Organ08>`
- Slot `[8]`  … `[31]` → `<sborg09>` … `<sborg32>`

---

## 4. Rilevamento organo in caricamento — `OrganLoadingWatchThread`

```
"Loading organ" dialog appare
      │
      ├─ Estrae il titolo dalla dialog (IAccessible / Win32)
      ├─ Chiama LoadStandbyOrganNames()  →  vector<wstring>[32]
      ├─ Confronta (exact match) su tutti i 32 slot
      │
      └─ Se MATCH su slot idx (0-based):
             g_currentLoadedFavoriteIndex = idx + 1    (1-based, 1..32)
             Invia output MIDI:  BF 50 <idx+1>
```

---

## 5. Messaggi MIDI in output — Bridge → Stream Deck

| Situazione | Messaggio MIDI inviato |
|---|---|
| Organo N caricato (N = 1..32) | `BF 50 <N>` |
| Organo scaricato o caricamento cancellato | `BF 50 00` |
| Plugin Stream Deck si avvia (sync) | `BF 50 <g_currentLoadedFavoriteIndex>` |

> Il messaggio `BF 50 xx` **non viene mai inoltrato** dal router MIDI
> (escluso esplicitamente per evitare loop di segnale).

---

## 6. Messaggi MIDI in input — Stream Deck → Bridge — `MidiInProc`

| `data2` ricevuto | Condizione | Azione |
|---|---|---|
| `00` | `isOrganLoaded == true` | Accoda `DeferredCmd::LoadFavoriteOrgan0` → Unload organ |
| `00` | `g_isLoadingOrgan == true` | Invia `VK_RETURN` alla dialog "Loading organ" (cancel) |
| `01` … `32` | `g_isLoadingOrgan == true` | Prima cancella (VK_RETURN sulla dialog) |
| `01` … `32` | sempre | Invia `BF 50 00` + `BF 50 <N>` in output (feedback stato), accoda `DeferredCmd::LoadFavoriteOrganN` |

---

## 7. Esecuzione differita — `DeferredCmdWorkerThread`

```cpp
// LoadFavoriteOrgan0  →  Unload:
ClickMenu(hwnd, { L"Organ", L"Unload organ" })

// LoadFavoriteOrgan1..32  →  Load favorite:
label = std::to_wstring(organIdx) + L": "   // "1: ", "9: ", "32: "
ClickMenuPath(hwnd, { L"Organ", L"Load favorite organ", label })
```

Il path del menu viene costruito **dinamicamente** dall'indice: nessuna macro separata
per ciascun organo.

---

## 8. Schema riassuntivo del flusso

```
Hauptwerk config XML  (Standby_Organ01..08 + sborg09..32)
        │
        │  ReadHauptwerkStandbyOrgans()   [Xml.cpp]
        ▼
    Settings.xml   <StandbyeOrgans>  [32 slot]
        │
        │  LoadStandbyOrganNames()   [Xml.cpp]
        ▼
OrganLoadingWatchThread ──── match ────► BF 50 xx OUT ──► Stream Deck
                                                ▲
                                                │  (sync / feedback stato)
                                                │
MidiInProc ◄──── BF 50 xx IN ◄──── Stream Deck
        │
        │  TryEnqueue(DeferredCmd::LoadFavoriteOrganN)
        ▼
DeferredCmdWorkerThread
        │
        │  ClickMenuPath(hwnd, { L"Organ", L"Load favorite organ", L"N: " })
        ▼
    Hauptwerk  →  carica organo preferito N
```

---

## File coinvolti

| File | Ruolo |
|---|---|
| `Xml.cpp` | Lettura config Hauptwerk, scrittura/lettura Settings.xml |
| `Xml.h` | Dichiarazione `LoadStandbyOrganNames()` |
| `Midi.cpp` | `MidiInProc`, `DeferredCmdWorkerThread`, `OrganLoadingWatchThread` |
| `Midi.h` | Costanti `CC_ch16`, `BF_0x50` |
| `StreamDeck.cpp` | `SendUnloadOrganMidiMessage()` (BF 50 00) |
| `StreamDeck.h` | Define `LOAD_FAVORITE_ORGAN_1..8` |
| `Qt.cpp` / `Qt.h` | `ClickMenuPath()` — navigazione menu Hauptwerk via MSAA |
