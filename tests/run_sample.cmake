# Regression check: run VST headless on the bundled 2016 Locust Ave sample and compare
# its csv to the baseline recorded from the first Linux/OpenCV 4 port (x86-64, OpenCV 4.6).
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir>.
#
# OpenCV's SIMD paths differ by CPU (SSE/AVX vs NEON), which moves a few tracked bumper edges
# by a pixel on arm64. So rather than demanding byte-identical output this requires the same
# vehicles in the same order and direction, each logged within MAX_FRAME_DRIFT frames of the
# baseline with a speed within MAX_SPEED_DRIFT mph. On x86-64 the output is still identical.
#
# The baseline was recorded before the csv gained its Flag and Timestamp columns, so those are
# checked on their own: the header must be the current one, every row must have all 15 columns,
# Date/Time must be the Foscam name's 20160130, 172251, and Timestamp must be the name's start
# time plus the row's Frame at 30 fps, with a UTC offset (whatever this machine's zone is).
cmake_policy(VERSION 3.16)  # keep empty csv fields as list elements (CMP0007)
set(MAX_FRAME_DRIFT 4)
set(MAX_SPEED_DRIFT 1)
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

# Rows of "date, time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix,
# DeltaPix, VehicleArea, , estSpeed[, Flag, Timestamp]" after the header, as lists of
# "frame|direction|speed". The columns these read are the same in the old and new formats.
function(read_vehicles path out)
  file(STRINGS ${path} lines)
  list(REMOVE_AT lines 0)
  set(rows)
  foreach(line IN LISTS lines)
    string(REPLACE "\r" "" line "${line}")
    string(REPLACE ", " ";" fields "${line}")
    list(GET fields 2 frame)
    list(GET fields 3 dir)
    list(GET fields 12 speed)
    list(APPEND rows "${frame}|${dir}|${speed}")
  endforeach()
  set(${out} "${rows}" PARENT_SCOPE)
endfunction()

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

read_vehicles(${BASELINE} expected)
read_vehicles(${OUT}/sample.csv actual)
list(LENGTH expected n_expected)
list(LENGTH actual n_actual)
if(NOT n_actual EQUAL n_expected)
  file(READ ${OUT}/sample.csv got)
  message(FATAL_ERROR "Tracked ${n_actual} vehicles, baseline has ${n_expected}; this run produced:\n${got}")
endif()

set(errors "")
math(EXPR last "${n_expected} - 1")
foreach(i RANGE ${last})
  list(GET expected ${i} e)
  list(GET actual ${i} a)
  string(REPLACE "|" ";" e "${e}")
  string(REPLACE "|" ";" a "${a}")
  list(GET e 0 e_frame)
  list(GET e 1 e_dir)
  list(GET e 2 e_speed)
  list(GET a 0 a_frame)
  list(GET a 1 a_dir)
  list(GET a 2 a_speed)
  math(EXPR d_frame "${a_frame} - ${e_frame}")
  math(EXPR d_speed "${a_speed} - ${e_speed}")
  if(NOT a_dir STREQUAL e_dir OR d_frame GREATER MAX_FRAME_DRIFT OR d_frame LESS -${MAX_FRAME_DRIFT}
     OR d_speed GREATER MAX_SPEED_DRIFT OR d_speed LESS -${MAX_SPEED_DRIFT})
    string(APPEND errors "  vehicle ${i}: expected frame ${e_frame} ${e_dir} ${e_speed} mph, got frame ${a_frame} ${a_dir} ${a_speed} mph\n")
  endif()
endforeach()
if(errors)
  message(FATAL_ERROR "Speeds differ from ${BASELINE}:\n${errors}")
endif()
