# Upstream issues, pull requests and forks

A review, done in October 2026, of everything filed against the original project
[pfr/VideoSpeedTracker](https://github.com/pfr/VideoSpeedTracker) and of the changes in its forks, checked against this
fork. "Fixed here" means the fix is in this repository. Where a report had no logs, the cause given is the most likely
one found in the 2016 code, and the entry says so.

## License

The original project is BSD 3-Clause. Every source file carries the full notice (copyright Paul Reynolds, Charlottesville,
2016), and the original README says "BSD 3-Clause" under License. The original repository has no separate LICENSE file;
[LICENSE](../LICENSE) here is that same header text.

## Issues

| # | Report | Status here |
|---|---|---|
| [1](https://github.com/pfr/VideoSpeedTracker/issues/1) | Get code into repo, write external docs | Done upstream in 2016. |
| [3](https://github.com/pfr/VideoSpeedTracker/issues/3) | The Win7-built exe with OpenCV 2.4.11 DLLs fails on Windows 8.1 / 10 (screenshots only, no error text) | The 2016 exe and DLLs are gone. The release workflow builds a Windows x64 zip with MSVC and a statically linked OpenCV 5, and ships OpenCV's FFmpeg plugin DLL and the MSVC runtime DLLs alongside the exes. |
| [13](https://github.com/pfr/VideoSpeedTracker/issues/13) | Where does the speed limit in the speed formula come from? | Never answered upstream. The 2016 formula hard-coded 25 MPH as the calibration speed, so the speed-limit prompt only changed colors. **Fixed here:** new `CalibrationSpeed` key (default 25), documented under "Calibrating speed" in the README. |
| [14](https://github.com/pfr/VideoSpeedTracker/issues/14) | Logitech C920 video: the program closes with no error, VS build has link errors, and an H.264 1280x720 file crashes after "Trying to capture" | Four separate problems. (1) Only exactly 1280x720 at exactly 30.000 fps was accepted, and the "Bailing" message had no newline, so the window closed before it showed. **Fixed here:** any size is scaled to 1280x720 and any frame rate is handled by `CalibrationFPS`. (2) There was no VS project, so the build failed at link time: the CMake build replaced it. (3) The H.264 crash is not reproducible without the file. Most likely cause: the loop stopped at `CAP_PROP_FRAME_COUNT - 2`, which is only an estimate for many H.264 files, so it could read empty frames and crash in `cvtColor`. **Fixed here:** the loop reads until the video ends. (4) Errors now go to stderr with the values involved, and the exit status is non-zero. |
| [15](https://github.com/pfr/VideoSpeedTracker/issues/15) | Has anyone ported this to Mac? | Yes: the CMake build (PR #1) and the macOS arm64 release package. |
| [16](https://github.com/pfr/VideoSpeedTracker/issues/16) | "App just hangs, Windows 10" (screenshot only) | Most likely cause: with no arguments, VST ran `dir <dataPathPrefix>\IPCam` and asked y/n about each folder, and it looped forever when the folder was missing or empty. The sample config points at `g:\locustdata`, so on most machines it was missing. **Fixed here:** the folder is listed with `std::filesystem` and VST exits with a message saying what to create. VST also exits instead of re-prompting when stdin closes. |

## Pull requests

| # | Change | Status here |
|---|---|---|
| 2, 4, 5 | README and extended docs | Merged upstream. Later docs fixes are in this fork's PR #2. |
| 6, 7 | Added VC12 OpenCV libraries and code fixes for them | Merged upstream. Superseded: no libraries are committed now, and builds get OpenCV from the platform. |
| 8, 10, 12 | piki's "Make it build on Mac and Linux" (three attempts) | Each was merged and reverted, or closed, because it risked breaking the Windows build. Its content is in piki's fork, covered below. |
| 9, 11 | Reverts of 8 and 10 | n/a |

## Forks

There are 10 forks. Only two contain commits that are not in the original repository.

### piki/VideoSpeedTracker (Patrick Reynolds, 2016–2017)

| Change | Status here |
|---|---|
| Makefiles for Mac and Linux, `DIR_SEP` paths, `readdir` instead of `system("dir")` | Superseded by CMake (PR #1). PR #4 replaced `system("dir")` with `std::filesystem`. |
| Fix infinite loop when the directory list is empty or stdin hits EOF | **Fixed here** (see issue 16). |
| Don't require FPS == 30.0 exactly | **Fixed here**, and generalised to any frame rate. |
| Handle blank lines and `\r` in the config; allow comments anywhere; allow keys in any order; table-driven config | **Fixed here.** The parser is table-driven, any order, case-insensitive, allows blank and comment lines, CRLF, and a final newline, and reports the file and line for unknown keys or bad values. The 2016 parser rejected any config ending in a newline, which most editors add. That also broke the per-user config seeded on first run. |
| Handle empty frames gracefully | **Fixed here.** `readFrame()` stops at the first empty frame. |
| Command-line options instead of prompts | Done in PR #1 (`--input`, `--headless`, ...). PR #4 added `--opt=value` and rejects non-numeric values with a message instead of crashing. |
| `~/.vstrc` instead of `./VST.cfg` | Superseded by PR #3's per-user config folders. |
| Shorter display delays (50 ms per frame pair while a car is shown) | **Adopted here.** The 1250 ms default paused for 1.25 s on every frame pair with a vehicle in view. `s` still slows playback down. |
| `arrowedLine` shim for OpenCV 2.4.9 | Not needed with OpenCV 4 or 5. |
| Fix signed/unsigned comparisons | Warnings only. Not taken separately. |
| Inverness 2017 site config | Site-specific. Not taken. |

### SecureBot/VideoSpeedTracker and pfr's `neighborhoods` branch (2016)

| Change | Status here |
|---|---|
| VC++ 2013 project files, OpenCV 3.1 transition | Superseded by CMake and OpenCV 4/5. |
| Per-neighborhood data folders (Locust, OLR, Rugby) and an `AutoProcessHiLites` tool that picks highlights automatically | Not ported. `AutoProcessHiLites` is a candidate for unattended batch runs, but it is a separate 580-line Windows/OpenCV 2.4 program. |
| Measure vehicle area at the speed-zone midpoint instead of the analysis-box midpoint | **Adopted here.** On a narrow view where the speed zone isn't centred, the 2016 code could finish a vehicle before ever measuring it, and it reported area 0. On the sample video this changes a few area values by 1 to 3 and no speeds. |
| If a vehicle leaves the zone before its area was measured (long trucks), use the last observed box | **Adopted here.** |
| Different area normalisation per lane (/375 near lane, /270 far lane) | Not adopted. The author marked it "Experimental", and it would move `largeVehicleArea` thresholds for existing configs. |
