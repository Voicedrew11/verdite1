; The Windows installer. make-installer.ps1 runs this on the package from the zip: it installs that
; tree and builds nothing. The game asks for the disc itself on first launch.
#define AppVersion GetEnv("VERDITE_VERSION")
#define Stage      GetEnv("VERDITE_STAGE")
#define OutDir     GetEnv("VERDITE_OUT")
#if AppVersion == "" || Stage == "" || OutDir == ""
  #error Run packaging\windows\make-installer.ps1 rather than iscc directly.
#endif

[Setup]
; How Windows tells an upgrade of Verdite1 from another program. Never change it, and never give
; another port this one.
AppId={{6F0E4B1C-3A7D-4C52-9E18-B2D7A45C1F60}
AppName=Verdite1
AppVersion={#AppVersion}
AppPublisher=Voicedrew11
AppPublisherURL=https://github.com/Voicedrew11/verdite1
AppUpdatesURL=https://github.com/Voicedrew11/verdite1/releases
; Per user, in %LOCALAPPDATA%\Programs\Verdite1: no administrator, and the game can keep its "data"
; folder beside it.
PrivilegesRequired=lowest
DefaultDirName={autopf}\Verdite1
DefaultGroupName=Verdite1
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\verdite1.exe
OutputDir={#OutDir}
OutputBaseFilename=Verdite1-{#AppVersion}-win-x64-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#Stage}\licenses\Verdite1-MIT.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; An upgrade drops what the last version shipped and a new one may not, as Verdite2's and Verdite3's
; do. The game's "data" folder is not in the list: it stays, as do saves in the user folder.
[InstallDelete]
Type: filesandordirs; Name: "{app}\mods"
Type: filesandordirs; Name: "{app}\licenses"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Verdite1"; Filename: "{app}\verdite1.exe"
Name: "{autodesktop}\Verdite1"; Filename: "{app}\verdite1.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\verdite1.exe"; Description: "{cm:LaunchProgram,Verdite1}"; Flags: nowait postinstall skipifsilent
