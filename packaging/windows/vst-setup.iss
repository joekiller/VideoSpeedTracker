; Windows installer for VideoSpeedTracker (Inno Setup 6, preinstalled on GitHub's windows-2022 runners).
;
; Built by .github/workflows/release.yml from the staged install (cmake --install, plus the FFmpeg plugin DLL),
; after the exes inside it are signed; the setup exe is then signed too.  By hand:
;
;   iscc /DSourceDir=C:\path\to\staged /DAppVersion=1.3.0 /DNumericVersion=1.3.0 /DOutputDir=dist packaging\windows\vst-setup.iss
;
; What it does:
;   * Installs per user by default (no UAC prompt, into %LOCALAPPDATA%\Programs), or for everyone when the user
;     picks that in the first dialog (Program Files, needs admin).  Same AppId either way, so upgrades replace
;     the files in place.
;   * A Start-menu "VST Studio" shortcut (and an optional desktop one), started in the user's home folder rather
;     than the install folder: VST looks for VST.cfg in the folder it starts in first, and the VST.cfg installed
;     there is only the template for the per-user copy (%APPDATA%\VideoSpeedTracker\VST.cfg).
;   * For an all-users install, a Windows Firewall rule letting VST Studio's companion server answer phones on
;     private networks (home Wi-Fi) only.  A per-user install can't add one; Windows asks the first time the
;     companion is turned on instead.
;   * Uninstalling removes the program and the firewall rule but never the user's config, calibration history,
;     videos or results.

#ifndef SourceDir
  #error Pass /DSourceDir=<staged install folder>
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.0.0"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1C2A57-93D8-4B0E-A7C4-5E2D9B81F340}
AppName=VideoSpeedTracker
AppVersion={#AppVersion}
AppVerName=VideoSpeedTracker {#AppVersion}
AppPublisher=Joe Lawson
AppPublisherURL=https://github.com/joekiller/VideoSpeedTracker
AppSupportURL=https://github.com/joekiller/VideoSpeedTracker/issues
AppComments=Measure the speed of traffic on your street with a camera and VST Studio.
VersionInfoVersion={#NumericVersion}
VersionInfoProductName=VideoSpeedTracker
DefaultDirName={autopf}\VideoSpeedTracker
DefaultGroupName=VideoSpeedTracker
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#SourceDir}\LICENSE.txt
SetupIconFile=vst-studio.ico
UninstallDisplayIcon={app}\VSTStudio.exe
UninstallDisplayName=VideoSpeedTracker {#AppVersion}
OutputDir={#OutputDir}
OutputBaseFilename=VideoSpeedTracker-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\VST Studio"; Filename: "{app}\VSTStudio.exe"; WorkingDir: "{%USERPROFILE}"; Comment: "Set up a speed camera for your street and see who's speeding"
Name: "{autodesktop}\VST Studio"; Filename: "{app}\VSTStudio.exe"; WorkingDir: "{%USERPROFILE}"; Tasks: desktopicon

[Run]
; The rule is replaced, not added, so reinstalling doesn't pile up duplicates.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""VST Studio companion"""; Flags: runhidden; Check: IsAdminInstallMode
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""VST Studio companion"" dir=in action=allow program=""{app}\VSTStudio.exe"" enable=yes profile=private"; Flags: runhidden; Check: IsAdminInstallMode
Filename: "{app}\VSTStudio.exe"; Description: "{cm:LaunchProgram,VST Studio}"; WorkingDir: "{%USERPROFILE}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""VST Studio companion"""; Flags: runhidden; Check: IsAdminInstallMode; RunOnceId: "DelFirewallRule"
