#define MyAppName "Last Download Manager"
#define MyAppVersion "2.1.1"
#define MyAppPublisher "Last Projects"
#define MyAppURL "https://lastprojects.com/"
#define MyAppExeName "LDM.exe"
#define MyProjectRoot ".."

[Setup]
AppId={{B0B82FA8-8287-4880-996D-83C924AF0A14}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile={#MyProjectRoot}\LICENSE
OutputDir={#MyProjectRoot}\dist
OutputBaseFilename=Last_Download_Manager_Setup_X64
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile={#MyProjectRoot}\installer\setup_icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyProjectRoot}\bin\x64\Release\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MyProjectRoot}\BrowserExtension\*"; DestDir: "{app}\BrowserExtension"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MyProjectRoot}\LDM\resources\cacert.pem"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyProjectRoot}\LDM\resources\app_icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyProjectRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyProjectRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon; IconFilename: "{app}\app_icon.ico"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
