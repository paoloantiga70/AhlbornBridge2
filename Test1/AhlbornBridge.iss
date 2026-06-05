; AhlbornBridge Installer - Inno Setup script
; AppVersion is patched automatically by the post-build step from Version.h.
; Do NOT edit @APP_VERSION@ manually - update Version.h instead.

#define ServiceExeName "ProcessManagerService.exe"
#define ServiceExeInScriptDir AddBackslash(SourcePath) + ServiceExeName
#define ServiceExeInRepoRelease AddBackslash(SourcePath) + "..\ProcessManagerService\x64\Release\ProcessManagerService.exe"
#define ServiceExeInRepoDebug AddBackslash(SourcePath) + "..\ProcessManagerService\x64\Debug\ProcessManagerService.exe"
#if FileExists(ServiceExeInScriptDir)
  #define ServiceExeSource ServiceExeInScriptDir
#elif FileExists(ServiceExeInRepoRelease)
  #define ServiceExeSource ServiceExeInRepoRelease
#elif FileExists(ServiceExeInRepoDebug)
  #define ServiceExeSource ServiceExeInRepoDebug
#else
  #error ProcessManagerService.exe not found. Build ProcessManagerService (x64 Release) or copy it next to the .iss before compiling setup.
#endif

[Setup]
AppName=AhlbornBridge2
AppVersion=@APP_VERSION@
AppPublisher=AhlbornBridge2
DefaultDirName={localappdata}\AhlbornBridge2
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
; Include ProcessManagerService.exe from a validated compile-time source.
Source: "{#ServiceExeSource}"; DestDir: "{app}"; DestName: "ProcessManagerService.exe"; Flags: ignoreversion
; Changelog — read at runtime to show What's New on first launch after update.
Source: "CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion

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
Name: "{userstartup}\AhlbornBridge2"; Filename: "{app}\AhlbornBridge.exe"; Tasks: autostart

[Tasks]
Name: "desktopicon"; Description: "Crea icona sul Desktop"; GroupDescription: "Opzioni installazione"
Name: "autostart"; Description: "Avvia AhlbornBridge2 con Windows"; GroupDescription: "Opzioni installazione"

[Code]
// Stop any running AhlbornBridge before install so the new files can be copied.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  ServiceExe: String;
  ServiceInstalled: Boolean;
begin
  if CurStep = ssInstall then
  begin
    Exec('taskkill', '/f /im AhlbornBridge.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(500);

    // Best-effort stop previous service instance before replacing binaries.
    Exec('sc.exe', 'stop AhlbornBridgeProcessManager', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(500);
  end
  else if CurStep = ssPostInstall then
  begin
    ServiceExe := ExpandConstant('{app}\ProcessManagerService.exe');
    if FileExists(ServiceExe) then
    begin
      Log('ProcessManagerService.exe found at: ' + ServiceExe);

      ServiceInstalled := Exec('sc.exe', 'query AhlbornBridgeProcessManager', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
      if ServiceInstalled then
      begin
        Log('ProcessManager service already installed. Skipping --install.');
      end
      else
      begin
        if Exec(ServiceExe, '--install', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
          Log('ProcessManager service install command executed. ExitCode=' + IntToStr(ResultCode))
        else
          Log('ProcessManager service install command FAILED. LastError=' + IntToStr(ResultCode));
      end;

      if Exec('sc.exe', 'start AhlbornBridgeProcessManager', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
        Log('ProcessManager service start command executed. ExitCode=' + IntToStr(ResultCode))
      else
        Log('ProcessManager service start command FAILED. LastError=' + IntToStr(ResultCode));
    end
    else
    begin
      Log('ProcessManagerService.exe NOT found in {app}. Service installation skipped.');
      MsgBox('ProcessManagerService.exe was not found in the installation folder. Process priority service was not installed.', mbError, MB_OK);
    end;
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
