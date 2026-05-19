; AhlbornBridge Installer - Inno Setup script
; AppVersion is patched automatically by the post-build step from Version.h.
; Do NOT edit @APP_VERSION@ manually - update Version.h instead.

[Setup]
AppName=AhlbornBridge2
AppVersion=@APP_VERSION@
AppPublisher=AhlbornBridge2
DefaultDirName={pf}\AhlbornBridge2
DefaultGroupName=AhlbornBridge2
OutputBaseFilename=AhlbornBridge2_Setup
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
;ArchitecturesInstallIn64BitMode=yes
WizardStyle=modern

; Includi i file di build dalla cartella contenente questo .iss. Metti qui l'eseguibile, le DLL e le risorse.
[Files]
; Includi tutto il contenuto della cartella di build (se metti lo .iss in quella cartella)
Source: "*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

; Installa la cartella Icons nella posizione Roaming dell'utente corrente usata dal runtime e dal collegamento
Source: "Icons\*"; DestDir: "{userappdata}\AhlbornBridge\Icons"; Flags: ignoreversion recursesubdirs createallsubdirs

; File di configurazione da includere esplicitamente
Source: "Settings.xml"; DestDir: "{app}"; Flags: ignoreversion

; Se preferisci specificare file singoli, sostituisci il pattern * con percorsi espliciti
; Source: "Release\AhlbornBridge.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\AhlbornBridge2"; Filename: "{app}\AhlbornBridge.exe"; IconFilename: "{userappdata}\AhlbornBridge\Icons\AhlbornBridge.ico"
Name: "{group}\Uninstall AhlbornBridge2"; Filename: "{uninstallexe}"
Name: "{userdesktop}\AhlbornBridge2"; Filename: "{app}\AhlbornBridge.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Crea icona sul Desktop"; GroupDescription: "Opzioni installazione"

[Run]
Filename: "{app}\AhlbornBridge.exe"; Description: "Avvia AhlbornBridge2"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{userappdata}\AhlbornBridge"

; Note:
; - Se l'app richiede Visual C++ Redistributable, includi il controllo o il pacchetto separato.
; - Modifica il nome dell'eseguibile in [Icons] e [Run] se non si chiama "AhlbornBridge.exe" dopo la build.
