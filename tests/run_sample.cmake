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
cmake_policy(VERSION 3.16)  # keep empty csv fields as list elements (CMP0007)
include(${CMAKE_CURRENT_LIST_DIR}/compare_vehicles.cmake)
set(BASELINE ${SRC}/tests/baseline_manual_20160130172251.csv)

file(MAKE_DIRECTORY ${OUT})
execute_process(
  COMMAND ${VST} --headless
          --input ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi
          --config ${SRC}/bin/VST.cfg
          --stats ${OUT}/sample.csv
  RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "VideoSpeedTracker exited with ${rc}")
endif()

# The time columns of this run (see the note at the top).
set(HEADER "Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp")
file(STRINGS ${OUT}/sample.csv lines)
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
  message(FATAL_ERROR "Time columns are wrong in ${OUT}/sample.csv:\n${time_errors}")
endif()

compare_vehicles(${BASELINE} ${OUT}/sample.csv)
