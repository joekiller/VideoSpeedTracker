# Getting started with VideoSpeedTracker

This guide takes a freshly downloaded release from "does it run?" to measuring speeds on your own street. Release
packages install it as `GETTING-STARTED.md` next to `README.md`.

## 1. See it work on the sample video

Every release package bundles a 2016 clip from Locust Avenue in Charlottesville (`sample/manual_20160130172251.avi`),
and the `VST.cfg` it ships with is set up for that clip. Run either of these:

```
VideoSpeedTracker --demo              # tracks the sample in a window; Esc quits
VideoSpeedTracker --demo --headless   # no window, just the results
```

The demo writes a csv of every vehicle it tracked to the `Stats` folder in your per-user data folder
(see [Where things go](#where-things-go)). It prints the file's path when it starts.

If you run `VideoSpeedTracker` with no options, you get the original interactive mode. While you have no videos of
your own yet, it says where to put them and then offers to run the sample. Press Enter to accept the default answer
to each question.

On Windows you can also double-click `VideoSpeedTracker.exe`. The window stays open after the program exits, so you
can read why it stopped.

## 2. Where things go

| | Config (`VST.cfg`) | Data (`IPCam`, `Stats`, `Trace`, `HiLites`) |
|---|---|---|
| Windows | `%APPDATA%\VideoSpeedTracker` | `%LOCALAPPDATA%\VideoSpeedTracker` |
| macOS | `~/Library/Application Support/VideoSpeedTracker` | `~/Library/Application Support/VideoSpeedTracker/Data` |
| Linux | `~/.config/videospeedtracker` | `~/.local/share/videospeedtracker` |

The config is chosen in this order:

1. `VST.cfg` in the folder you run from. On Windows that includes the unzipped program folder, so if you start
   the program there, edit the `VST.cfg` next to the exe.
2. Your per-user copy. The first run without (1) creates it from the shipped template.
3. Whatever you pass with `--config PATH`, which overrides both.

Every run prints `Using config <path>` first, so you can always see which file it read. The shipped `dataPathPrefix`
is blank, which means "the per-user data folder". Set it to a path to keep videos and results somewhere else, such
as a big drive.

## 3. Check your camera position

Before calibrating, make sure the view can work. The full checklist is in the [README](../README.md#set-up-checklist).
In short:

- The street should cross the frame left to right, about halfway up.
- The speed zone, the stretch between the two white lines, should take a car at least one second to cross at
  the speed limit. At 25 mph that is about 37 ft.
- Use 720p or better at 25 to 30 fps. Any size and frame rate is accepted: frames are scaled to 1280x720, and
  every pixel value in `VST.cfg` uses that 1280x720 grid.

## 4. Set the geometry for your view

Record a minute of ordinary traffic and open one frame in an image viewer that shows pixel coordinates (Paint, GIMP,
Preview's inspector, `ffmpeg -i clip.mp4 -frames:v 1 -s 1280x720 frame.png`). Measure in a 1280x720 copy of the
frame. Then set these keys in your `VST.cfg`:

| Key | What to measure |
|---|---|
| `AnalysisBoxLeft`, `AnalysisBoxTop` | Top-left corner of a box that holds both lanes and the cars' full height, and as little else as possible: no sky, no swaying trees, no sidewalk. |
| `AnalysisBoxWidth`, `AnalysisBoxHeight` | Size of that box. Left + Width must be 1280 or less, and Top + Height must be 720 or less. |
| `speedLineLeft`, `speedLineRight` | x of the two speed-zone lines, **relative to `AnalysisBoxLeft`**. Keep them where cars are fully in view and in the flattest, middle part of the frame. |
| `R2LStreetY`, `L2RStreetY` | y of the hubcap line for each lane, **relative to `AnalysisBoxTop`**. R2L is usually the far lane, so it is higher, which means a smaller y. |
| `obstruction` | `[left,right]` x range, relative to `AnalysisBoxLeft`, of a pole or tree trunk in front of the street. Use `[0,0]` for none. |
| `nextHeight` | Rough height in pixels of a typical car in your view. |
| `L2RDirection`, `R2LDirection` | Compass direction for each way of travel. These are only labels in the output. |

Run a clip interactively with `VideoSpeedTracker` (put it in `IPCam/<a folder>/`), or pass it with
`VideoSpeedTracker --input clip.mp4`. Interactive mode draws the box, speed lines, obstruction and hubcap lines over
the first frame and asks whether they look right. Answer `n`, adjust, and run again until they do. The colors are
explained in the [manual's appendix](Video%20Speed%20Tracker%20Overview%20and%20User%20Manual.md#appendix).

## 5. Calibrate speed

VST counts how many frames a vehicle takes to cross the speed zone and compares that with a known pass:

```
speed = CalibrationSpeed × CalibrationFrames × (video fps / CalibrationFPS) / frames taken
```

1. Drive (or have someone drive) through the zone at a steady, known speed, with cruise control or a GPS speedometer
   app, once in each direction you need. On a one-way street you only need the legal direction, but a pass the
   other way (slowly, on a closed or empty street) lets you calibrate wrong-way speeds too.
2. Run VST on that clip, for example `VideoSpeedTracker --headless --input calib.mp4 --stats calib.csv`. The
   `# Frames` column of your car's row is the number of frames it took to cross the zone. If VST didn't track the
   pass, step through the clip in a video player and count the frames between the front bumper crossing the first
   white line and crossing the second.
3. Put that count in `CalibrationFramesL2R` or `CalibrationFramesR2L`, the speed you drove in `CalibrationSpeed`,
   and the clip's frame rate in `CalibrationFPS`.
4. Do a second pass at a different speed and check that VST reports it within a mile or two per hour.

If a direction can't be calibrated, a rough value is the other direction's frame count scaled by how much shorter
or longer the zone looks in that lane.

## 6. Analyze your videos

- Interactive: put each day's clips in `IPCam/<yyyymmdd>/` and run `VideoSpeedTracker`. Answer `*` to process a whole
  folder.
- Batch or scripted: run `VideoSpeedTracker --headless --input clip.mp4 --stats out.csv --speed-limit 25`.
  `--highlights hl.mp4` saves a video of the offenders. See `VideoSpeedTracker --help` for every option.

Each csv row is one vehicle, with its direction, frame range, pixel distance, size, estimated speed and the date and
time it passed (the columns are described in the [README](../README.md#the-stats-csv)).

## Example: a narrow view of a one-way street

This example is for a second-floor window on a one-way street that runs northeast, where legal traffic moves
right to left across the frame. The view is short, so the speed zone is narrow and the camera should record at a
full 30 fps. These numbers are placeholders to show the shape of the config. Measure your own.

```
# Second-floor window, one-way street, legal direction NE = right to left in the frame.
# L2R traffic is therefore going the wrong way; its rows in the stats are the wrong-way callouts.
dataPathPrefix =
L2RDirection = "SW"          # wrong way
R2LDirection = "NE"          # legal direction
obstruction = [0,0]          # nothing in front of the street
AnalysisBoxLeft = 0
AnalysisBoxTop = 300         # below the porch roof and across-the-street yards; measure yours
AnalysisBoxWidth = 1280
AnalysisBoxHeight = 220
speedLineLeft = 400          # zone must still take >= 1 s at the limit: about 37 ft at 25 mph
speedLineRight = 880
R2LStreetY = 150             # one lane: put both hubcap lines on it
L2RStreetY = 150
nextHeight = 110             # cars look bigger from close up
CalibrationFramesL2R = 30    # placeholder until a wrong-way calibration pass is done
CalibrationFramesR2L = 30    # count from your own calibration drive
CalibrationSpeed = 25
CalibrationFPS = 30
```

On a one-lane street both directions use the same lane, so the two hubcap lines are close together or the same.
VST was built for two-lane, two-way streets, so expect to tune `maxL2RDistOnEntry`, `maxR2LDistOnEntry` and `SLOP`
for a narrow view.

## When something goes wrong

See [Troubleshooting](../README.md#troubleshooting) in the README. The most common first-run messages:

- **No video folders found in …\IPCam**: you have no videos there yet. Accept the offer to run the sample, put a
  folder of clips there, or use `--input`.
- **unknown setting / can't read a value**: there is a typo in `VST.cfg`. The message gives the file and line.
- **Can't open video**: OpenCV can't decode the file. Convert it with `ffmpeg -i in.ext -c:v libx264 out.mp4`.
