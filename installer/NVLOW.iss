; NVLOW installer (Inno Setup 6)
; Build with:  .\build.ps1 -Installer      -> dist\NVLOW-Setup-<version>.exe
; or manually: ISCC.exe /DAppVersion=1.1.0 installer\NVLOW.iss

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif

#define AppName      "NVLOW"
#define AppExe       "NVLOW.exe"
#define AppPublisher "NVLOW"

[Setup]
; Never change AppId: it lets new versions upgrade the existing install.
AppId={{7C2B9E4A-3F1D-4B8E-9A6C-5D2E8F1A4B37}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppComments=Shrink NVIDIA recordings without losing quality
VersionInfoVersion={#AppVersion}

; Per-user install by default (no admin prompt); the user can choose "all users" instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableReadyPage=no

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

OutputDir=..\dist
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
SetupIconFile=..\res\nvlow.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes

; Close a running NVLOW before upgrading/uninstalling (matches the app's single-instance mutex).
AppMutex=Local\NVLOW.SingleInstance
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; In-app updates run Setup with /SILENT: reopen NVLOW automatically when the upgrade is done.
Filename: "{app}\{#AppExe}"; Flags: nowait skipifnotsilent runasoriginaluser

; User settings (%APPDATA%\NVLOW) and compressed videos are intentionally kept on uninstall.
