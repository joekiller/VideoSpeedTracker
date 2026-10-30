# VST Studio

VST Studio is the desktop app for VideoSpeedTracker. It runs the same tracker as the `VideoSpeedTracker` command
line, and replaces hand-editing `VST.cfg` with a guided setup: plan the camera position, draw the zones on the
picture, calibrate with one drive past, then watch vehicles get measured and review the speeders and wrong-way
drivers. It ships in every release package next to the command-line tools (`VSTStudio`, or **VST Studio** in
the Linux applications menu).

![Live tracking](images/studio/05-live-tracking.jpg)

## Pages

The side rail follows the order a first-time user works in. Every page reads and writes the same `VST.cfg` the
command line uses, so a setup made here works for headless and cron runs too.

### Get started

Six steps for someone who has never set up a speed camera: what equipment to get, picking a spot, setting up
the camera, marking the street, calibrating, and starting.

![Get started](images/studio/01-get-started.jpg)

**Pick your spot** is a site planner. Enter the camera's field of view, resolution, frame rate and shutter, the
distance to the lane and how long a stretch of it is clear. It shows a to-scale top view and works out the
usable timing zone, the frames a car spends in it, the timing and distance error, and motion blur, with a
verdict and a table of worst-case error by speed. The model:

- lane in frame = 2 × distance × tan(field of view / 2), limited by the clear stretch
- timing zone = visible lane − one car length − 0.2 s of run-up for the tracker to lock on
- timing error ≈ speed ÷ frames in the zone (a one-frame slip), distance error ≈ speed × distance error ÷ zone
- blur = speed × shutter time, in pixels at the lane

![Site planner](images/studio/02-site-planner.jpg)

### Video source

Open recorded video (several files run as a batch), a USB camera or a phone in USB webcam mode, or a network
camera's RTSP/HTTP URL. A photo of the view also works for planning zones before the camera is installed. The
preview lists resolution, frame rate, length and codec, with plain-language checks against what the tracker
needs. Without a camera, **Use the sample** opens the 2016 Locust Avenue clip.

![Video source](images/studio/03-source.jpg)

The command line takes the same sources: `--input 0` opens the first camera and `--input rtsp://...` a stream.

### Zones

Drag everything on the picture: the analysis box (edges, corners, or the tab to move it), the two white speed
lines, the lane lines at hubcap height, and the obstruction band for a tree or pole. The inspector shows the
same values as numbers, explains each one, and lists anything the tracker would reject. Unsaved edits are used
by the next run (from a scratch copy, `last-run.cfg` in the config folder), so you can try before you save.

**Your street** sets the compass direction of each lane and whether the street is one-way. On a one-way street
VST flags every vehicle going the other way: the lane on the picture turns red, the run shows an alert, and the
results count wrong-way drivers. This is the `LegalDirection` key in `VST.cfg` (`Both`, `L2R`, `R2L`, or the
compass label of one direction, such as `NE`); the command line prints a `WRONG WAY` line for each.

![Zones](images/studio/04-zones.jpg)

### Calibrate

VST counts frames rather than measuring distance, so it needs one pass at a known speed.

- **Drive-through:** run tracking, drive past on cruise control, pick your pass from the list and enter the
  speed you drove. From the Results page, **This was me** on a selected vehicle does the same.
- **Count frames:** step through a calibration video (←/→, Shift for 10 frames, Space to play), mark the frame
  where the front bumper touches each line, and enter the speed.

Both keep the other direction's calibration valid by expressing it against the same `CalibrationSpeed` and
`CalibrationFPS`. The page shows what crossing times read as which speeds.

![Calibrate](images/studio/07-calibrate.jpg)

### Live tracking

Vehicles are drawn as they're tracked: a box per vehicle, a white bar on the front bumper the tracker times,
and the speed once it crosses the second line, coloured green, amber (over the limit) or red (at or above the
"way over" speed). Wrong-way vehicles are red with a warning. **Tint the motion the tracker sees** overlays the
frame difference the tracker works from. Files play in real time or as fast as the computer allows; each vehicle
lands in the filmstrip with a snapshot, and the side panel keeps counts and the fastest vehicles.

Each run writes the usual outputs under the data folder: `Stats/stats_<name>.csv`, a highlights video in
`HiLites/` (if enabled), and a short clip per vehicle in `Clips/`.

### Results

Filter by speeding, way over, or wrong way; see a speed histogram with the limit marked, every vehicle over
time, the 85th-percentile speed, and a sortable table. Selecting a vehicle shows its snapshot, details and clip.
**Open csv** loads any stats file from an earlier run or from the command line. For recordings named by
Foscam cameras (`manual_yyyymmddhhmmss`), the time each vehicle passed is worked out from the file name.

![Results](images/studio/06-results.jpg)

### Companion app

Turning on the companion starts a small web server on the local network (port 8787) for the phone companion
app: live view with the zones drawn on, calibration from the curb, run control, and recent vehicles with
snapshots and clips. Pairing uses a random token carried in the QR code (`http://<ip>:8787/#token=...`); every
API call needs it, and **New pairing code** signs out every paired phone. The API is described in
[companion-api.md](companion-api.md).

![Companion](images/studio/08-companion.jpg)

## Where things are saved

| | |
|---|---|
| Settings (`VST.cfg`) | the file shown on the Source page; by default the per-user config (see the README) |
| App preferences | `vst-studio.ini` in the same config folder: speed limit, site planner inputs, recent sources, pairing token |
| Results | the data folder (`dataPathPrefix`, or the per-user data folder when blank): `Stats/`, `HiLites/`, `Clips/`, `Trace/` |

## For developers

The GUI is `src/Gui`, built by the main CMake build (turn it off with `-DVST_BUILD_GUI=OFF`). CMake fetches its
dependencies at configure time at pinned versions and links them statically: GLFW 3.4, Dear ImGui 1.91.9b,
portable-file-dialogs, cpp-httplib 0.18.3, nlohmann/json 3.11.3, Nayuki's QR code generator 1.8.0, and the
Inter and Font Awesome Free fonts (compiled in). Linux builds need the X11 and OpenGL headers
(`libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`).

The tracker is embedded, not rewritten: `videoSpeedTracker.cpp` compiled with `VST_EMBEDDED` turns `main()` into
`vst::run()` and calls the hooks in `src/Wintel64/VideoSpeedTracker/Engine.h` where the CLI would show a window,
poll the keyboard or exit. The CLI build is unchanged and the sample-video regression test still guards it.

`VSTStudio --shots DIR` drives every page on the sample video and saves a screenshot of each; CI runs it under
Xvfb (`.github/workflows/build.yml`) and uploads the images, which is how the ones on this page were made.
