# VST companion API (v1)

The local HTTP API that the desktop app (VST Studio) serves for the phone companion in
[`companion/`](../companion/README.md). This file is the contract between the two: the desktop app
implements it, the companion consumes it, and [`companion/dev/mock-vst.mjs`](../companion/dev/mock-vst.mjs)
implements it with simulated data so either side can be tested alone.

## Transport and pairing

* Off by default. The user turns on the companion on VST Studio's **Companion app** page, which then listens on
  `http://<lan-ip>:8787/` and shows that URL as a QR code with a pairing token in the fragment:
  `http://192.168.1.20:8787/#token=Zx8…`. The fragment never reaches the server or any log; the
  companion stores the token on the phone and strips it from the address bar.
* `GET /` and other non-`/api` paths serve static files from the `companion` folder next to the
  executable, so the phone loads the companion from the same origin as the API.
* Every `/api/*` call except `/api/hello` needs the token, either as `Authorization: Bearer <token>`
  or, for `<img>`/`<video>` sources and `EventSource` (which cannot set headers), as `?token=<token>`.
* A bad or missing token gets `401` with `{"error": "…"}`. Every HTTP error (4xx, 5xx) is JSON
  `{"error": "message"}`: `400` for a body that isn't a JSON object or an unknown action, `404` for
  anything that isn't there (an unknown route, a vehicle or its media that is gone, a path outside
  the `companion` folder), `503` when VST Studio didn't get to the request within 5 s.
* Requests that are understood but refused (settings that don't validate, a calibration that can't
  be applied) are not HTTP errors: they answer `200` with `{"ok": false, "problems": ["…"]}`, so the
  companion can show the problems in place.
* JSON bodies are UTF-8 `application/json`. CORS is allowed (`Access-Control-Allow-Origin: *`,
  `Authorization` and `Content-Type` headers, `GET, PUT, POST, OPTIONS`), so a companion served from
  elsewhere (a dev server, `localhost`) can reach it.
* Coordinates are always in VST's 1280x720 frame space, whatever the camera's size.
* Speeds are whole mph, as in the stats csv.
* It is plain HTTP on the local network. Reaching it from outside the house is left to the user
  (for example a Tailscale tailnet); the API itself never calls out.

## Endpoints

### `GET /api/hello` (no auth)

```json
{ "app": "VST", "version": "1.1.0", "auth": true }
```

Lets the companion tell "found VST, needs pairing" apart from "nothing at this address".

### `GET /api/status`

```json
{
  "version": "1.1.0",
  "running": true,
  "paused": false,
  "reconnecting": false,
  "source": { "kind": "camera", "name": "Front window", "fps": 30, "width": 1280, "height": 720 },
  "frame": 81234,
  "totalFrames": 0,
  "procFps": 29.7,
  "counts": { "vehicles": 412, "speeders": 97, "egregious": 6, "wrongWay": 3, "topSpeed": 47 },
  "speedLimit": 25,
  "egregious": 40,
  "legalDirection": "R2L",
  "siteName": "7½ St SW"
}
```

