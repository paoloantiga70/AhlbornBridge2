; AhlbornBridgeSD Plugin Installer - Inno Setup script
; AppVersion is patched automatically by the post-build step from PluginVersion.h.
; Do NOT edit @PLUGIN_VERSION@ manually - update PluginVersion.h instead.

[Setup]
AppName=AhlbornBridgeSD2 Stream Deck Plugin
AppVersion=@PLUGIN_VERSION@
AppPublisher=Paolo Antiga
DefaultDirName={userappdata}\Elgato\StreamDeck\Plugins\com.ahlbornbridge.organ.sdPlugin
DefaultGroupName=AhlbornBridgeSD2 Plugin
OutputBaseFilename=AhlbornBridgeSD2_Setup
Compression=lzma
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableDirPage=yes
DisableProgramGroupPage=yes
Uninstallable=yes
UninstallDisplayName=AhlbornBridgeSD2 Stream Deck Plugin

[Files]
; Plugin executable
Source: "AhlbornBridgeSD.exe"; DestDir: "{app}"; Flags: ignoreversion

; Manifest and Property Inspector (from sdPlugin source folder)
Source: "{#SourceBase}\manifest.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceBase}\property_inspector.html"; DestDir: "{app}"; Flags: ignoreversion

; Images
Source: "{#SourceBase}\images\*"; DestDir: "{app}\images"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
// Stop Stream Deck before install, restart after
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    Exec('taskkill', '/f /im StreamDeck.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
  begin
    // Will be launched by [Run] section if user checks the box
  end;
end;

// Return the StreamDeck.exe path (C:\Program Files\Elgato\StreamDeck).
function GetStreamDeckPath(Param: String): String;
begin
  Result := ExpandConstant('{commonpf64}') + '\Elgato\StreamDeck\StreamDeck.exe';
end;

[Run]
Filename: "{code:GetStreamDeckPath}"; Description: "Start Stream Deck"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill"; Parameters: "/f /im StreamDeck.exe"; Flags: runhidden

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
