; OmniRDP Inno Setup Installer
; Version 1.0.0

#define MyAppName "OmniRDP"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "OmniRDP"
#define MyAppURL "https://github.com/OmniRDP/OmniRDP"

[Setup]
AppId=OmniRDP
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={pf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=OmniRDP-Setup
Compression=lzma2
PrivilegesRequired=admin
SolidCompression=yes
DisableProgramGroupPage=yes

[Code]
var
  UpgradePrepared: Boolean;

procedure RunExistingUninstall(FileName: String);
var
  ResultCode: Integer;
  ExpandedFileName: String;
begin
  ExpandedFileName := ExpandConstant(FileName);

  if FileExists(ExpandedFileName) then
  begin
    Log('Upgrade cleanup: running ' + ExpandedFileName + ' --uninstall');

    if Exec(ExpandedFileName, '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    begin
      if ResultCode = 0 then
        Log('Upgrade cleanup: ' + ExpandedFileName + ' --uninstall succeeded')
      else
        Log('Upgrade cleanup: ' + ExpandedFileName + ' --uninstall failed with exit code ' + IntToStr(ResultCode));
    end
    else
      Log('Upgrade cleanup: failed to execute ' + ExpandedFileName + ' --uninstall');
  end
  else
    Log('Upgrade cleanup: ' + ExpandedFileName + ' not found, skipping');
end;

procedure KillProcessByImage(ImageName: String);
var
  ResultCode: Integer;
  TaskKillPath: String;
begin
  TaskKillPath := ExpandConstant('{sys}\taskkill.exe');

  Log('Upgrade cleanup: terminating running process image ' + ImageName + ' to release locked tray binaries');

  if Exec(TaskKillPath, '/IM "' + ImageName + '" /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if ResultCode = 0 then
      Log('Upgrade cleanup: taskkill succeeded for ' + ImageName)
    else
      Log('Upgrade cleanup: taskkill for ' + ImageName + ' exited with code ' + IntToStr(ResultCode));
  end
  else
    Log('Upgrade cleanup: failed to execute ' + TaskKillPath + ' for ' + ImageName + ', continuing');
end;

procedure PrepareForUpgrade;
begin
  if UpgradePrepared then
    exit;

  UpgradePrepared := True;

  { Upgrades temporarily stop tray/service to release locked binaries before file copy. }
  RunExistingUninstall('{app}\OmniRDP-tray.exe');
  { Upgrade-only fallback: old tray versions may remain running after --uninstall and lock the binary. }
  KillProcessByImage('OmniRDP-tray.exe');
  RunExistingUninstall('{app}\OmniRDP-svc.exe');
end;

[Files]
; Application files
Source: "..\artifacts\windows-Release\bin\OmniRDP.exe"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: PrepareForUpgrade
Source: "..\artifacts\windows-Release\bin\OmniRDP-svc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\OmniRDP-tray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\freerdp3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\freerdp-client3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\freerdp-server3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\winpr3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\libcrypto-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\libssl-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\windows-Release\bin\zlib1.dll"; DestDir: "{app}"; Flags: ignoreversion
; Configuration template
Source: "..\setup\config.ini.template"; DestDir: "{commonappdata}\OmniRDP"; DestName: "config.ini"; Flags: onlyifdoesntexist
Source: "..\setup\license\*"; DestDir: "{app}\license"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
; Install service
Filename: "{app}\OmniRDP-svc.exe"; Parameters: "--install"; Flags: runasoriginaluser
; Install tray
Filename: "{app}\OmniRDP-tray.exe"; Parameters: "--install"; Flags: runasoriginaluser

[UninstallRun]
; Uninstall tray
Filename: "{app}\OmniRDP-tray.exe"; Parameters: "--uninstall"
; Uninstall service
Filename: "{app}\OmniRDP-svc.exe"; Parameters: "--uninstall"
