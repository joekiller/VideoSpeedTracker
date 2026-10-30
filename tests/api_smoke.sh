#!/usr/bin/env bash
# Smoke test of VST Studio's companion API (src/Gui/Server.cpp) against docs/companion-api.md.
#
#   api_smoke.sh <VSTStudio exe> <repo root> <work dir> [cmake]
#
# Starts the real desktop app under a virtual X display (xvfb-run) with its preferences in a throwaway home:
# Remote companion on, a known port and token, the bundled sample video as the source and real-time pacing off.
# Then it drives the app the way the phone does, over HTTP with curl:
#
#   hello, auth (401 without the token), status and config fields, a rejected PUT /api/config that must leave
#   VST.cfg byte-identical, the /api/live stream (status and config events), POST /api/run start, polling until
#   the run ends, the vehicle count and the stats csv against tests/baseline_manual_20160130172251.csv (with the
#   same drift rules as the command-line regression test), a snapshot JPEG, a clip MP4, and 404s with JSON bodies
#   for missing things and path-traversal probes.  Where Node 20+ is installed it also runs the companion's
#   contract suite (companion/test/*.test.mjs) against the app: read-only first, the MUTATE=1 tests last.
#
# The app always dies with the script: it runs in its own session under `timeout` (a hard cap even if this
# script is killed), and the EXIT trap kills that whole process group (xvfb-run, Xvfb and VSTStudio).
#
# Prints "API_SMOKE_SKIPPED: <why>" and exits 0 when it can't run here (no GUI build, no xvfb-run or curl);
# CMakeLists.txt maps that line to a skipped test.
set -u

VST=${1:-}
SRC=${2:?repo root}
OUT=${3:?work dir}
CMAKE=${4:-cmake}

# VST_REQUIRE_API_SMOKE=1 (set in CI's Linux job) turns a skip into a failure, so the test can't quietly stop running.
skip() {
  if [ "${VST_REQUIRE_API_SMOKE:-}" = 1 ]; then echo "FAIL: VST_REQUIRE_API_SMOKE is set but the test can't run: $*"; exit 1; fi
  echo "API_SMOKE_SKIPPED: $*"
  exit 0
}
[ -n "$VST" ] && [ -x "$VST" ] || skip "VST Studio isn't built (VST_BUILD_GUI=OFF?)"
for tool in xvfb-run curl setsid timeout; do
  command -v "$tool" > /dev/null || skip "$tool isn't installed (on Debian/Ubuntu: apt-get install xvfb curl)"
done

SAMPLE=$SRC/sampleData/IPCam/20160130/manual_20160130172251.avi
BASELINE=$SRC/tests/baseline_manual_20160130172251.csv
TOKEN=smoke-$RANDOM$RANDOM
FAILS=0
PGID=

# ---- helpers ------------------------------------------------------------------------------------------

pass() { echo "  ok    $*"; }
fail() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
# Stops here (the trap still cleans up): for failures that make the remaining checks meaningless.
die() { echo "  FAIL  $*"; echo "--- VST Studio log ---"; cat "$OUT/vst.log" 2>/dev/null; exit 1; }

