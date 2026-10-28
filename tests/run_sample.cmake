# Regression check: run VST headless on the bundled 2016 Locust Ave sample and compare
# its csv to the baseline recorded from the first Linux/OpenCV 4 port (x86-64, OpenCV 4.6).
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir>.
#
# OpenCV's SIMD paths differ by CPU (SSE/AVX vs NEON), which moves a few tracked bumper edges
# by a pixel on arm64. So rather than demanding byte-identical output this requires the same
# vehicles in the same order and direction, each logged within MAX_FRAME_DRIFT frames of the
# baseline with a speed within MAX_SPEED_DRIFT mph. On x86-64 the output is still identical.
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
# DeltaPix, VehicleArea, , estSpeed" after the header, as lists of "frame;direction;speed".
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
