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
* `counts` cover the current session (since VST Studio started).
* `legalDirection` is `L2R`, `R2L` or `both` (two-way street, nobody is ever wrong-way).
* VST Studio also reports `counts.avgSpeed` (mph, not rounded), `batch` (`{"index", "count"}`: which
  of the queued videos is being processed) and `calibrationMarks` (`{"L2R": [start, end], "R2L": […]}`,
  the hand-marked frames, `-1` when unmarked). The companion doesn't need them; the mock reports them too.

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

### `GET /api/live` (Server-Sent Events)

`text/event-stream` that pushes changes so the phone needs no polling. Events:

| event | data | when |
|---|---|---|
| `status` | same body as `/api/status` | about once a second |
| `vehicle` | one `/api/vehicles` item | each time a pass is logged |
| `tracks` | `[{"id", "dir", "x", "y", "w", "h", "speed"}]` | optional, ~5/s, vehicles currently being tracked (frame coords; `speed` 0 until known) |
| `config` | same body as `GET /api/config` | when the stream opens, then within about a second of any config change, from any client (the desktop included) |

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
  "R2LStreetY": 122, "L2RStreetY": 158,
  "L2RDirection": "SW", "R2LDirection": "NE",
  "CalibrationFramesL2R": 35, "CalibrationFramesR2L": 40, "CalibrationSpeed": 25, "CalibrationFPS": 30,
  "legalDirection": "R2L",
  "speedLimit": 25, "egregious": 40, "siteName": "7½ St SW"
}
```

As in `VST.cfg`, `speedLineLeft`, `speedLineRight` and `obstruction` are x offsets from
`AnalysisBoxLeft`; `R2LStreetY` and `L2RStreetY` are y offsets from `AnalysisBoxTop`. `obstruction`
of `[0, 0]` means none.

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

### `POST /api/run`

`{"action": "start" | "stop" | "pause" | "resume"}` → `{"ok": true, "status": { … }}`, where `status`
is the `/api/status` body after the action. An unknown action is `400` `{"error"}`. VST Studio can
decline to start (no source chosen, zones that don't validate); `ok` is still true and
`status.running` stays `false`, while the desktop shows why.

### Calibration

The tracker turns frames-in-the-zone into speed by comparison with a vehicle driven at a known
speed. Two ways to record it:

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

Both ways of applying return `{"ok": true, "problems": [], "config": { … }}`, or `200`
`{"ok": false, "problems": [ … ]}` when the pass is gone, the span isn't marked or `speed` isn't
positive.

### `GET /api/stats.csv`

The session's stats csv, same columns as the command-line `stats_*.csv`. `404` `{"error"}` before the
first run.

```
Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp
```

`Flag` is `*****` for an implausible speed, else empty. `Timestamp` is when the vehicle passed, ISO 8601 with the
UTC offset (`2026-10-30T14:03:22-04:00`), or empty when the source gives no way to tell. The columns are described
in the [README](../README.md#the-stats-csv).
