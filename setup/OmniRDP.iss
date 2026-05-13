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
DefaultDirName={commonpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=OmniRDP-Setup
Compression=lzma2
PrivilegesRequired=admin
SolidCompression=yes
DisableProgramGroupPage=yes
ArchitecturesInstallIn64BitMode=x64compatible

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

procedure KillAllOmniRDPProcesses;
var
  Images: array[0..2] of String;
  I: Integer;
begin
  Images[0] := 'OmniRDP-tray.exe';
  Images[1] := 'OmniRDP-svc.exe';
  Images[2] := 'OmniRDP.exe';
  for I := 0 to 2 do
    KillProcessByImage(Images[I]);
end;

function AppDirHasFiles: Boolean;
var
  FindRec: TFindRec;
  AppDir: String;
begin
  Result := False;
  AppDir := ExpandConstant('{app}');
  if FindFirst(AppDir + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Result := True;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    { Kill all OmniRDP processes BEFORE the uninstaller attempts file removal. }
    Log('Uninstall: pre-removal kill of OmniRDP processes');
    KillAllOmniRDPProcesses;
    Sleep(1000);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    if AppDirHasFiles then
    begin
      { Safety net: files still remain — kill again and give extra time. }
      Log('Uninstall: files remain in {app}, retrying process kill');
      KillAllOmniRDPProcesses;
      Sleep(3000);
      if AppDirHasFiles then
        Log('Uninstall: files STILL remain after retry — manual cleanup may be needed')
      else
        Log('Uninstall: retry succeeded, all files removed');
    end
    else
      Log('Uninstall: all files removed successfully');
  end;
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
Filename: "{app}\OmniRDP-svc.exe"; Parameters: "--install"
; Install tray
Filename: "{app}\OmniRDP-tray.exe"; Parameters: "--install"; Flags: runasoriginaluser
; Launch tray after install
Filename: "{app}\OmniRDP-tray.exe"; Description: "Launch OmniRDP Tray"; Flags: postinstall skipifsilent runasoriginaluser nowait

[UninstallRun]
; Uninstall tray
Filename: "{app}\OmniRDP-tray.exe"; Parameters: "--uninstall"; RunOnceId: "UninstallTray"
; Uninstall service
Filename: "{app}\OmniRDP-svc.exe"; Parameters: "--uninstall"; RunOnceId: "UninstallService"
