# VideoSpeedTracker
The Video Speed Tracker (VST) is an open source, technically sound, vehicle speed measuring system
that can track bidirectional traffic, one lane in each direction (e.g. a typical residential street).
Beyond a typical home computer, VST requires video from an HD camera, many of which can be purchased
for less than $100 (e.g. the Foscam Fi9103EP, power over Ethernet, outdoor camera.  If you buy a POE camera,
be sure to buy something like the TP-LINK TL-PoE150S - about $18 - to power it.).

As of 31 March 2016, watch the use of the speed tracker in support of documenting Charlottesville, Virginia's current struggles with urban vehicular assault in its established residential neighborhoods at http://eyetach.github.io/CharlottesvilleSpeeds/.

*Note:* that site is no longer online; an [archived copy from May 2016](https://web.archive.org/web/20160521015116/http://eyetach.github.io:80/CharlottesvilleSpeeds/)
is available on the Internet Archive.

Video example:

[![Video example](http://img.youtube.com/vi/1HVkKysDvGA/0.jpg)](http://www.youtube.com/watch?v=1HVkKysDvGA "Video Speed Tracker on a 25 MPH Residential Street")

## Set-up Checklist

1. You really want 100 feet or more of open view (a maple tree trunk in the foreground is OK) of
the street. Your speed measuring zone should be at least one second wide for a vehicle going
the speed limit. For a speed limit of 25 MPH, that’s about 37 feet. My speed measuring zone is
1-1/3 seconds wide, which translates to about 50 feet for a vehicle going 25 MPH. I’ve been
able to track vehicles up to 71 MPH (Blue line on the front bumper the whole way!).

2. You need an HD (1280 x 720) video input stream for the stretch of street you want to analyze.
That requires a camera. I’m using the Foscam FI9803EP outdoor, HD, power over ethernet
camera. It’s about $90 online. I have no particular loyalty to Foscam, but I can say I’ve had
about a half dozen of their cameras and only one (an FI9821) has developed an issue (when it
pans, the video signal cuts out). That’s a decent record for an inexpensive cam that does HD
and 30FPS. My 9803EP has been working about four weeks, including through the infamous
Middle Atlantic January 2016 Snowzilla.

   *Note:* the OpenCV 4 build accepts any resolution (frames are scaled to the 1280 x 720 coordinates VST.cfg uses)
   and any frame rate (speeds are scaled by `CalibrationFPS`). 720p or better at 25–30 fps is still what to aim for.

3. Aim the camera well. Aim the camera so that the street passes left to right
about half way up the overall image. This should minimize the effects of lens distortion. A
vehicle moving a constant velocity cross the lens moves at different pixel rates, frame to frame,
as it proceeds from an edge towards the center and back towards the far edge. My tracker is
what’s known as a “piecewise, linear least squares” tracker. The piecewise part (throwing out
the oldest data) is what allows it to adapt to changing pixel rates as a vehicle moves through
the scene. Setting up as you see in Figure 2 of the included overview
([Overview and User Manual](docs/Video%20Speed%20Tracker%20Overview%20and%20User%20Manual.md)) helps the tracker produce high quality results.

4. If you’re recompiling, you’ll need OpenCV 2.4.11 installed to have a successful compile.

   *Note:* the [CMake build](#building-on-linux--macos) added later uses OpenCV 5 (or 4) instead, and the
   [release packages](#downloads) are built on OpenCV 5.

5. Set critical one time setup values in VST.cfg. These values are described in the appendix of the included [pdf](docs/Video%20Speed%20Tracker%20Overview%20and%20User%20Manual_v3.pdf) under docs
([Markdown version](docs/Video%20Speed%20Tracker%20Overview%20and%20User%20Manual.md#appendix)).
   Keys can be in any order, and blank and `#` comment lines are fine. See [Calibrating speed](#calibrating-speed).

6. Create a location (the “prefixPath”) for five directories used by VST (and the final highlights
video processor):

   1. IPCam – You put directories for (typically) daily collections of .avi video files here.

   2. HiLites – VST outputs highlights videos into this directory

      1. forPosting – Subdirectory for output of final highlights video processor

   3. Stats – VST puts csv files with tracked vehicle data here

   4. Trace – VST puts debug files here.

All of the files VST creates are given names derived from the input video file name (or the video file
directory if you answer “*” when asked for which file in a directory full of video files to process.) For
the IPCam directory, I create subdirectories with syntax yyyymmdd (e.g. 20160205) in which I place
that day’s video files.

## Downloads

Prebuilt binaries are attached to each [GitHub release](https://github.com/joekiller/VideoSpeedTracker/releases),
built from the CMake sources against OpenCV 5 by `.github/workflows/release.yml`:

| Platform | File | Install |
|---|---|---|
| Debian 12 / 13, Raspberry Pi OS (64-bit), Ubuntu 24.04 — amd64 and arm64 | `videospeedtracker_<ver>_<distro>_<arch>.deb` | `sudo apt install ./videospeedtracker_*.deb` (OpenCV is built in; apt pulls in FFmpeg and GTK) |
| Windows x64 | `VideoSpeedTracker-<ver>-windows-x64.zip` | unzip anywhere; OpenCV is built in, the FFmpeg and MSVC runtime DLLs sit next to the exes |
| macOS (Apple silicon) | `VideoSpeedTracker-<ver>-macos-arm64.tar.gz` | `brew install opencv`, then untar |

To cut a release, push a version tag (`git tag v1.0.0 && git push origin v1.0.0`) or create a release with a new `v*`
tag in the GitHub UI. The workflow builds and tests every platform, then creates the release (or adds to it) with the
packages and a `SHA256SUMS` file.

### Verifying a download

Every package is signed by GitHub at build time with an [artifact attestation](https://docs.github.com/en/actions/security-for-github-actions/using-artifact-attestations)
(Sigstore-backed SLSA build provenance), which proves the file was built by this repository's release workflow from the
tagged commit. With the [GitHub CLI](https://cli.github.com/):

```
gh attestation verify videospeedtracker_1.0.0_debian-bookworm_arm64.deb -R joekiller/VideoSpeedTracker
```

`SHA256SUMS` lists every file's checksum (`sha256sum -c SHA256SUMS --ignore-missing`). These attestations are not
Windows Authenticode or Apple notarization, so Windows SmartScreen and macOS Gatekeeper still warn on first launch
(on macOS, clear it with `xattr -d com.apple.quarantine VideoSpeedTracker ProcessHiLites`).

### Code signing (optional)

Releases build and publish unsigned until these are configured; each platform's signing steps switch on by
themselves once its secrets exist (repo **Settings → Secrets and variables → Actions**).

**Windows — Azure Artifact Signing (~$9.99/month).** Individuals in the US/Canada qualify.
1. Create an Azure subscription, then an Artifact Signing account and an identity validation (Public Trust,
   individual; takes a few days) and a certificate profile.
2. Create an app registration with a federated credential for this repo (GitHub OIDC), and give it the
   *Artifact Signing Certificate Profile Signer* role on the account.
3. Secrets: `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`. Variables: `AZURE_SIGNING_ENDPOINT`
   (for example `https://eus.codesigning.azure.net/`), `AZURE_SIGNING_ACCOUNT`, `AZURE_CERT_PROFILE`.

SmartScreen still warns until the signing identity builds reputation over a few releases.

**macOS — Apple Developer Program ($99/year).**
1. Enroll as an individual, create a *Developer ID Application* certificate and export it with its key as a .p12.
2. Create an app-specific password for your Apple ID.
3. Secrets: `MACOS_CERT_P12_BASE64` (`base64 -i cert.p12`), `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`,
   `APPLE_APP_PASSWORD`.

The binaries are then signed with the hardened runtime and notarized.

### Where config and data live

Without `--config`, each program uses `VST.cfg` / `ProcessHiLites.cfg` from the working directory if one is there
(the original 2016 workflow), otherwise its per-user copy. The first run creates that copy from the template installed
with the program, points its `dataPathPrefix` at the per-user data folder, and creates `IPCam`, `HiLites/forPosting`,
`Stats` and `Trace` inside it. The install folder is never written, so the same binaries work from a .deb, an unzipped
folder, or a Store/MSIX or `.app` package.

| | Config | Data |
|---|---|---|
| Linux | `$XDG_CONFIG_HOME/videospeedtracker` (`~/.config/videospeedtracker`) | `$XDG_DATA_HOME/videospeedtracker` (`~/.local/share/videospeedtracker`) |
| Windows | `%APPDATA%\VideoSpeedTracker` | `%LOCALAPPDATA%\VideoSpeedTracker` |
| macOS | `~/Library/Application Support/VideoSpeedTracker` | `~/Library/Application Support/VideoSpeedTracker/Data` |

Each run prints the config file it used.

### Licenses

VST is BSD 3-Clause ([LICENSE](LICENSE)); every package includes it. The Linux and Windows packages link OpenCV
(Apache 2.0) and its bundled third-party code statically, and the Windows zip also ships OpenCV's FFmpeg plugin
(LGPL 2.1); their license texts are in `licenses/opencv` (Windows) or `/usr/share/doc/videospeedtracker/opencv`
(Linux). FFmpeg and GTK on Linux, and OpenCV on macOS, come from apt or Homebrew with their own licenses.

The original 2016 Win7 x64 executables and OpenCV 2.4.11 DLLs that used to live in `bin/` have been removed; they are
still in the git history before the release workflow was added. Every package contains both `VideoSpeedTracker` and
the highlights editor `ProcessHiLites`, with their sample configs.

## Building on Linux / macOS

The sources under `src/Wintel64` build with CMake against OpenCV 5 or 4. The distro's OpenCV 4 is the quickest
start (tested with 4.6 on Ubuntu 24.04); `packaging/build-opencv.sh <prefix>` builds the static OpenCV 5 the
release packages use, then pass `-DCMAKE_PREFIX_PATH=<prefix>`. On macOS, `brew install opencv` gives 5.x.

```
sudo apt-get install libopencv-dev cmake g++
cmake -S . -B build && cmake --build build -j
ctest --test-dir build          # runs the bundled 2016 sample video and checks speeds against tests/baseline_*.csv
```

Run with no arguments for the original interactive mode. Pass options to skip the prompts and run without windows:

```
build/VideoSpeedTracker --headless \
  --input sampleData/IPCam/20160130/manual_20160130172251.avi \
  --config bin/VST.cfg --stats stats.csv --speed-limit 25 \
  --highlights hilites.mp4 --hl-min 35
```

`--help` lists every option. The 8.6 minute sample processes in about 20 seconds headless (roughly 25× real time).

`ProcessHiLites` (the highlights editor, also built by CMake) steps through a highlights video one vehicle at a time
and writes the ones you keep, cropped to 640×360, to `forPosting/forPost_<name>`. It needs a display. With no arguments
it reads `ProcessHiLites.cfg` from the working directory and offers the videos in `<dataPathPrefix>/HiLites`; or name
the files directly:

```
build/ProcessHiLites --config bin/ProcessHiLites.cfg --input hilites.mp4 [--output keepers.mp4]
```

## Calibrating speed

VST doesn't know distances. It measures how many frames a vehicle takes to cross the speed zone between the two white
lines and compares that with a calibration run:

```
speed = CalibrationSpeed × CalibrationFrames × (video fps / CalibrationFPS) / frames taken
```

Record yourself driving through the zone in each direction at a steady, known speed, count the frames each pass took,
and put them in `CalibrationFramesL2R` / `CalibrationFramesR2L`, with the speed you drove as `CalibrationSpeed` (default
25 MPH) and the camera's frame rate as `CalibrationFPS` (default 30). The speed limit you give VST (`--speed-limit` or
the interactive prompt) only colors the results (green at or under, yellow over, red at or above the egregious bound).
It does not change the measured speeds.

## Troubleshooting

These come from the [issues filed against the original project](docs/upstream-issues.md).

| Symptom | Cause and fix |
|---|---|
| Interactive mode seems to hang at start | The 2016 code looped forever when `<dataPathPrefix>/IPCam` was missing or empty. It now says what's missing and exits. Put each day's videos in their own folder under `IPCam`, point `dataPathPrefix` somewhere else, or skip the prompts with `--input`. |
| Exits right after reading the config, or "Frame width is not 1280" / "Frame rate is not 30" | The 2016 code accepted only exactly 1280x720 at exactly 30.000 fps, so 1080p, 29.97 fps and phone video all quit. Any size and rate now work. |
| "Can't open video" | The file is missing or this OpenCV can't decode it. Convert it with `ffmpeg -i in.ext -c:v libx264 out.mp4`. |
| Crash partway through an H.264 file | Likely cause: the 2016 loop trusted the container's frame count and read past the last real frame. VST now reads until the video ends. |
| "unknown setting" or "can't read a value" | Typo in VST.cfg. The message gives the file and line number. |
| Speeds look scaled by a constant factor | Check `CalibrationSpeed`, `CalibrationFPS` and the two `CalibrationFrames` values (see above). |

VideoSpeedTracker prints every error to stderr and exits with status 1 on any error, so scripts and cron jobs can check `$?`.

## Additional Documentation

- A much more detailed version of this readme is available
[**here.**](docs/Video%20Speed%20Tracker%20Overview%20and%20User%20Manual.md)
- PDF version of the manual: [Video Speed Tracker Overview and User Manual_v3.pdf](docs/Video%20Speed%20Tracker%20Overview%20and%20User%20Manual_v3.pdf)
- [Excessive City Vehicle Speeds As Observed from Locust Avenue](docs/forCouncil_v2.pdf) (February 2016 analysis
of December 2015 – January 2016 data)

## Original Author and Development Lead
- Paul Reynolds (reynolds@virginia.edu) www.cs.virginia.edu/~pfr

## License

- BSD 3-Clause, see [LICENSE](LICENSE)
