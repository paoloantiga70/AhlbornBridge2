; AhlbornBridge Installer - Inno Setup script
; AppVersion is patched automatically by the post-build step from Version.h.
; Do NOT edit @APP_VERSION@ manually - update Version.h instead.

[Setup]
AppName=AhlbornBridge2
AppVersion=@APP_VERSION@
AppPublisher=AhlbornBridge2
DefaultDirName={localappdata}\AhlbornBridge2
DefaultGroupName=AhlbornBridge2
OutputBaseFilename=AhlbornBridge2_Setup
Compression=lzma
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
;ArchitecturesInstallIn64BitMode=yes
WizardStyle=modern

; Includi i file di build dalla cartella contenente questo .iss. Metti qui l'eseguibile, le DLL e le risorse.
[Files]
; Includi tutto il contenuto della cartella di build (se metti lo .iss in quella cartella)
Source: "*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

; Installa la cartella Icons nella posizione Roaming dell'utente corrente usata dal runtime e dal collegamento
Source: "Icons\*"; DestDir: "{userappdata}\AhlbornBridge2\Icons"; Flags: ignoreversion recursesubdirs createallsubdirs

; File di configurazione: copiato solo alla prima installazione, preservato negli aggiornamenti
Source: "Settings.xml"; DestDir: "{userappdata}\AhlbornBridge2"; Flags: onlyifdoesntexist

; Se preferisci specificare file singoli, sostituisci il pattern * con percorsi espliciti
; Source: "Release\AhlbornBridge.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\AhlbornBridge2"; Filename: "{app}\AhlbornBridge.exe"; IconFilename: "{userappdata}\AhlbornBridge2\Icons\AhlbornBridge.ico"
Name: "{group}\Uninstall AhlbornBridge2"; Filename: "{uninstallexe}"
Name: "{userdesktop}\AhlbornBridge2"; Filename: "{app}\AhlbornBridge.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Crea icona sul Desktop"; GroupDescription: "Opzioni installazione"
Name: "autostart"; Description: "Avvia AhlbornBridge2 con Windows"; GroupDescription: "Opzioni installazione"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "AhlbornBridge2"; ValueData: """{app}\AhlbornBridge.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Code]
// Stop any running AhlbornBridge before install so the new files can be copied.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    Exec('taskkill', '/f /im AhlbornBridge.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(500);
  end;
end;

[Run]
Filename: "{app}\AhlbornBridge.exe"; Description: "Avvia AhlbornBridge2"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{userappdata}\AhlbornBridge2"

; Note:
; - Se l'app richiede Visual C++ Redistributable, includi il controllo o il pacchetto separato.
; - Modifica il nome dell'eseguibile in [Icons] e [Run] se non si chiama "AhlbornBridge.exe" dopo la build.
