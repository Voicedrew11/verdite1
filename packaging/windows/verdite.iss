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

; The version comes from the script, by the rule scripts/version.sh gives. A
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

; "lowest" installs for the one player, under %LOCALAPPDATA%\Programs, with the
; shortcuts in their own Desktop and Start menu, which the game may then point at
; the card icon it reads off the disc (Verdite Core's ShortcutIcon). "admin", the
; default, installs for every user, and the game cannot rewrite those shortcuts.
#define Privileges GetEnv("VERDITE_INNO_PRIVILEGES")
#if Privileges == ""
  #define Privileges "admin"
#endif

[Setup]
; One GUID per game, from package.env: it is how Windows tells an upgrade of
; this port from a second, different program.
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Voicedrew11
PrivilegesRequired={#Privileges}
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

; An upgrade installs over the old tree, and Inno removes nothing the new release
; dropped. A stale .cs in content\ was compiled into the game (Verdite3 0.1.1 over
; 0.1.0: seven errors), and a stale DLL in bin\ can be loaded. Both are the
; release's own, never the player's (saves and settings are in %LOCALAPPDATA%), so
; they go before the new files land. The launcher also ignores anything in content\
; that content\manifest.txt does not list, which covers the zip.
[InstallDelete]
Type: filesandordirs; Name: "{app}\content"
Type: filesandordirs; Name: "{app}\bin"

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
