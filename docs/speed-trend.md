# Speed trend: slowing down, steady or speeding up

For every car it times, VST also says how its speed changed while it crossed the speed zone, and where that change
was taking it:

| Shown as | Csv `Trend` | Acceleration | What it usually means |
|---|---|---|---|
| Slowing down | `decelerating` | -2 mph/s or less | braking or easing off (rolling up to a stop sign is 5-10 mph/s) |
| Steady | `steady` | between -2 and +2 mph/s | cruising; over a 20 ft zone at 20 mph, 2 mph/s changes the speed by only 1.4 mph |
| Speeding up | `accelerating` | +2 to +6 mph/s | pulling away normally |
| Rapidly speeding up | `rapid` | +6 mph/s or more | flooring it: 0-60 in 10 s is 6 mph/s, insurers' "harsh acceleration" is about 7-8 |
| Can't tell (blank) | *(empty)* | | seen too few times or too raggedly; see "Noise" below |

1 mph/s is 1.47 ft/s², or 0.046 g.

Next to the class are the speed at the first speed line (**in**, csv `EntryMph`), at the second (**out**, `ExitMph`),
the acceleration (`AccelMphPerSec`, + speeding up) and the speed **20 ft further on** (`MphIn20ft`) if the car keeps
accelerating or braking the way it was. A steady car is projected at its exit speed, so the projection never shows a
change the class says can't be measured; a car braking hard enough to stop within 20 ft shows 0 ("stopped").

Where it shows: Studio's Results table (**In → out** column, sortable by acceleration, with the class's icon and
colour; the detail card spells it out), the phone's Passes (a pill on every pass that isn't steady, and **Speed
change** and **20 ft further on** on a pass's page), the stats csv's last five columns and the companion API's
`trend` object ([companion-api.md](companion-api.md)).

## How it's measured

`src/Wintel64/VideoSpeedTracker/SpeedTrend.h` has the maths; `VehicleDynamics::computeTrend()` feeds it.

1. VST already follows each car's front bumper frame pair by frame pair. When the car crosses the second speed line
   (the moment it is timed), every bumper position seen so far inside the analysis box, before the zone as well as in
   it, becomes feet along the direction of travel from the first speed line, against seconds. Feet come from the
   calibration (the calibration car covered the zone in `CalibrationFrames` at `CalibrationSpeed`), so there's nothing
   new to set up, and on a receding street (`streetEdge`) they are distances on the ground.
2. A parabola, feet = b0 + b1·t + b2·t², is fitted through them by least squares. Its curvature is the acceleration,
   a = 2·b2, taken as constant over the half second or so a car is in view.
3. Entry and exit speeds follow from v² = v0² + 2·a·d, so the two speeds and the acceleration always agree. They
   are then shifted together until their average is the speed VST reported (for constant acceleration the average
   speed over a stretch is exactly the mean of its end speeds), so a row never reads "25 mph: in 27, out 28".
4. The projection is sqrt(v_out² + 2·a·20 ft).

The headline speed is unchanged: it is still the straight-line fit (or whole-frame count) it always was.

## Noise

A 14-20 ft zone sees a car only 5-16 times, so the acceleration has an uncertainty that the class has to respect.
The fit's standard error comes from its covariance and the bumper jitter. The jitter is the fit's own residuals
pooled with four degrees of freedom' worth of a 1 px floor, because a car seen five or six times has only two or three
residual degrees of freedom of its own. Then:

- a class other than Steady needs |a| of at least k standard errors, k being Student's t for 98% two-sided (3.0 for 6
  points, 2.6 for 15), so a steady car is called accelerating or decelerating in about 2% of passes;
- when k standard errors is more than 6 mph/s the fit can't tell steady from rapid, and the trend is left blank unless
  |a| clears it anyway;
- fewer than 5 positions, a fit straying more than the timing's own limit (`margin(4)` pixels rms: two cars merged, a
  track picked up again), or a car not moving forward leaves it blank too.

`tests/speed_trend_test.cpp` checks this on simulated passes (400 per case, Gaussian jitter). "box" is seen from 12% of
the zone before the first line with 0.1 ft jitter; "late" is what the 2016 sample's 20 ft crop actually gives, the
first bumper 40% of the way in (5-7 positions) with 0.03 ft jitter:

| View | Zone | fps | Truth | Slowing | Steady | Speeding up | Rapid | Can't tell |
|---|---|---|---|---|---|---|---|---|
| box | 20 ft | 30 | steady | 0% | 100% | 0% | 0% | 0% |
| box | 20 ft | 30 | +4 mph/s | 0% | 0% | 99% | 0% | 0% |
| box | 20 ft | 30 | -4 mph/s | 99% | 1% | 0% | 0% | 0% |
| box | 14 ft | 30 | steady | 2% | 96% | 2% | 0% | 0% |
| box | 14 ft | 30 | +4 mph/s | 0% | 35% | 54% | 11% | 1% |
| late | 20 ft | 30 | steady | 0% | 100% | 0% | 0% | 0% |
| late | 20 ft | 30 | +4 mph/s | 0% | 8% | 90% | 2% | 0% |
| late | 14 ft | 30 | steady | 0% | 87% | 1% | 0% | 11% |
| late | 14 ft | 30 | +4 mph/s | 0% | 34% | 28% | 16% | 22% |
| late | 14 ft | 30 | +10 mph/s | 0% | 2% | 1% | 91% | 6% |
| late | 14 ft | 60 | steady | 1% | 98% | 1% | 0% | 0% |

So: in 20 ft at 30 fps, braking and pulling away are clear. At 14 ft and 30 fps a mild change is often read as steady
or left blank, but a car flooring it is still caught, and the direction is never wrong. 60 fps helps short zones most.
`speed_trend_test --table` prints every case.

## On real footage

The 2016 Locust Avenue sample has no ground truth for acceleration, so it was checked against itself: the sample's
20 ft short-view crop (`tests/short_view.cmake`) against the full view over the same stretch of road (the full view's
bumper positions inside the crop, fitted the same way, at its lower resolution). Over the 50 cars both timed:

| Direction | 20 ft crop, mean | Full view, same stretch, mean | Full view, whole 100 ft |
|---|---|---|---|
| SE (L2R) | +3.5 mph/s | +3.6 mph/s | +5.0 mph/s |
| NW (R2L) | -2.5 mph/s | -2.0 mph/s | -0.3 mph/s |

The short view agrees with the full view where they see the same road. SE cars on that block were speeding up and NW
cars slowing a little through the middle of it; acceleration varies along a street, so a short view measures it where
it looks. The fits strayed 0.5-1.5 px rms from a parabola.

To check your own setup, drive through at a steady speed (it should read Steady), then once pulling away from a stop
just before the zone (Speeding up).
