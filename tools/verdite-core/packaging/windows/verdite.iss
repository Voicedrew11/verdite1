; Inno Setup script for a Verdite port.
;
; Run the game's packaging/windows/build-windows.ps1 first: this installs an
; already published tree and does not build one, and every name below comes from
; the environment that script sets out of the game's packaging/package.env.
;
; The installer deliberately does NOT ask for the disc. The game is built on
; first launch, from the image the player picks in the app itself, into
; %LOCALAPPDATA%\<name> -- so the install directory stays read-only, an
; uninstall leaves saves alone, and reinstalling does not force a rebuild.

; The version comes from the game's VERSION, the one place it is written. A
; literal fallback here would be a second source of it and would quietly ship an
; installer whose name disagreed with its contents, so a missing value is an
; error instead; the same for the names.
#define AppVersion GetEnv("VERDITE_VERSION")
#define AppName    GetEnv("VERDITE_NAME")
#define AppGuid    GetEnv("VERDITE_INNO_APP_ID")
#define Root       GetEnv("VERDITE_ROOT")
#if AppVersion == "" || AppName == "" || AppGuid == "" || Root == ""
  #error VERDITE_VERSION, VERDITE_NAME, VERDITE_INNO_APP_ID or VERDITE_ROOT is not set. Run the game's packaging/windows/build-windows.ps1 rather than iscc directly.
#endif

[Setup]
; One GUID per game, from package.env: it is how Windows tells an upgrade of
; this port from a second, different program.
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Voicedrew11
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
; <name>.exe at the install root is the stub; the self-contained runtime lives in
; bin\. Shortcuts still point at the stub so the player never has to open that
; folder.
UninstallDisplayIcon={app}\{#AppName}.exe
OutputDir={#Root}\dist
OutputBaseFilename={#AppName}-{#AppVersion}-win-x64-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#Root}\LICENSE
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Root}\dist\win-x64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; Saves, settings and the built game live in %LOCALAPPDATA%\<name> and are NOT
; removed: an uninstall should not delete somebody's save file. The built game
; assembly goes with them, which costs a rebuild on reinstall and is the right
; way round -- a lost save cannot be rebuilt.
