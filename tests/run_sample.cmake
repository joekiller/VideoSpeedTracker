# Regression check: run VST headless on the bundled 2016 Locust Ave sample and compare
# its csv to the baseline recorded from the first Linux/OpenCV 4 port (x86-64, OpenCV 4.6).
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir>.
#
# The comparison (same vehicles, order and direction; small frame/speed drift allowed for
# arm64 SIMD differences) lives in compare_vehicles.cmake, shared with the companion API test.
#
# The baseline was recorded before the csv gained its Flag and Timestamp columns, so those are
# checked on their own: the header must be the current one, every row must have all 15 columns,
# Date/Time must be the Foscam name's 20160130, 172251, and Timestamp must be the name's start
# time plus the row's Frame at 30 fps, with a UTC offset (whatever this machine's zone is).
#
# Then the run manifest beside the csv (run_manual_20160130172251.json; see writeManifest() in videoSpeedTracker.cpp):
# complete, as many vehicles logged as the baseline has rows (61), the calibration and zone from bin/VST.cfg, and
# started / ended at the Foscam name's start time and that plus the frames analyzed at 30 fps.
# min_logged_speed.cmake (tests min_logged_speed_0 and _30) reuses this run's manifest (the sample_run fixture).
cmake_policy(VERSION 3.16)  # keep empty csv fields as list elements (CMP0007)
include(${CMAKE_CURRENT_LIST_DIR}/compare_vehicles.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/manifest.cmake)
set(BASELINE ${SRC}/tests/baseline_manual_20160130172251.csv)
# Named the way VST names it without --stats, so the manifest gets the name a user would see.
set(CSV ${OUT}/stats_manual_20160130172251.csv)

file(MAKE_DIRECTORY ${OUT})
execute_process(
  COMMAND ${VST} --headless
          --input ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi
          --config ${SRC}/bin/VST.cfg
          --stats ${CSV}
  RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "VideoSpeedTracker exited with ${rc}")
endif()

# The time columns of this run (see the note at the top).
set(HEADER "Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp")
file(STRINGS ${CSV} lines)
list(GET lines 0 got_header)
if(NOT got_header STREQUAL HEADER)
  message(FATAL_ERROR "csv header is\n  ${got_header}\nexpected\n  ${HEADER}")
endif()
list(REMOVE_AT lines 0)
set(time_errors "")
foreach(line IN LISTS lines)
  # Split on "," and trim, rather than on ", ", so an empty last column still counts as a column.
  string(REPLACE "," ";" fields "${line}")
  list(LENGTH fields n_fields)
  if(NOT n_fields EQUAL 15)
    string(APPEND time_errors "  ${n_fields} columns, expected 15: ${line}\n")
    continue()
  endif()
  list(GET fields 0 date)
  list(GET fields 1 time)
  list(GET fields 2 frame)
  list(GET fields 14 stamp)
  string(STRIP "${date}" date)
  string(STRIP "${time}" time)
  string(STRIP "${frame}" frame)
  string(STRIP "${stamp}" stamp)
  # manual_20160130172251 started at 17:22:51; the vehicle passed frame/30 whole seconds later.
  math(EXPR t "17 * 3600 + 22 * 60 + 51 + ${frame} / 30")
  math(EXPR hh "${t} / 3600")
  math(EXPR mm "${t} % 3600 / 60")
  math(EXPR ss "${t} % 60")
  foreach(v hh mm ss)
    if(${v} LESS 10)
      set(${v} "0${${v}}")
    endif()
  endforeach()
  set(want "2016-01-30T${hh}:${mm}:${ss}")
  if(NOT date STREQUAL "20160130" OR NOT time STREQUAL "172251"
     OR NOT stamp MATCHES "^${want}[+-][0-9][0-9]:[0-9][0-9]$")
    string(APPEND time_errors "  frame ${frame}: Date/Time ${date} ${time}, Timestamp ${stamp}; expected 20160130 172251, ${want}+hh:mm\n")
  endif()
endforeach()
if(time_errors)
  message(FATAL_ERROR "Time columns are wrong in ${CSV}:\n${time_errors}")
endif()

compare_vehicles(${BASELINE} ${CSV})

# ---- the run manifest ----
read_manifest(${OUT}/run_manual_20160130172251.json M)
csv_rows(${BASELINE} baseline_rows)
csv_rows(${CSV} rows)
set(errors "")
macro(expect name got want)
  if(NOT "${got}" STREQUAL "${want}")
    string(APPEND errors "  ${name} is \"${got}\", expected \"${want}\"\n")
  endif()
endmacro()
expect(format "${M_format}" 1)
expect(stats "${M_stats}" stats_manual_20160130172251.csv)
expect(source.input "${M_source_input}" manual_20160130172251.avi)
expect(source.kind "${M_source_kind}" file)
expect(complete "${M_complete}" ON)
expect(fps "${M_fps}" 30)
expect(outages "${M_outages}" 0)
expect("counts.logged (baseline rows)" "${M_logged}" ${baseline_rows})
expect("counts.logged (csv rows)" "${M_logged}" ${rows})
# bin/VST.cfg: the 2016 Locust Avenue calibration and zone; minLoggedSpeed isn't set there, so it's the default 18.
foreach(kv CalibrationSpeed=25 CalibrationFPS=30 CalibrationFramesL2R=35 CalibrationFramesR2L=40 LegalDirection=Both
           L2RDirection=SE R2LDirection=NW speedLineLeft=350 speedLineRight=909 minLoggedSpeed=18 subFrameTiming=0)
  string(REPLACE "=" ";" kv "${kv}")
  list(GET kv 0 k)
  list(GET kv 1 v)
  expect(config.${k} "${M_config_${k}}" ${v})
endforeach()
foreach(k belowMinSpeed untimed)
  if(NOT M_${k} MATCHES "^[0-9]+$")
    string(APPEND errors "  counts.${k} is \"${M_${k}}\", expected a count\n")
  endif()
endforeach()
# The sample is 8.6 minutes at 30 fps.  started = the name's 17:22:51; ended = that plus the frames analyzed / 30, whole seconds.
if(NOT M_frames GREATER 15000)
  string(APPEND errors "  frames is ${M_frames}, expected the whole 8.6 minute sample (about 15458)\n")
endif()
math(EXPR t "17 * 3600 + 22 * 60 + 51 + ${M_frames} / 30")
math(EXPR hh "${t} / 3600")
math(EXPR mm "${t} % 3600 / 60")
math(EXPR ss "${t} % 60")
foreach(v hh mm ss)
  if(${v} LESS 10)
    set(${v} "0${${v}}")
  endif()
endforeach()
if(NOT M_started MATCHES "^2016-01-30T17:22:51[+-][0-9][0-9]:[0-9][0-9]$")
  string(APPEND errors "  started is ${M_started}, expected 2016-01-30T17:22:51+hh:mm\n")
endif()
if(NOT M_ended MATCHES "^2016-01-30T${hh}:${mm}:${ss}[+-][0-9][0-9]:[0-9][0-9]$")
  string(APPEND errors "  ended is ${M_ended}, expected 2016-01-30T${hh}:${mm}:${ss}+hh:mm (${M_frames} frames after the start)\n")
endif()
if(errors)
  message(FATAL_ERROR "The run manifest is wrong:\n${errors}${M_json}")
endif()
