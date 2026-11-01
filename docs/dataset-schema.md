# Published dataset format (`vst-dataset/1`)

A published dataset is a folder that VST Studio writes under `<data>/Published/` when you publish a calibration set
(Results > pick a set > Publish). It is meant to be handed to someone else, such as a city engineer, a council or a
neighbour, who can check it without VST: every speed can be recomputed from the files, and the checksums show that
nothing was changed after publishing.

Studio opens one with **Open > Published** or **Open > An archive or published folder somewhere else**. Anyone else can
double-click `review.html`, which works offline in any browser.

## The folder

| File | Written | Covered by `SHA256SUMS` | What it holds |
|---|---|---|---|
| `dataset.json` | once, when published | yes | What the dataset is: name, site, dates, calibration and summary ([below](#datasetjson)) |
| `points.csv` | once | yes | One row per car, with the exact time it took to cross the zone ([below](#pointscsv)) |
| `archive.json`, `vehicles.csv`, `Stats/`, `VST.cfg`, `Snapshots/`, `Clips/`, `HiLites/` | once | yes | The same layout as an archive (see [vst-studio.md](vst-studio.md#archive-a-dataset-and-review-it-later)): the original stats csvs, run manifests, the config, and pictures |
| `SHA256SUMS` | once | (is the list) | `sha256sum` format: one `<hex>  <path>` line per file above |
| `status.json` | after each re-check | no | Every re-check so far and the current status ([below](#statusjson)) |
| `checks/<date>_<hhmm>.csv` | one per re-check | no | Every car's speed before and after that re-check ([below](#checks)) |
| `review.html` | when published and after each re-check | no | The page people read. It is regenerated to show the latest status, so it is not checksummed |

The files from the first group never change after publishing. A re-check only adds to `status.json` and `checks/` and
regenerates `review.html`. A correction is published as a new folder (`version` 2, `correctionOf` naming the old one),
and the original stays as it was.

### Checking that nothing was changed

In the folder, run

```
sha256sum -c SHA256SUMS          # Linux, Git Bash on Windows
shasum -a 256 -c SHA256SUMS      # macOS
```

Every line should say `OK`. Studio runs the same check when it opens the folder and lists any changed files.

## dataset.json

UTF-8 JSON. Readers ignore fields they don't know. A reader refuses a dataset whose major version (the number after
`vst-dataset/`) is newer than it understands.

| Field | Type | Meaning |
|---|---|---|
| `schema` | string | `"vst-dataset/1"` |
| `id` | string | The folder name, e.g. `2026-11-01_my-street-oct-20-to-oct-22` |
| `name` | string | The name it was published under |
| `version` | int | 1 for the first publication, 2 for its correction, and so on |
| `correctionOf` | string | `id` of the dataset this one corrects, `""` if none |
| `site` | string | The site name from Studio's settings |
| `published` | string | ISO 8601 local time of publishing |
| `vst` | string | VST version that published it |
| `speedLimit` | int | mph. A car is *over* when its speed is greater than this |
| `wayOver` | int | mph. A car is *way over* at this speed or more |
| `directions` | object | `L2R` and `R2L`: compass words for each direction of travel; `legal`: `"L2R"`, `"R2L"` or `""` (two-way) |
| `first`, `last` | string | ISO 8601 start of the first and last run in it |
| `calibrationSet` | int | The set number in Studio it came from |
| `view` | object | Where the camera was looking: `speedLines` `[left, right]` and `streetEdge` `[y0, y1]` in analysis-box pixels, `levelDegrees`, `analysisBox` `[left, top, width, height]` (all 0 = not recorded) |
| `calibration` | object | `zoneFt`, `sigma`, `band95` and `how` ([below](#calibration)) |
| `summary` | object | `cars`, `over`, `wayOver`, `wrongWay`, `p85` ([below](#summary)) |
| `points` | string | `"points.csv"` |
| `pointCount` | int | Rows in `points.csv` |

### calibration

| Field | Meaning |
|---|---|
| `zoneFt.L2R`, `zoneFt.R2L` | The street distance between the two speed lines, in feet, for each direction (the far lane is usually longer) |
| `sigma.L2R`, `sigma.R2L` | Relative 1-sigma uncertainty of that distance (0.02 = 2%). `0` means not on record, and 0.06 (6%) is assumed |
| `band95.L2R`, `band95.R2L` | The promised 95% accuracy, relative: 1.96 × sigma (with the 6% assumption when sigma is 0) |
| `how` | Plain words for how the calibration was measured, e.g. `"3 measurements agree within 2%"` |

### summary

Counted over traffic only: rows with `flagged` = 0 and `ownDrive` = 0.

| Field | Meaning |
|---|---|
| `cars` | Cars counted |
| `over` | Cars with `speedMph` > `speedLimit` |
| `wayOver` | Cars with `speedMph` ≥ `wayOver` |
| `wrongWay` | Cars with `wrongWay` = 1 |
| `p85` | 85th percentile speed, nearest rank: the speed at position ⌈0.85 n⌉ in ascending order |

## points.csv

Comma-separated, a header row, UTF-8. A cell holding a comma, a quote or a newline is quoted, with quotes doubled
(RFC 4180). Empty cells mean not known.

| Column | Meaning |
|---|---|
| `n` | Row number, from 1 |
| `passed` | When the car passed, ISO 8601 local time (or the video position, for files without a clock) |
| `direction` | `L2R` or `R2L` in the camera picture |
| `compass` | The same direction in compass words |
| `wrongWay` | 1 = against a one-way street |
| `zoneSeconds` | Seconds the car took between the two speed lines, 4 decimals. Empty for rows recorded before VST measured it |
| `speedMph` | The published speed, whole mph |
| `trend` | `decelerating`, `steady`, `accelerating`, `rapid`, or empty (as the stats csv's Trend column, see the README) |
| `entryMph`, `exitMph` | Speed at the first and second line |
| `accelMphPerSec` | Acceleration across the zone |
| `mphIn20ft` | Projected speed 20 ft past the zone |
| `lengthFt` | Vehicle length estimate, feet |
| `flagged` | 1 = marked as a bad reading; listed but left out of `summary` |
| `ownDrive` | 1 = a calibration drive, 2 = a check drive; listed but left out of `summary` |
| `run` | The stats csv in `Stats/` it came from |
| `frame` | Frame number in that run |

### Recomputing a speed

```
speed mph = zoneFt[direction] / zoneSeconds / 1.46667        (1 mph = 5280/3600 ft/s)
```

`speedMph` is that value rounded. For rows without `zoneSeconds`, `speedMph` is the speed VST measured, rescaled to
the published zone when the set's calibration changed since (within half a mph).

## status.json

```json
{ "schema": "vst-dataset/1", "id": "...", "status": "tainted", "checks": [ { ... }, ... ] }
```

`status` is the latest check's verdict (`accurate`, `review`, `tainted`), or `""` when never re-checked. Each entry of
`checks`, oldest first:

| Field | Meaning |
|---|---|
| `when` | ISO 8601 time of the re-check |
| `verdict` | The overall result: the worse of the two directions |
| `calibration` | The newer calibration, in the same shape as `dataset.json`'s |
| `test.L2R`, `test.R2L` | The equivalence test for each direction ([below](#the-re-check-test)): `delta`, `sigmaNew`, `tolerance`, `lo`, `hi`, `verdict` (`""` when no cars went that way) |
| `before`, `after` | `summary` with the published speeds and with the newer calibration |
| `meanDiff`, `maxDiff` | Average and largest change per car, mph (negative = slower) |
| `file` | The per-car file, e.g. `checks/2026-11-12_0930.csv` |

## checks

`checks/<date>_<hhmm>.csv`, one per re-check:

| Column | Meaning |
|---|---|
| `n` | Row in `points.csv` |
| `direction` | `L2R` or `R2L` |
| `before` | Published speed, mph |
| `after` | Speed with the newer calibration, 2 decimals |
| `difference` | `after - before` |
| `includedInNumbers` | 1 when the car counts in `summary` (not flagged, not an own drive) |

## The re-check test

A calibration only changes the zone distance, so a newer calibration of the same view changes every speed in a
direction by the same factor. For each direction with cars:

```
delta     = newer zoneFt / published zoneFt - 1      (negative: the published speeds read high)
T         = 1.96 × published sigma                    (the promised band; sigma 0 -> 0.06)
sigmaNew  = newer calibration's sigma                 (0 -> 0.06)
lo, hi    = delta ∓ 1.645 × sigmaNew                  (90% interval for delta)
```

Two one-sided tests at 95% (TOST):

- **accurate** when `|delta| + 1.645 sigmaNew ≤ T`: the newer calibration confirms the promise.
- **tainted** when `|delta| - 1.645 sigmaNew > T`: the speeds are off by more than promised, even allowing for the newer
  calibration's own uncertainty.
- **review** otherwise: the newer calibration isn't precise enough to say. Another calibration drive narrows it.

The overall verdict is the worse of the two directions. A later check replaces an earlier verdict as the status, and
every check stays listed.

## Compatibility

Version 1 readers ignore unknown fields and unknown files, so new optional fields can be added within `vst-dataset/1`.
A change that would make an old reader misread a dataset gets a new major version (`vst-dataset/2`), and older VST
refuses it with "This dataset uses a newer format".
