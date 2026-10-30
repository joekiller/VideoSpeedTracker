# minLoggedSpeed: runs the 2016 sample again with the threshold off (0) and raised (30), and checks them against the
# default-18 run of sample_video_baseline (its manifest, through the sample_run fixture).
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir> -DBASE=<sample_video_baseline's work dir>
# and -DMIN=<threshold> (0 or 30; CMakeLists.txt registers one test for each). Without MIN it checks both in turn.
# The files of each value carry it in their names (min0.cfg, stats_min0.csv, run_min0.json), so tests for different
# values can share OUT and run at the same time.
#
# The threshold only decides which timed vehicles get a csv row, so for every value:
#   - the csv has exactly counts.logged rows, none slower than minLoggedSpeed;
#   - logged + belowMinSpeed (the timed vehicles) and untimed are the same as in the default run;
# and 0 logs at least as many as the default (every timed vehicle), 30 fewer, with the rest in belowMinSpeed.
cmake_policy(VERSION 3.16)  # keep empty csv fields as list elements (CMP0007)
include(${CMAKE_CURRENT_LIST_DIR}/manifest.cmake)
file(MAKE_DIRECTORY ${OUT})
file(READ ${SRC}/bin/VST.cfg sample)

read_manifest(${BASE}/run_manual_20160130172251.json D)
math(EXPR timed "${D_logged} + ${D_belowMinSpeed}")

if(NOT DEFINED MIN)
  set(MIN 0 30)
endif()
foreach(min IN LISTS MIN)
  set(csv ${OUT}/stats_min${min}.csv)
  file(WRITE ${OUT}/min${min}.cfg "${sample}minLoggedSpeed = ${min}\n")
  execute_process(
    COMMAND ${VST} --headless
            --input ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi
            --config ${OUT}/min${min}.cfg --stats ${csv}
    RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "minLoggedSpeed = ${min}: VideoSpeedTracker exited with ${rc}")
  endif()
  read_manifest(${OUT}/run_min${min}.json M)
  csv_rows(${csv} rows)
  math(EXPR got_timed "${M_logged} + ${M_belowMinSpeed}")
  set(why "")
  if(NOT M_config_minLoggedSpeed EQUAL min)
    string(APPEND why "  config.minLoggedSpeed is ${M_config_minLoggedSpeed}\n")
  endif()
  if(NOT rows EQUAL M_logged)
    string(APPEND why "  the csv has ${rows} rows but counts.logged is ${M_logged}\n")
  endif()
  if(NOT got_timed EQUAL timed)
    string(APPEND why "  logged + belowMinSpeed = ${got_timed}; with the default 18 it is ${timed} (${D_logged} + ${D_belowMinSpeed})\n")
  endif()
  if(NOT M_untimed EQUAL D_untimed)
    string(APPEND why "  untimed is ${M_untimed}; with the default 18 it is ${D_untimed}\n")
  endif()
  if(min EQUAL 0 AND (M_logged LESS D_logged OR NOT M_belowMinSpeed EQUAL 0))
    string(APPEND why "  0 should log every timed vehicle: logged ${M_logged} (default run ${D_logged}), belowMinSpeed ${M_belowMinSpeed}\n")
  endif()
  if(min EQUAL 30 AND (NOT M_logged LESS D_logged OR M_belowMinSpeed EQUAL 0))
    string(APPEND why "  30 should leave some of the sample's vehicles out: logged ${M_logged} (default run ${D_logged}), belowMinSpeed ${M_belowMinSpeed}\n")
  endif()
  file(STRINGS ${csv} lines)
  list(REMOVE_AT lines 0)
  foreach(line IN LISTS lines)
    string(REPLACE ", " ";" fields "${line}")
    list(GET fields 12 speed)
    if(speed LESS min)
      string(APPEND why "  logged at ${speed} mph, under ${min}: ${line}\n")
    endif()
  endforeach()
  if(why)
    message(FATAL_ERROR "minLoggedSpeed = ${min}:\n${why}${M_json}")
  endif()
  message(STATUS "minLoggedSpeed = ${min}: ${M_logged} logged, ${M_belowMinSpeed} below, ${M_untimed} untimed")
endforeach()