cleanup() {
  if [ -n "$PGID" ]; then
    kill -TERM -- "-$PGID" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 -- "-$PGID" 2>/dev/null || break; sleep 0.3; done
    kill -KILL -- "-$PGID" 2>/dev/null
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# req METHOD PATH [curl args...]: sets CODE, TYPE and BODY (the body is also in $OUT/body).
# Authenticated unless the caller passes its own -H Authorization or NOAUTH=1.
req() {
  local method=$1 path=$2; shift 2
  local auth=(-H "Authorization: Bearer $TOKEN")
  [ "${NOAUTH:-0}" = 1 ] && auth=()
  local meta
  meta=$(curl -sS --max-time 20 -X "$method" ${auth[@]+"${auth[@]}"} -o "$OUT/body" -w '%{http_code} %{content_type}' "$@" "$BASE$path" 2>&1)
  CODE=${meta%% *}
  TYPE=${meta#* }
  # Binary bodies (JPEG, MP4) stay in the file only; bash variables can't hold their NUL bytes.
  case $TYPE in image/*|video/*) BODY="(binary $TYPE)" ;; *) BODY=$(cat "$OUT/body" 2>/dev/null) ;; esac
}
# expect CODE [what]: checks the last response's status.
expect() {
  if [ "$CODE" = "$1" ]; then pass "${2:-} -> $1"; else fail "${2:-} -> expected $1, got $CODE: ${BODY:0:300}"; fi
}
# has KEY...: the last body (compact JSON from nlohmann::json::dump) has each "KEY": field.
has() {
  local k missing=
  for k in "$@"; do grep -q "\"$k\":" <<< "$BODY" || missing="$missing $k"; done
  if [ -z "$missing" ]; then pass "fields: $*"; else fail "missing fields:$missing in ${BODY:0:400}"; fi
}
# num KEY: the first numeric value of "KEY": in the last body.
num() { grep -o "\"$1\":-\?[0-9.]*" <<< "$BODY" | head -1 | cut -d: -f2; }
json_error() {
  if [[ $TYPE == application/json* ]] && grep -q '"error":"' <<< "$BODY"; then pass "$1 has a JSON {error} body"
  else fail "$1 should have a JSON {error} body, got '$TYPE': ${BODY:0:200}"; fi
}

# ---- a throwaway home with the app's preferences -----------------------------------------------------

rm -rf "$OUT"
mkdir -p "$OUT/home" "$OUT/config/videospeedtracker" "$OUT/data" "$OUT/cwd"
cp "$SRC/bin/VST.cfg" "$OUT/VST.cfg"   # blank dataPathPrefix: data goes to $XDG_DATA_HOME/videospeedtracker

# A free port: try random ones until nothing answers (bash's /dev/tcp connects, or fails if the port is closed).
for _ in $(seq 20); do
  PORT=$((20000 + RANDOM % 20000))
  (exec 3<> "/dev/tcp/127.0.0.1/$PORT") 2>/dev/null || break
done
BASE=http://127.0.0.1:$PORT

# Key names are AppPrefs::load()'s (src/Gui/Settings.cpp).  realtime=0 runs the video as fast as it decodes;
# highlights=0 skips the highlights video, which the API doesn't need.
cat > "$OUT/config/videospeedtracker/vst-studio.ini" <<EOF
config = $OUT/VST.cfg
source = $SAMPLE
guideDone = 1
realtime = 0
highlights = 0
remote = 1
remotePort = $PORT
remoteToken = $TOKEN
EOF

echo "Starting VST Studio on port $PORT (work dir $OUT)"
# setsid: a new session, so the trap can kill the whole group.  timeout: the app can never outlive 4 minutes,
# whatever happens to this script.
HOME=$OUT/home XDG_CONFIG_HOME=$OUT/config XDG_DATA_HOME=$OUT/data \
  setsid timeout -k 5 240 xvfb-run -a -s "-screen 0 1600x980x24" "$VST" > "$OUT/vst.log" 2>&1 < /dev/null &
APP=$!
PGID=$APP   # setsid execs in place (a background job is never a group leader), so its pid is the group id
cd "$OUT/cwd" || exit 1

# ---- startup ------------------------------------------------------------------------------------------

echo "Waiting for /api/hello"
for i in $(seq 60); do
  NOAUTH=1 req GET /api/hello
  [ "$CODE" = 200 ] && break
  kill -0 "$APP" 2>/dev/null || die "VST Studio exited before the API came up"
  sleep 0.5
done
[ "$CODE" = 200 ] || die "/api/hello never answered"
expect 200 "GET /api/hello (no token)"
has app version auth
grep -q '"app":"VST"' <<< "$BODY" && pass 'hello says app VST' || fail "hello: $BODY"

echo "Auth"
NOAUTH=1 req GET /api/status
expect 401 "GET /api/status without a token"
json_error "401"
NOAUTH=1 req GET /api/status -H "Authorization: Bearer wrong-$TOKEN"
expect 401 "GET /api/status with a wrong token"
NOAUTH=1 req GET "/api/status?token=$TOKEN"
expect 200 "GET /api/status?token= (for <img> and EventSource)"

echo "Status and config"
req GET /api/status
expect 200 "GET /api/status"
has version running paused source frame totalFrames procFps counts vehicles speeders egregious wrongWay topSpeed \
    speedLimit legalDirection siteName kind name fps width height
grep -q '"running":false' <<< "$BODY" && pass "not running yet" || fail "should not be running yet: $BODY"
grep -q '"kind":"file"' <<< "$BODY" && pass "source is the sample file" || fail "source should be the sample file: $BODY"

req GET /api/config
expect 200 "GET /api/config"
has AnalysisBoxLeft AnalysisBoxTop AnalysisBoxWidth AnalysisBoxHeight speedLineLeft speedLineRight obstruction \
    R2LStreetY L2RStreetY L2RDirection R2LDirection CalibrationFramesL2R CalibrationFramesR2L CalibrationSpeed \
    CalibrationFPS legalDirection speedLimit egregious siteName
CONFIG_BEFORE=$BODY

echo "Rejected config changes leave VST.cfg alone"
SUM_BEFORE=$(cksum < "$OUT/VST.cfg")
req PUT /api/config -H 'Content-Type: application/json' --data '{"speedLineLeft": 900, "speedLineRight": 100}'
expect 200 "PUT /api/config with speed lines out of order (rejected in-band)"
grep -q '"ok":false' <<< "$BODY" && pass "ok is false" || fail "ok should be false: ${BODY:0:300}"
grep -q '"problems":\["' <<< "$BODY" && pass "problems are listed" || fail "problems should be listed: ${BODY:0:300}"
has config
req PUT /api/config -H 'Content-Type: application/json' --data '{"AnalysisBoxLeft": "left"}'
grep -q '"ok":false' <<< "$BODY" && pass "a string for a number is rejected" || fail "a string for a number should be rejected: ${BODY:0:300}"
req PUT /api/config -H 'Content-Type: application/json' --data '{"speedLimit": 2}'
grep -q '"ok":false' <<< "$BODY" && pass "speedLimit 2 is rejected" || fail "speedLimit 2 should be rejected: ${BODY:0:300}"
req PUT /api/config -H 'Content-Type: application/json' --data 'not json'
expect 400 "PUT /api/config with a body that isn't JSON"
json_error "400"
[ "$(cksum < "$OUT/VST.cfg")" = "$SUM_BEFORE" ] && pass "VST.cfg is byte-identical" || fail "VST.cfg changed after rejected PUTs"
req GET /api/config
[ "$BODY" = "$CONFIG_BEFORE" ] && pass "GET /api/config is unchanged" || fail "config changed after rejected PUTs: $BODY"

echo "Live events"
# A few seconds of the stream: the first tick sends status and then config (docs: config on connect and on change).
curl -sSN --max-time 3 "$BASE/api/live?token=$TOKEN" > "$OUT/live.txt" 2>/dev/null
grep -q '^event: status' "$OUT/live.txt" && pass "/api/live sends status" || fail "/api/live sent no status: $(head -c 300 "$OUT/live.txt")"
grep -q '^event: config' "$OUT/live.txt" && pass "/api/live sends config" || fail "/api/live sent no config: $(head -c 300 "$OUT/live.txt")"

echo "Run"
req POST /api/run -H 'Content-Type: application/json' --data '{"action": "dance"}'
expect 400 "POST /api/run with an unknown action"
json_error "400"
req POST /api/run -H 'Content-Type: application/json' --data '{"action": "start"}'
expect 200 "POST /api/run start"
grep -q '"ok":true' <<< "$BODY" && pass "ok is true" || fail "ok should be true: ${BODY:0:300}"
grep -q '"status":{' <<< "$BODY" && pass "the reply carries the status" || fail "the reply should carry the status: ${BODY:0:300}"
grep -q '"running":true' <<< "$BODY" && pass "running" || die "the run didn't start: ${BODY:0:300}"

# The sample is 15458 frames (8.5 minutes); with real-time pacing off VST Studio gets through it in ~90 s on a
# 4-core runner, the GUI drawing every frame under a software-rendered Xvfb included.
START=$SECONDS
while :; do
  req GET /api/status
  grep -q '"running":false' <<< "$BODY" && break
  [ $((SECONDS - START)) -lt 150 ] || die "still running after 150 s: ${BODY:0:300}"
  kill -0 "$APP" 2>/dev/null || die "VST Studio exited during the run"
  # Progress every ~10 s, so a slow runner shows where the time goes.
  [ $(((SECONDS - START) % 10)) = 0 ] && echo "  ..    frame $(num frame) of $(num totalFrames) at $(num procFps) fps"
  sleep 1
done
pass "run finished in $((SECONDS - START)) s"

echo "Results"
EXPECTED=$(($(grep -c . "$BASELINE") - 1))   # data rows, without the header
VEHICLES=$(num vehicles)
[ "$VEHICLES" = "$EXPECTED" ] && pass "status counts $VEHICLES vehicles, as the baseline" \
  || fail "status counts ${VEHICLES:-?} vehicles, the baseline has $EXPECTED"
req GET "/api/vehicles?limit=5000"
expect 200 "GET /api/vehicles"
LISTED=$(grep -o '"snapshot":' <<< "$BODY" | wc -l)
[ "$LISTED" = "$EXPECTED" ] && pass "/api/vehicles lists $LISTED" || fail "/api/vehicles lists $LISTED, the baseline has $EXPECTED"
has id time video frame dir compass wrongWay speed speeding egregious area large frames startFrame endFrame fps snapshot clip
FIRST=$(grep -o '"id":[0-9]*' <<< "$BODY" | head -1 | cut -d: -f2)
CLIP=$(grep -o '"clip":"[^"]*"' <<< "$BODY" | head -1 | cut -d'"' -f4)

req GET /api/stats.csv
expect 200 "GET /api/stats.csv"
if "$CMAKE" -DBASELINE="$BASELINE" -DACTUAL="$OUT/body" -P "$SRC/tests/compare_vehicles.cmake" > "$OUT/compare.txt" 2>&1; then
  pass "stats csv matches the baseline (same rules as sample_video_baseline)"
else
  fail "stats csv differs from the baseline:"; cat "$OUT/compare.txt"
fi

req GET "/api/vehicles/$FIRST"
expect 200 "GET /api/vehicles/$FIRST"
req GET "/api/vehicles/$FIRST/snapshot.jpg"
expect 200 "GET /api/vehicles/$FIRST/snapshot.jpg"
[ "$(head -c 3 "$OUT/body" | od -An -tx1 | tr -d ' \n')" = ffd8ff ] && [[ $TYPE == image/jpeg* ]] \
  && pass "snapshot is a JPEG" || fail "snapshot isn't a JPEG ($TYPE)"
if [ -n "$CLIP" ]; then
  req GET "$CLIP"
  expect 200 "GET $CLIP"
  # An MP4 starts with a box whose type, at bytes 4-7, is "ftyp".
  [ "$(head -c 8 "$OUT/body" | tail -c 4)" = ftyp ] && [[ $TYPE == video/mp4* ]] \
    && pass "clip is an MP4" || fail "clip isn't an MP4 ($TYPE, $(head -c 8 "$OUT/body" | od -An -c))"
else
  fail "no vehicle has a clip"
fi
req GET "/api/frame.jpg?overlay=1"
expect 200 "GET /api/frame.jpg"

# The companion's own contract suite (companion/test/), which normally runs against the mock, pointed at the real
# thing now that it has vehicles, snapshots and clips.  Read-only plus refused requests, so it can't disturb the
# checks below.  Optional: skipped where there's no Node 20+.
echo "Contract tests (companion/test) against VST Studio"
if command -v node > /dev/null && [ "$(node -p 'process.versions.node.split(".")[0]')" -ge 20 ] 2>/dev/null; then
  NODE_OK=1
  if BASE=$BASE TOKEN=$TOKEN node --test "$SRC"/companion/test/*.test.mjs > "$OUT/node-test.txt" 2>&1; then
    pass "node --test: $(grep -E '^# (pass|skipped)' "$OUT/node-test.txt" | tr '\n' ' ')"
  else
    fail "node --test against VST Studio:"; grep -E '^not ok|error:|expected|actual' "$OUT/node-test.txt" | head -40
  fi
elif [ "${VST_REQUIRE_API_SMOKE:-}" = 1 ]; then
  fail "VST_REQUIRE_API_SMOKE is set but there's no Node 20+ for the contract tests"
else
  echo "  skip  no Node 20+ here"
fi

echo "Not found"
req GET /api/vehicles/99999999
expect 404 "GET /api/vehicles/99999999"
json_error "404 vehicle"
req GET /api/vehicles/99999999/snapshot.jpg
expect 404 "GET /api/vehicles/99999999/snapshot.jpg"
json_error "404 snapshot"
req GET /api/vehicles/99999999/clip.mp4
expect 404 "GET /api/vehicles/99999999/clip.mp4"
json_error "404 clip"
req GET /api/no-such-route
expect 404 "GET /api/no-such-route"
json_error "404 route"
# --path-as-is: send the dots as typed instead of letting curl resolve them.
for probe in /../../../../etc/passwd /%2e%2e/%2e%2e/%2e%2e/etc/passwd /api/../../etc/passwd /..%2f..%2f..%2fetc/passwd; do
  req GET "$probe" --path-as-is
  expect 404 "path traversal $probe"
  grep -q 'root:' "$OUT/body" && fail "path traversal $probe leaked a file"
done

[ "$(cksum < "$OUT/VST.cfg")" = "$SUM_BEFORE" ] && pass "VST.cfg still byte-identical after the run" || fail "VST.cfg changed during the run"

# Last, now that the byte-identical checks are done: the contract tests that change things (a valid PUT, which
# must be announced on /api/live, and pause/resume), which they undo again.
if [ -n "${NODE_OK:-}" ]; then
  echo "Contract tests that change settings (MUTATE=1)"
  if BASE=$BASE TOKEN=$TOKEN MUTATE=1 node --test "$SRC"/companion/test/*.test.mjs > "$OUT/node-test-mutate.txt" 2>&1; then
    pass "node --test, MUTATE=1: $(grep -E '^# (pass|skipped)' "$OUT/node-test-mutate.txt" | tr '\n' ' ')"
  else
    fail "node --test, MUTATE=1, against VST Studio:"; grep -E '^not ok|error:|expected|actual' "$OUT/node-test-mutate.txt" | head -40
  fi
fi

echo
if [ "$FAILS" -gt 0 ]; then
  echo "$FAILS check(s) failed"
  echo "--- VST Studio log ---"; cat "$OUT/vst.log"
  exit 1
fi
echo "All companion API checks passed"
