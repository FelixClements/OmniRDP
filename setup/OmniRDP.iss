; OmniRDP Inno Setup Installer
; Version 1.0.0

#define MyAppName "OmniRDP"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "OmniRDP"
#define MyAppURL "https://github.com/OmniRDP/OmniRDP"

[Setup]
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

[Files]
; Application files
Source: "..\artifacts\windows-Release\bin\OmniRDP.exe"; DestDir: "{app}"; Flags: ignoreversion
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
