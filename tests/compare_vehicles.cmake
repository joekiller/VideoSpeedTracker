# Compares a stats csv from a run on the 2016 Locust Ave sample with the recorded baseline.
# Shared by run_sample.cmake (the command-line tracker) and api_smoke.sh (VST Studio's companion API).
#
#   include(compare_vehicles.cmake) then compare_vehicles(<baseline csv> <actual csv>)
#   cmake -DBASELINE=<csv> -DACTUAL=<csv> -P compare_vehicles.cmake     (standalone)
#
# OpenCV's SIMD paths differ by CPU (SSE/AVX vs NEON), which moves a few tracked bumper edges
# by a pixel on arm64. So rather than demanding byte-identical output this requires the same
# vehicles in the same order and direction, each logged within MAX_FRAME_DRIFT frames of the
# baseline with a speed within MAX_SPEED_DRIFT mph. On x86-64 the output is still identical.
cmake_policy(VERSION 3.16)  # keep empty csv fields as list elements (CMP0007)
set(MAX_FRAME_DRIFT 4)
set(MAX_SPEED_DRIFT 1)

# Rows of "date, time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix,
# DeltaPix, VehicleArea, , estSpeed[, Flag, Timestamp]" after the header, as lists of "frame;direction;speed".
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

# Fails (FATAL_ERROR) unless `actual` logs the baseline's vehicles within the drift limits above.
function(compare_vehicles baseline actual)
  read_vehicles(${baseline} expected)
  read_vehicles(${actual} got_rows)
  list(LENGTH expected n_expected)
  list(LENGTH got_rows n_actual)
  if(NOT n_actual EQUAL n_expected)
    file(READ ${actual} got)
    message(FATAL_ERROR "Tracked ${n_actual} vehicles, baseline has ${n_expected}; this run produced:\n${got}")
  endif()

  set(errors "")
  math(EXPR last "${n_expected} - 1")
  foreach(i RANGE ${last})
    list(GET expected ${i} e)
    list(GET got_rows ${i} a)
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
    message(FATAL_ERROR "Speeds differ from ${baseline}:\n${errors}")
  endif()
endfunction()

# Run directly with -P: compare the two files given on the command line.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED BASELINE OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "Usage: cmake -DBASELINE=<csv> -DACTUAL=<csv> -P compare_vehicles.cmake")
  endif()
  compare_vehicles(${BASELINE} ${ACTUAL})
  message(STATUS "${ACTUAL} matches ${BASELINE}")
endif()
