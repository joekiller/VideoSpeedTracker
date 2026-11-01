#include "UserPaths.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
using namespace std;

namespace userpaths {

namespace {

// $name if it is set to an absolute path (the XDG spec says to ignore relative values), else empty.
fs::path absoluteEnv(const char* name){
	const char* v = getenv(name);
	if (v == nullptr || *v == '\0') return {};
	fs::path p(v);
	return p.is_absolute() ? p : fs::path();
}

fs::path home(){
	fs::path h = absoluteEnv("HOME");
	return h.empty() ? fs::current_path() : h;
}

#if defined(_WIN32)
fs::path knownFolder(REFKNOWNFOLDERID id){
	PWSTR raw = nullptr;
	fs::path p;
	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) p = raw;
	CoTaskMemFree(raw);
	return p;
}
#endif

// Copies the template line by line, swapping the right-hand side of "dataPathPrefix = ... # comment" for dataDir().
bool seedConfig(const fs::path& from, const fs::path& to){
	ifstream in(from);
	if (!in) return false;
	vector<string> lines;
	string line;
	while (getline(in, line)){
		if (!line.empty() && line.back() == '\r') line.pop_back();  // templates are CRLF; write native line endings
		size_t eq = line.find('=');
		if (line.compare(0, 14, "dataPathPrefix") == 0 && eq != string::npos){
			size_t hash = line.find('#', eq);
			string comment = hash == string::npos ? string() : "  " + line.substr(hash);
			line = "dataPathPrefix = " + dataDir().string() + comment;
		}
		lines.push_back(line);
	}
	error_code ec;
	fs::create_directories(to.parent_path(), ec);
	ofstream out(to);
	// No newline after the last line: the 2016 VST.cfg reader treats a trailing empty line as an error.
	for (size_t i = 0; i < lines.size(); i++) out << (i ? "\n" : "") << lines[i];
	return bool(out);
}

}  // namespace

void makeDataDirs(const fs::path& d){
	error_code ec;
	for (const char* sub : { "IPCam", "HiLites/forPosting", "Stats", "Trace" })
		fs::create_directories(d / fs::path(sub).make_preferred(), ec);
}

fs::path dataPath(const string& dataPathPrefix){
	if (!dataPathPrefix.empty()) return fs::path(dataPathPrefix);
	fs::path d = dataDir();
	makeDataDirs(d);
	return d;
}

// Installed file locations, relative to the executable:
//   <exe dir>/rel                          Windows zip / installer / MSIX (flat layout)
//   <exe dir>/../share/videospeedtracker/  .deb (/usr/bin -> /usr/share) and other FHS prefixes
//   <exe dir>/../Resources/                macOS VSTStudio.app, for VSTStudio and the command-line tools alike: all
//                                          three live in Contents/MacOS (a symlink to one, e.g. in Homebrew's bin,
//                                          works too, since executableDir() resolves links)
fs::path installedFile(const string& rel){
	fs::path exe = executableDir();
	if (exe.empty()) return {};
	fs::path r = fs::path(rel).make_preferred();
	for (const fs::path& candidate : { exe / r, exe / ".." / "share" / "videospeedtracker" / r, exe / ".." / "Resources" / r }){
		error_code ec;
		if (fs::is_regular_file(candidate, ec)) return fs::weakly_canonical(candidate, ec);
	}
	return {};
}

fs::path sampleVideo(){
	return installedFile("sample/manual_20160130172251.avi");
}

#if defined(_WIN32)
namespace {
// A console whose only attached process is us was opened for us by Explorer (double-click), and
// closes the moment we exit.  Keep it up so the messages printed before exit can be read.
void pauseIfOwnConsole(){
	DWORD ids[2];
	if (GetConsoleProcessList(ids, 2) == 1){
		cout << endl << "Press Enter to close this window." << flush;
		cin.clear();
		string line;
		getline(cin, line);
	}
}
}  // namespace

void holdConsoleOnExit(){ atexit(pauseIfOwnConsole); }
#else
void holdConsoleOnExit(){}
#endif

fs::path configDir(){
#if defined(_WIN32)
	return knownFolder(FOLDERID_RoamingAppData) / "VideoSpeedTracker";
#elif defined(__APPLE__)
	return home() / "Library" / "Application Support" / "VideoSpeedTracker";
#else
	fs::path xdg = absoluteEnv("XDG_CONFIG_HOME");
	return (xdg.empty() ? home() / ".config" : xdg) / "videospeedtracker";
#endif
}

fs::path dataDir(){
#if defined(_WIN32)
	return knownFolder(FOLDERID_LocalAppData) / "VideoSpeedTracker";
#elif defined(__APPLE__)
	return configDir() / "Data";
#else
	fs::path xdg = absoluteEnv("XDG_DATA_HOME");
	return (xdg.empty() ? home() / ".local" / "share" : xdg) / "videospeedtracker";
#endif
}

fs::path executableDir(){
	error_code ec;
#if defined(_WIN32)
	wchar_t buf[32768];
	DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
	return n == 0 ? fs::path() : fs::path(wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	string buf(size, '\0');
	if (_NSGetExecutablePath(&buf[0], &size) != 0) return {};
	return fs::canonical(fs::path(buf.c_str()), ec).parent_path();
#else
	return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

string resolveConfig(const string& name){
	error_code ec;
	fs::path local = fs::current_path(ec) / name;
	fs::path user = configDir() / name;
	string chosen;
	if (fs::is_regular_file(local, ec))
		chosen = local.string();
	else if (fs::is_regular_file(user, ec))
		chosen = user.string();
	else {
		fs::path tmpl = installedFile(name);
		if (tmpl.empty()){
			cout << "No " << name << " in the working directory or " << configDir().string()
				<< ", and no installed template to copy.  Pass --config PATH." << endl;
			return "";
		}
		if (!seedConfig(tmpl, user)){
			cout << "Couldn't create " << user.string() << " from " << tmpl.string() << endl;
			return "";
		}
		makeDataDirs(dataDir());
		cout << "Created " << user.string() << " from the installed template; edit it for your camera." << endl
			<< "Data folders (IPCam, HiLites, Stats, Trace) are in " << dataDir().string() << endl;
		chosen = user.string();
	}
	cout << "Using config " << chosen << endl;
	return chosen;
}

}  // namespace userpaths
