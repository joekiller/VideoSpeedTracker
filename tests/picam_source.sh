#!/usr/bin/env bash
# Raspberry Pi camera source (PiCamera.cpp) end to end, with tests/fake_rpicam_vid.sh standing in for rpicam-vid: there is
# no Pi camera in CI, so this checks everything up to the camera itself.  Registered by CMake where ffmpeg and bash are.
#   picam_source.sh <VideoSpeedTracker> <repo root> <work dir> [ffmpeg]
#
#   1. --list-pi-cameras names each kind of camera and the frame rate it will run at (a v2 can't do 60 at 720p).
#   2. A run on picam:0?fps=30 starts the recorder with the expected arguments (fixed shutter, I420 on stdout), logs the
#      camera's settings, uses this computer's clock, tracks the sample's vehicles with the same directions and speeds as
#      the file run's baseline, and when "the camera" stops treats it as a dropout (reconnects) rather than an end.
#   3. Camera Module 3 gets its focus locked; the Global Shutter Camera gets no focus arguments.
#   4. A camera delivering fewer frames than asked (here 30 of 60) has its measured rate used for speeds, and says so.
#   5. Failures say what to do: a camera another program holds, no camera attached, a bad setting.
set -u
VST=$1; SRC=$2; OUT=$3
export FAKE_RPICAM_FFMPEG=${4:-ffmpeg}
rm -rf "$OUT"; mkdir -p "$OUT"
export VST_RPICAM_VID=$SRC/tests/fake_rpicam_vid.sh
export FAKE_RPICAM_VIDEO=$SRC/sampleData/IPCam/20160130/manual_20160130172251.avi
export FAKE_RPICAM_ARGS=$OUT/args.txt
BASELINE=$SRC/tests/baseline_manual_20160130172251.csv
LOG=$OUT/vst.log
CSV=$OUT/picam.csv
VPID=
cleanup() { [ -n "$VPID" ] && kill "$VPID" 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- VST log"; grep -v "Speed is" "$LOG" 2>/dev/null | tail -30; echo "--- args"; cat "$FAKE_RPICAM_ARGS" 2>/dev/null; exit 1; }
expect() { grep -qF -- "$2" "$1" || fail "expected \"$2\" in $1"; }

# ---- 1. listing ----
for s in imx296:"Global Shutter Camera (imx296), 60 fps" imx708:"Camera Module 3 (imx708), 60 fps" imx219:"Camera Module v2 (imx219), 47 fps"; do
	FAKE_RPICAM_SENSOR=${s%%:*} "$VST" --list-pi-cameras > "$OUT/list.txt" 2>&1 || fail "--list-pi-cameras failed for ${s%%:*}: $(cat "$OUT/list.txt")"
	expect "$OUT/list.txt" "picam:0    ${s#*:}"
done
echo "listing: Global Shutter, Camera Module 3 and v2 named, with their frame rates"

# ---- 2. a run through the stand-in camera: 60 s of the sample, then it stops ----
FAKE_RPICAM_SECONDS=60 "$VST" --headless --input 'picam:0?fps=30' --config "$SRC/bin/VST.cfg" --stats "$CSV" > "$LOG" 2>&1 &
VPID=$!
for ((i = 0; i < 600; i++)); do
	grep -q "Reconnecting to Pi camera 0: attempt 1" "$LOG" && break
	kill -0 "$VPID" 2>/dev/null || fail "VST exited instead of riding out the camera stopping"
	sleep 0.2
done
grep -q "Reconnecting to Pi camera 0: attempt 1" "$LOG" || fail "no reconnect after the camera stopped"
kill "$VPID"; wait "$VPID" 2>/dev/null; VPID=
expect "$LOG" "Pi camera 0 (Global Shutter Camera): 1280x720 at 30 fps, shutter 1/1000 s, gain automatic"
expect "$LOG" "Vehicle times: this computer's clock"
expect "$FAKE_RPICAM_ARGS" "--camera 0 -t 0 --nopreview --codec yuv420 --flush -o - --width 1280 --height 720 --framerate 30 --shutter 1000"
grep -q -- "--lens-position" "$FAKE_RPICAM_ARGS" && fail "the Global Shutter Camera has no motorised lens, yet got focus arguments"
# The camera's first ~2 s goes to timing its frame rate, so its frame 0 is a little into the sample.  Offset every row by
# the first one's and they must line up with the baseline's rows in the part of the sample that was played.
awk -F', *' -v base="$BASELINE" '
	BEGIN { while ((getline l < base) > 0) if (n++) { split(l, f, ", *"); bf[++m] = f[3]; bd[m] = f[4]; bs[m] = f[13] } }
	NR == 1 { next }
	{ got++; f3[got] = $3; d[got] = $4; s[got] = $13 }
	END {
		if (got < 3) { print "only " got " vehicles tracked"; exit 1 }
		# The first camera row is the first baseline row at or after its frame with the same direction and speed.
		best = -1
		for (k = 1; k <= m && best < 0; k++)
			if (bf[k] >= f3[1] && bd[k] == d[1] && bs[k] - s[1] <= 1 && s[1] - bs[k] <= 1) best = k
		if (best < 0) { print "first vehicle " d[1] " " s[1] " mph is not in the baseline"; exit 1 }
		off = bf[best] - f3[1]
		if (off > 100) { print "frame offset " off " is more than the ~70 frames timing should take"; exit 1 }
		for (i = 1; i <= got; i++) {
			k = best + i - 1
			df = f3[i] + off - bf[k]; ds = s[i] - bs[k]
			if (d[i] != bd[k] || df > 4 || df < -4 || ds > 1 || ds < -1) { print "vehicle " i ": frame " f3[i] "+" off " " d[i] " " s[i] " mph, baseline frame " bf[k] " " bd[k] " " bs[k] " mph"; exit 1 }
		}
		# Every baseline vehicle well inside what was played (60 s = 1800 frames) must be there.
		for (k = best + got; k <= m && bf[k] < 1800 - 120; k++) { print "missed baseline vehicle at frame " bf[k]; exit 1 }
		print got " vehicles match the baseline (frame offset " off ")"
	}' "$CSV" > "$OUT/compare.txt" || fail "$(cat "$OUT/compare.txt"); csv: $(cat "$CSV")"
cat "$OUT/compare.txt"
grep -q '"kind": "camera"' "$OUT/run_picam.json" || fail "the run manifest should call a Pi camera a camera: $(cat "$OUT/run_picam.json")"
echo "run: tracked through the stand-in camera and reconnected when it stopped"

# ---- 3. Camera Module 3: focus locked ----
: > "$FAKE_RPICAM_ARGS"
FAKE_RPICAM_SENSOR=imx708 FAKE_RPICAM_SECONDS=3 "$VST" --headless --input 'picam:0?focus=0.5&rotate=180' --config "$SRC/bin/VST.cfg" --stats "$OUT/cm3.csv" > "$LOG" 2>&1 &
VPID=$!
for ((i = 0; i < 100; i++)); do grep -q "Lost Pi camera 0" "$LOG" && break; sleep 0.2; done
kill "$VPID"; wait "$VPID" 2>/dev/null; VPID=
expect "$FAKE_RPICAM_ARGS" "--framerate 60 --shutter 1000 --autofocus-mode manual --lens-position 0.5 --rotation 180"
expect "$LOG" "Pi camera 0 (Camera Module 3): 1280x720 at 60 fps, shutter 1/1000 s, gain automatic, focus locked at 0.5 dioptres"
echo "Camera Module 3: focus locked, rotated"

# ---- 4. slower than asked ----
FAKE_RPICAM_REALTIME=1 FAKE_RPICAM_SECONDS=4 "$VST" --headless --input picam --config "$SRC/bin/VST.cfg" --stats "$OUT/slow.csv" > "$LOG" 2>&1 &
VPID=$!
for ((i = 0; i < 100; i++)); do grep -q "Lost Pi camera 0" "$LOG" && break; sleep 0.2; done
kill "$VPID"; wait "$VPID" 2>/dev/null; VPID=
grep -qE "Global Shutter Camera is delivering (29|30|31)(\.[0-9])? fps, not the 60 asked for" "$LOG" || fail "no note about the slower frame rate"
grep -qE "^Video is (29|30|31)(\.[0-9])? fps and calibration was counted at 30 fps|^Pi camera 0 \(Global Shutter Camera\): 1280x720 at (29|30|31)(\.[0-9])? fps" "$LOG" || fail "the measured rate isn't the one used"
echo "slower than asked: the measured frame rate is used, and the log says why"

# ---- 5. failures ----
FAKE_RPICAM_FAIL=busy "$VST" --headless --input picam --config "$SRC/bin/VST.cfg" --stats "$OUT/busy.csv" > "$LOG" 2>&1 && fail "a busy camera should fail the run"
expect "$LOG" "Pi camera 0 is in use by another program"
FAKE_RPICAM_SENSOR=none "$VST" --headless --input picam --config "$SRC/bin/VST.cfg" --stats "$OUT/none.csv" > "$LOG" 2>&1 && fail "no camera should fail the run"
expect "$LOG" "No Raspberry Pi camera found. Check the ribbon cable"
"$VST" --headless --input 'picam:1' --config "$SRC/bin/VST.cfg" --stats "$OUT/one.csv" > "$LOG" 2>&1 && fail "a missing camera 1 should fail the run"
expect "$LOG" "There's no Pi camera 1; this Pi has camera 0 (Global Shutter Camera)."
"$VST" --headless --input 'picam:0?shuter=1000' --config "$SRC/bin/VST.cfg" --stats "$OUT/typo.csv" > "$LOG" 2>&1 && fail "a misspelt setting should fail the run"
expect "$LOG" 'Unknown Pi camera setting "shuter"'
VST_RPICAM_VID=/nonexistent "$VST" --headless --input picam --config "$SRC/bin/VST.cfg" --stats "$OUT/notool.csv" > "$LOG" 2>&1 && fail "no rpicam-vid should fail the run"
grep -qE "rpicam-vid, which runs Raspberry Pi cameras, isn't installed|work when VST runs on the Raspberry Pi" "$LOG" || fail "no rpicam-vid: no advice"
echo "failures: busy, no camera, wrong number, misspelt setting and no rpicam-vid each say what to do"
echo "PASS"
