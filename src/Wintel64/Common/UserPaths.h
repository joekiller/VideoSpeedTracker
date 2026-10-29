// Per-user file locations shared by VideoSpeedTracker and ProcessHiLites.
//
// Installed copies of VST.cfg / ProcessHiLites.cfg are read-only templates (a .deb puts them in
// /usr/share, a Store/MSIX package or .app bundle can't be written at all), so each user gets their
// own config and data folders in the place their OS expects:
//
//            config                                         data (IPCam, HiLites, Stats, Trace)
//   Linux    $XDG_CONFIG_HOME/videospeedtracker             $XDG_DATA_HOME/videospeedtracker
//            (default ~/.config/videospeedtracker)          (default ~/.local/share/videospeedtracker)
//   Windows  %APPDATA%\VideoSpeedTracker                     %LOCALAPPDATA%\VideoSpeedTracker
//   macOS    ~/Library/Application Support/VideoSpeedTracker  ~/Library/Application Support/VideoSpeedTracker/Data
//
// Windows uses the Known Folder API and macOS uses $HOME, so both follow MSIX and App Sandbox redirection.

#pragma once
#include <filesystem>
#include <string>

namespace userpaths {

std::filesystem::path configDir();
std::filesystem::path dataDir();
std::filesystem::path executableDir();

// Creates the IPCam, HiLites/forPosting, Stats and Trace folders under d (existing ones are left alone).
void makeDataDirs(const std::filesystem::path& d);

// Folder a config's dataPathPrefix names: the value itself, or dataDir() (with its subfolders created)
// when it is blank, which is how the shipped templates leave it.
std::filesystem::path dataPath(const std::string& dataPathPrefix);

// A file installed with the program (rel may contain '/'), looked for next to the executable, in
// ../share/videospeedtracker (FHS installs, e.g. the .deb) and in ../Resources (macOS bundle).
// Empty if not found, e.g. when running from a build tree.
std::filesystem::path installedFile(const std::string& rel);

// The 2016 Locust Avenue clip that release packages bundle under sample/; empty if it isn't installed.
// The VST.cfg template's geometry and calibration are the ones that clip was recorded with.
std::filesystem::path sampleVideo();

// Windows: if the program got a console of its own (started by double-clicking the exe), wait for
// Enter before the window closes at exit, so its messages can be read.  No-op elsewhere.
void holdConsoleOnExit();

// Picks the config file to read when --config wasn't given, in this order:
//  1. <name> in the working directory (the original 2016 workflow, so existing setups keep working)
//  2. <configDir>/<name>
//  3. a fresh copy of the template installed with the program, written to (2) with dataPathPrefix
//     pointed at dataDir(), which is created with the IPCam, HiLites/forPosting, Stats and Trace folders
// Prints which file it chose. Returns an empty string, after printing why, if none can be found or made.
std::string resolveConfig(const std::string& name);

}