* `source` is `null` when nothing is open. `source.kind` is `file`, `camera`, `stream` or `image`
  (a still opened for planning zones; it can't be tracked); `totalFrames` is 0 for live sources.
* `reconnecting` is true while a camera or stream has stopped sending pictures and the tracker is reopening it
  (after 1, 2, 4, 8, 16, then every 30 s). Passes resume when it's back.
* `counts` cover the current session (since VST Studio started). `vehicles` are the ones logged (timed, a clean track, at least
  `minLoggedSpeed`). Studio also reports `tries` (every car the tracker followed into the speed zone), split into
  `vehicles`, `slow` (timed but under `minLoggedSpeed`) and `untimed` (lost before a usable speed).
* `liveViews` is how many streams (`/api/stream.mjpg`, `/api/live`) are open, out of 4 (see
  [Stream limits](#stream-limits-and-signing-out)).
* `legalDirection` is `L2R`, `R2L` or `both` (two-way street, nobody is ever wrong-way).
* VST Studio also reports `counts.avgSpeed` (mph, not rounded), `batch` (`{"index", "count"}`: which
  of the queued videos is being processed) and `calibrationMarks` (`{"L2R": [start, end], "R2L": […]}`,
  the hand-marked frames, `-1` when unmarked). The companion doesn't need them; the mock reports them too.
* `calibration` is how far each direction's calibration has been shown to hold (see [Calibration](#calibration)):

  ```json
  "calibration": {
    "L2R": { "state": "verified", "error": 0.3, "applied": "2026-11-02T10:14:03-05:00", "checked": "2026-11-02T10:21:40-05:00", "frames": 34 },
    "R2L": { "state": "unchecked", "error": null, "applied": null, "checked": null, "frames": 40 }
  }
  ```

  `state` is `none` (not calibrated), `unchecked` (calibrated, no check since), `verified` (the latest check
  after the latest calibration was within ±2 mph) or `failed` (it was off by more). `error` is that check's
  `measuredMph - drivenMph`, and `checked` its time; both are `null` unless `state` is `verified` or `failed`.
  `applied` is the time of the latest calibration on record, `null` for one made before VST kept a history (the
  state is then `unchecked` rather than `none`). `frames` is `CalibrationFrames<dir>` now in force. A check only
  counts while that value is the one it was made against: changing it any other way (`PUT /api/config`,
  editing `VST.cfg`) puts the direction back to `unchecked`. VST Studio works this out from
  `calibration-history.csv` in its config folder, so it survives restarts.

### `GET /api/health`

How an unattended run is holding up (see "Leaving it running" in [vst-studio.md](vst-studio.md)):

```json
{
  "today":  { "date": "2026-11-02", "trackedHours": 9.4, "outageHours": 0.05, "outages": 1,
              "vehicles": 412, "clipsRemoved": 0, "camera": "same" },
  "disk":   { "videoBytes": 3435973837, "videoFiles": 412, "budgetBytes": 21474836480,
              "freeBytes": 195421011968, "lowDisk": false },
  "camera": { "result": "same", "warning": false, "dx": 0.4, "dy": -0.2, "confidence": 0.91,
              "checked": "2026-11-02T17:20:00" }
}
```

* `today` counts live cameras and streams only. It's the row `Stats/health.csv` gets when the day ends.
* `budgetBytes` is `null` with no limit, and `freeBytes` is `null` until the first check (seconds after start).
* `camera.result` is `unchecked`, `same`, `moved` or `unclear` (too dark or featureless to tell). `warning` is
  true after three `moved` checks in a row, about 6 minutes. `dx`/`dy` are in 1280x720 frame pixels.
* `/api/status` carries a summary of this as `health: {"camera", "cameraWarning", "lowDisk"}`.

### `GET /api/vehicles?since=<id>&from=<iso>&to=<iso>&limit=<n>`

Logged passes, newest first. All parameters are optional: `since` returns only ids greater than it,
`from`/`to` bound `time`, `limit` defaults to 100 and may go up to 5000.

```json
[
  {
    "id": 1042,
    "time": "2026-10-30T17:22:51-04:00",
    "video": "camera",
    "frame": 6256,
    "dir": "R2L",
    "compass": "NE",
    "wrongWay": false,
    "speed": 38,
    "speeding": true,
    "egregious": false,
    "area": 63,
    "large": false,
    "frames": 30,
    "startFrame": 6194,
    "endFrame": 6224,
    "fps": 30,
    "snapshot": "/api/vehicles/1042/snapshot.jpg",
    "clip": "/api/vehicles/1042/clip.mp4"
  }
]
```

* `dir` is the screen direction; `compass` is the configured heading for it (`L2RDirection`/`R2LDirection`).
* `wrongWay` is true when `legalDirection` is not `both` and `dir` differs from it.
* `speeding` is `speed > speedLimit`; `egregious` is `speed >= egregious`.
* `frames` is the number of frames the vehicle took to cross the speed zone, measured at `fps`.
* `clip` is a short (2-4 s) mp4 of the analysis-box strip from entry to exit, or `null` when none is
  kept (for example after a restart). `snapshot` is always present.

### `GET /api/vehicles/<id>`

One pass, same object as the list items. `404` JSON when gone.

### `GET /api/vehicles/<id>/snapshot.jpg`, `GET /api/vehicles/<id>/clip.mp4`

Binary media for one pass. `404` JSON when gone.

### `GET /api/frame.jpg?overlay=0|1`

The latest camera frame as JPEG. `overlay=1` draws the zones and tracked boxes the way the desktop
does. The companion asks for `overlay=0` and draws its own editable overlay.

### `GET /api/stream.mjpg`

The live view as `multipart/x-mixed-replace` JPEG frames (about 10 fps is plenty), with the same
`overlay` parameter.

### Stream limits and signing out

* At most 4 streams (`/api/stream.mjpg` and `/api/live` together) are open at once. Each holds one of
  VST's request threads while it's open, so the limit keeps every other call answering however many
  tabs are left open. The next stream gets `503` with `{"error": "…"}`; the companion then falls back
  to polling.
* **New pairing code** on the desktop ends every open stream within about a second, and the
  reconnect gets `401`. The companion treats that as signed out and goes back to Connect.

### `GET /api/live` (Server-Sent Events)

`text/event-stream` that pushes changes so the phone needs no polling. Events:

| event | data | when |
|---|---|---|
| `status` | same body as `/api/status` | about once a second |
| `vehicle` | one `/api/vehicles` item | each time a pass is logged |
| `tracks` | `[{"id", "dir", "x", "y", "w", "h", "speed"}]` | optional, ~5/s, vehicles currently being tracked (frame coords; `speed` 0 until known) |
| `config` | same body as `GET /api/config` | when the stream opens, then within about a second of any config change, from any client (the desktop included) |

Between events VST also sends an SSE comment (`:`) about every 200 ms, which EventSource ignores. It lets
VST notice a closed tab within a fraction of a second and give its stream slot back.

Without `/api/live` the companion falls back to polling `/api/status` every 2 s and
`/api/vehicles?since=` every 5 s.

### `GET /api/config`, `PUT /api/config`

The tracker's zones, calibration and directions as JSON, using the `VST.cfg` key names, plus the
companion-facing settings:

```json
{
  "AnalysisBoxLeft": 10, "AnalysisBoxTop": 220, "AnalysisBoxWidth": 1269, "AnalysisBoxHeight": 190,
  "speedLineLeft": 350, "speedLineRight": 909,
  "obstruction": [251, 311],
  "R2LStreetY": 122, "L2RStreetY": 158, "R2LStreetYRight": 0, "L2RStreetYRight": 0, "streetEdge": [0, 0],
  "L2RDirection": "SW", "R2LDirection": "NE",
  "CalibrationFramesL2R": 35, "CalibrationFramesR2L": 40, "CalibrationSpeed": 25, "CalibrationFPS": 30,
  "legalDirection": "R2L",
  "speedLimit": 25, "egregious": 40, "siteName": "7½ St SW",
  "minLoggedSpeed": 18,
  "subFrameTiming": 1, "maxL2RDistOnEntry": 440, "maxR2LDistOnEntry": 440, "entryLookBack": 1000,
  "levelDegrees": 0
}
```

`minLoggedSpeed` (mph, `VST.cfg`'s key of the same name) is the slowest timed vehicle written to the
stats csv; slower ones are only counted in the run manifest. 0 logs every timed vehicle; negative values
are refused.

`subFrameTiming` (0 or 1) turns on short-view timing for a view of about 14 to 25 ft of street, where a car
fills most of the frame. `maxL2RDistOnEntry` and `maxR2LDistOnEntry` are the tracker's look-ahead, the
pixels a car moves in two frames at the fastest speed it tracks, and `entryLookBack` is how far back it
looks for a car entering the box. A short view needs several hundred pixels for each; Studio's site
planner works them out from the camera and street. See `VST.cfg`'s keys of the same names.

`levelDegrees` (-15 to 15) turns every frame counter-clockwise by that much before tracking, for a camera that
isn't mounted level. `/api/stream.mjpg` shows the turned picture, so lines drawn on it
line up with what the tracker sees.

As in `VST.cfg`, `speedLineLeft`, `speedLineRight` and `obstruction` are x offsets from
`AnalysisBoxLeft`; `R2LStreetY` and `L2RStreetY` are y offsets from `AnalysisBoxTop`. `obstruction`
of `[0, 0]` means none.

`R2LStreetY` and `L2RStreetY` are where each lane line meets the analysis box's left edge;
`R2LStreetYRight` and `L2RStreetYRight` are where it meets the right edge, for a street that runs at an
angle across the picture (0 = level, the same as the left end). `streetEdge` (`[left, right]`, the same
way; `[0, 0]` = off) is an optional line along the curb: where it closes in on a lane line tells
short-view timing how the street recedes, so it measures each car along the ground. See `VST.cfg`'s keys
of the same names and `Perspective.h`.

`GET` also returns, read-only, `obstruction_extent`, `SENSITIVITY_VALUE`, `BLUR_SIZE` and
`largeVehicleArea` (the `VST.cfg` values) and `frameWidth`/`frameHeight` (always 1280 and 720).
`PUT` ignores keys it doesn't know, so a client may send back a whole `GET` body.

`PUT` takes any subset of these keys and validates the whole result. If it is valid, VST saves it
at once: `legalDirection` to `VST.cfg` as `LegalDirection` along with the tracker keys, and
`speedLimit`, `egregious` and `siteName` to the desktop app's own settings. A run already in
progress keeps the settings it started with; the new ones take effect the next time tracking
starts. If it isn't valid nothing changes, and the answer is still `200`:

```json
{ "ok": false, "problems": ["speedLineRight must be greater than speedLineLeft"], "config": { … } }
```

`config` is always the configuration now saved (unchanged when `ok` is false), and `problems` is
`[]` when `ok` is true.

### `GET /api/plan`, `PUT /api/plan`

VST Studio's site planner (step 2, Pick your spot), so the phone can plan the street, size it from the picture and
place it on the map.  The planner edits a **draft**: every change is a preview (on the phone and on the computer's
page alike) until it is saved with `{"save": true}`, here or by Save in VST Studio, or dropped with `{"discard": true}`.
With autosave on (`{"autosave": true}`, off by default) each change is saved as it is made.  The draft survives a
restart of VST Studio.

```json
{
  "street": { "distFt": 20.0, "clearFt": 20.0, "coverFt": 26.0, "source": "guess", "sourceName": "Rough guess",
              "note": "Narrow street from a front window", "speedLimit": 25 },
  "presets": [ { "key": "narrow", "name": "Narrow street from a front window", "hint": "…", "distFt": 20, "clearFt": 20 }, … ],
  "camera": { "hfov": 66, "width": 1920, "fps": 30, "shutter": 120 },
  "verdict": { "grade": 0, "label": "Good to go", "text": "About ±0.9 mph at 35 mph, over a 18 ft zone.", "checkMph": 35,
               "errorMph": 0.9, "zoneFt": 18.0, "shortView": true, "fastest": "up to about 60 mph",
               "fastest60": "over 70 mph", "frameRate": null },
  "measured": { "coverFt": 97.9, "clearFt": 92.5, "how": "your step 5 calibration, 2026-10-31" },
  "carLengths": null,
  "picture": true,
  "typicalCarFt": 15.5,
  "map": { "camLat": 38.02950, "camLon": -78.48900, "laneLat": 38.02950, "laneLon": -78.48892, "street": "7th 1/2 Street Southwest",
           "distFt": 23.0, "facing": "E" },
  "legalDirection": "R2L", "L2RDirection": "S", "R2LDirection": "N",
  "shortViewOnSave": false,
  "dirty": true,
  "changes": [ "Window to the lane: 20 → 23 ft", "Map: camera and lane pins on 7th 1/2 Street Southwest" ],
  "autosave": false,
  "saved": { "distFt": 20.0, "clearFt": 20.0, "speedLimit": 25, "source": "guess", "grade": 1, "label": "Works, a bit rougher",
             "text": "About ±0.9 mph at 35 mph, over a 18 ft zone. Cars blur a lot at this shutter: 1/500 s or faster sharpens them." }
}
```

* Everything outside `saved` describes the draft.  `dirty` says it differs from the saved plan, and `changes` lists how,
  in words.  `saved` is the saved plan's numbers and verdict.  `legalDirection`, `L2RDirection` and `R2LDirection` are
  as they will be once saved (the map can set them); `shortViewOnSave` says saving also turns on short-view timing.
* `map` is `null` until the map's two pins are placed; then it holds them, the street's name and the distance and
  the camera's facing worked out from them.

* `street.distFt` is the camera to the middle of the moving lane, `clearFt` the stretch of it with nothing in the way,
  `coverFt` the width of street the whole picture spans there. `source` says where they came from: `guess` (a preset
  or the first-run default), `typed`, `picture` (car lengths across the picture), `map` or `calibration` (measured by the
  calibration in force; VST Studio applies a new calibration's measurement by itself, once). `note` says how, in words.
* `verdict` is the planner's, in its words: `grade` 0 good, 1 rougher, 2 needs a tweak; `errorMph` is `null` when the
  view can't time a car at `checkMph` (the speed limit + 10); `fastest`/`fastest60` are set for a short view;
  `frameRate` is advice when the planned camera is under 24 fps.
* `measured` is what the calibration says the street is, or `null` with no calibration for this camera (or the
  bundled sample's). `picture` says whether a picture of the user's own view is open to size it from.

`PUT /api/plan` makes one change and answers `{ "ok", "problems", "plan" }` like `PUT /api/config`, refusals in-band:

* `{"preset": "narrow"}`: a preset's numbers, as a rough guess.
* `{"distFt": 24, "clearFt": 30}`: typed numbers (either or both, 3 to 1000 ft).
* `{"carLengths": 6}`: that many `typicalCarFt` car lengths span the picture (1 to 20).
* `{"useMeasured": true}`: back to what the calibration measured.
* `{"speedLimit": 25}`: the posted limit the plan checks against (5 to 80).
* `{"shortView": true}`: turn on short-view timing, fitted to this plan, when the draft is saved.
* `{"map": {"camLat", "camLon", "laneLat", "laneLon", "wayBearing", "oneway", "maxspeedMph", "street"}}`: the map's two pins
  (2 to 500 ft apart), and what OpenStreetMap says about the street under the lane pin, all but the pins optional:
  `wayBearing` is the street's drawn direction there in degrees from north, `oneway` 1 along it, -1 against it, 0
  two-way.  Sets the distance, the compass headings across the picture, the legal direction of a one-way street, the
  limit and (when the site has no name yet) the site name.
* `{"save": true}`, `{"discard": true}`, `{"autosave": true|false}`: as above.

### `POST /api/run`

`{"action": "start" | "stop" | "pause" | "resume"}` → `{"ok": true, "status": { … }}`, where `status`
is the `/api/status` body after the action. An unknown action is `400` `{"error"}`. VST Studio can
decline to start (no source chosen, zones that don't validate); `ok` is still true and
`status.running` stays `false`, while the desktop shows why.

### Calibration

The tracker turns frames-in-the-zone into speed by comparison with a vehicle driven at a known
speed. Three ways to record it:

* **From a logged pass (preferred).** Drive through at a known speed, find the pass, then
  `POST /api/calibration/apply {"dir": "L2R"|"R2L", "speed": 25, "vehicleId": 1042}`. VST sets
  `CalibrationFrames<dir>` so that a pass like that one (its `frames` at its `fps`) reads `speed`
  mph. `CalibrationSpeed` and `CalibrationFPS` stay as they are, since the other direction's
  calibration is expressed against them: `CalibrationFrames<dir> = frames × speed / CalibrationSpeed
  × CalibrationFPS / fps`, rounded.
* **By hand.** `POST /api/calibration/mark {"dir", "edge": "start"|"end"}` records the current frame
  as the calibration vehicle's front bumper crosses the first and then the second speed line, and
  answers `{"ok": true, "frame": 2161, "marks": [start, end]}` (`-1` for an edge not marked yet);
  `POST /api/calibration/apply {"dir", "speed"}` (no `vehicleId`) then uses the marked span.

* **From two spots a measured distance apart (no car): cones, or one pole moved between them.**
  `POST /api/calibration/distance {"cones": [[x1, y1], [x2, y2]], "feet": 10, "dir": "both"|"L2R"|"R2L"}`.
  `cones` are the two spots in frame coordinates (1280×720, like the speed
  lines) and `feet` is the taped distance between them; `dir` defaults to `"both"`. The zone between
  the speed lines is `(speedLineRight - speedLineLeft) / |x2 - x1| × feet` long, and VST sets
  `CalibrationFrames<dir>` to the frames a car at `CalibrationSpeed` takes to cross it at
  `CalibrationFPS`: `zoneFeet / (CalibrationSpeed × 5280 / 3600) × CalibrationFPS`, rounded. The answer
  adds `"zoneFeet"` and the unrounded `"frames"` (both to 0.1). Refusals: cones missing or off the
  picture, `feet` not positive, an unknown `dir`, spots less than 40 px apart across the picture, or
  speed lines not set. The calibration shows as Not checked until a check pass (below).

Every way of applying returns `{"ok": true, "problems": [], "config": { … }}`, or `200`
`{"ok": false, "problems": [ … ]}` when the pass is gone, the span isn't marked, `speed` isn't
positive, or (for the spots) one of the refusals above. A body that isn't a JSON object is a `400`.

**Checking it.** One pass at a speedometer reading is easy to get wrong, so after calibrating, drive past
once more (ideally at a different speed) and send that pass to
`POST /api/calibration/check`. It takes the same body as apply (`{"dir", "speed", "vehicleId"}`, or
`{"dir", "speed"}` for the hand-marked span; `dir` may be left out with a `vehicleId`) and changes no
settings. It answers:

```json
{ "ok": true, "problems": [], "direction": "R2L", "drivenMph": 30, "measuredMph": 30.3, "error": 0.3, "verified": true }
```

`measuredMph` is what the calibration in force says the pass was, unrounded except to 0.1 mph
(`CalibrationSpeed × CalibrationFrames<dir> × (fps / CalibrationFPS) / frames`), `error` is
`measuredMph - drivenMph` (also to 0.1), and `verified` is `|error| <= 2`. Refusals are as for apply: `200`
`{"ok": false, "problems": [ … ]}`. The new state shows in `GET /api/status` `calibration` at once.

Every apply and check, from the phone or the desktop, is appended to `calibration-history.csv` in VST
Studio's config folder (the format is in [vst-studio.md](vst-studio.md#5-calibrate)), so the record of how a
calibration was made and tested outlives the session.

### `GET /api/stats.csv`

The session's stats csv, same columns as the command-line `stats_*.csv`. `404` `{"error"}` before the
first run.

```
Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp
```

`Flag` is `*****` for an implausible speed, else empty. `Timestamp` is when the vehicle passed, ISO 8601 with the
UTC offset (`2026-10-30T14:03:22-04:00`), or empty when the source gives no way to tell. The columns are described
in the [README](../README.md#the-stats-csv).

### `GET /api/report.html?from=YYYY-MM-DD&to=YYYY-MM-DD`

The street report: one self-contained HTML page (inline CSS and SVG charts, no external requests) that prints to
PDF from any browser. It's the same report as **Make report** on VST Studio's Results page, built from every stats csv
in the data folder for the local dates `from` to `to` (inclusive; either can be left out for "from the first" or "to
the last" data), with the site name, speed limit and "way over" speed from VST Studio's settings. Example frames
are never included through the API. VST Studio also saves the page as
`<data>/Reports/report_<site>_<from>_<to>.html`.

`Content-Type: text/html; charset=utf-8`. A `from` or `to` that isn't a date is `400` `{"error"}`. The token can go
in the query string (`&token=`), so the phone can open the page in a new tab and share it from there. The phone's
**Stats** view links to it as **Street report**, for the period on screen.

## Checking an implementation

`companion/test/contract.test.mjs` checks this document over HTTP (Node 20+, no dependencies):

```sh
node --test companion/test/*.test.mjs                                   # against the mock
BASE=http://127.0.0.1:8787 TOKEN=<pairing code> node --test companion/test/*.test.mjs   # a running VST Studio
```

Against a real VST Studio it only reads and sends requests that must be refused; `MUTATE=1` adds the
ones that change settings (and put them back; the calibration tests' rows stay in `calibration-history.csv`,
which is never rewritten). `tests/api_smoke.sh`, run by `ctest`, starts VST
Studio under Xvfb, tracks the sample video through this API, and runs the suite against it.
