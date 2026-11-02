#!/usr/bin/env bash
# Live-stream reconnect check, registered by CMake only where ffmpeg is installed (not on Windows).
#   stream_reconnect.sh <VideoSpeedTracker> <repo root> <work dir> <ffmpeg> [engine_reconnect_test]
#
# ffmpeg serves the sample video as a live MPEG-TS stream over HTTP (no length, like an IP camera).  Each serve() plays a
# slice of the sample in real time and then ends the stream, which VST sees as a dropout.
#   1. Command line: the stream drops after 14 s (one vehicle logged), stays down long enough for two reconnect attempts,
#      then comes back with a slice that has another vehicle in it.  VST must log the outage and the reconnect, keep running
#      (a file would have ended), log vehicles on both sides of the gap with this computer's clock in Timestamp, and never
#      log a vehicle whose track spans the gap.
#   2. Embedded (when the test program is built with VST Studio): Stop pressed during a backoff wait ends the run at once.
set -u
VST=$1; SRC=$2; OUT=$3; FFMPEG=$4; ENGINE_TEST=${5:-}
SAMPLE=$SRC/sampleData/IPCam/20160130/manual_20160130172251.avi
PORT=$((20000 + $$ % 20000))
URL=http://127.0.0.1:$PORT/live.ts
rm -rf "$OUT"; mkdir -p "$OUT"
LOG=$OUT/vst.log
CSV=$OUT/live.csv
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- VST log"; grep -v "Speed is" "$LOG" 2>/dev/null | tail -40; echo "--- csv"; cat "$CSV" 2>/dev/null; exit 1; }

# serve <start s> <length s>: plays that slice of the sample to the first client, then ends the stream.
serve() {
	"$FFMPEG" -nostdin -hide_banner -loglevel error -re -ss "$1" -t "$2" -i "$SAMPLE" -an -c:v copy \
		-f mpegts -listen 1 "$URL" >> "$OUT/ffmpeg.log" 2>&1 &
	FF=$!; pids+=("$FF")
	sleep 0.5   # let it bind the port
}
# wait_for <regex> <count> <timeout s>: until the VST log has at least <count> lines matching.
wait_for() {
	local i
	for ((i = 0; i < $3 * 5; i++)); do
		[ "$(grep -cE "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0
		kill -0 "$VPID" 2>/dev/null || fail "VST exited while waiting for \"$1\""
		sleep 0.2
	done
	fail "timed out waiting for \"$1\""
}

# ---- 1. command line ----
serve 0 14
"$VST" --headless --input "$URL" --config "$SRC/bin/VST.cfg" --stats "$CSV" > "$LOG" 2>&1 &
VPID=$!; pids+=("$VPID")
wait_for "Vehicle times: this computer's clock" 1 15
wait_for "> +Lost " 1 40
wait_for "attempt 1 in 1 s" 1 5
wait_for "attempt 2 in 2 s" 1 10   # still down: the waits grow
serve 30 12
wait_for "> +Reconnected to " 1 30
wait "$FF"                                       # second slice over: a second dropout, which VST must also ride out
wait_for "> +Lost " 2 30
kill -0 "$VPID" 2>/dev/null || fail "VST exited at a dropout instead of reconnecting"
kill "$VPID"; wait "$VPID" 2>/dev/null

lost=$(grep -m1 -oE "^<[0-9]+> +Lost " "$LOG" | grep -oE "[0-9]+")
today=$(date +%Y-%m-%d)
awk -F', *' -v lost="$lost" -v today="$today" '
	NR == 1 { if ($0 !~ /, estSpeed, Flag, Timestamp, EntryMph, ExitMph, AccelMphPerSec, Trend, MphIn20ft, LengthPx, ZoneSeconds$/) { print "bad header: " $0; bad = 1 }; next }
	{
		if (NF != 22) { print "row has " NF " columns: " $0; bad = 1; next }
		# Live rows: Date and Time are the clock when logged, and Timestamp is the same moment in ISO 8601 with offset.
		d = substr($15, 1, 4) substr($15, 6, 2) substr($15, 9, 2); t = substr($15, 12, 2) substr($15, 15, 2) substr($15, 18, 2)
		if ($15 !~ ("^" today "T[0-9][0-9]:[0-9][0-9]:[0-9][0-9][+-][0-9][0-9]:[0-9][0-9]$") || $1 != d || $2 != t) { print "bad time columns: " $0; bad = 1 }
		if ($5 <= lost && $6 > lost) { print "vehicle spans the gap at frame " lost ": " $0; bad = 1 }
		if ($5 < lost) before++; else after++
	}
	END {
		if (!before) { print "no vehicle logged before the dropout"; bad = 1 }
		if (!after) { print "no vehicle logged after reconnecting"; bad = 1 }
		exit bad
	}' "$CSV" > "$OUT/csv-check.txt" || fail "$(cat "$OUT/csv-check.txt")"
echo "command line: lost the stream at frame $lost, reconnected, vehicles logged on both sides of the gap"

# The run manifest beside the csv is rewritten as soon as a dropout ends, so even though VST was killed rather than stopped
# it must be there, still marked incomplete, naming the stream and recording the first gap with both of its ends.
MANIFEST=$OUT/run_live.json
[ -f "$MANIFEST" ] || fail "no run manifest at $MANIFEST"
stamp='"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}[+-][0-9]{2}:[0-9]{2}"'
grep -q '"complete": false' "$MANIFEST" || fail "manifest isn't marked incomplete: $(cat "$MANIFEST")"
grep -qF "\"source\": { \"input\": \"$URL\", \"kind\": \"stream\" }" "$MANIFEST" || fail "manifest doesn't name the stream: $(cat "$MANIFEST")"
grep -qE "\{ \"from\": $stamp, \"to\": $stamp \}" "$MANIFEST" || fail "manifest has no complete outage: $(cat "$MANIFEST")"
echo "command line: the run manifest records the outage"

# ---- 2. embedded: Stop during the backoff wait ----
if [ -n "$ENGINE_TEST" ]; then
	serve 0 3
	"$ENGINE_TEST" "$URL" "$SRC/bin/VST.cfg" "$OUT/embedded.csv" > "$OUT/engine.log" 2>&1 || { cat "$OUT/engine.log"; echo "FAIL: embedded stop while reconnecting"; exit 1; }
	tail -1 "$OUT/engine.log"
	# Stopped while down: the final manifest is complete, and its gap runs to the stop.
	grep -q '"complete": true' "$OUT/run_embedded.json" && grep -qE "\{ \"from\": $stamp, \"to\": $stamp \}" "$OUT/run_embedded.json" \
		|| { cat "$OUT/run_embedded.json"; echo "FAIL: embedded run manifest after a stop while reconnecting"; exit 1; }
fi
echo "PASS"
