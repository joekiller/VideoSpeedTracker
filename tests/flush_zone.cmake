# Speed zone flush with the analysis box: does VST still time cars when a speed line sits on the box's edge?
#
# A box drawn hard against the side of the picture, with the speed zone starting at that side, is what a narrow view
# invites (Joe's 7½ St SW setup, 2026-11-01).  It used to time nothing in either direction:
#   - exit side: the projected front bumper is held inside the box (VehicleDynamics::estimateNextVehicleData), so it
#     could never get past a line at the box's edge, and every car left untimed;
#   - entry side: a car is first seen already over a line at the edge, which read as a lost track picked up again
#     (markInvalidSpeed), so it was never timed either.
# Now the exit test uses the projection before it is held in the box, and a car that comes into view at the edge within
# a look-ahead of the line is timed by the front-bumper fit (VehicleDynamics::markEnteredAtEdge).
#
# The bundled 2016 sample is run with its own config (bin/VST.cfg) and the same speed lines on the picture, but the box
# cut at one of them: SIDE=left moves the box's left side to the left line (speedLineLeft = 0), SIDE=right its right
# side to the right line (speedLineRight = AnalysisBoxWidth).  Speeds are compared with the full view's
# (tests/baseline_manual_20160130172251.csv), matched by direction and middle of crossing as in short_view.cmake.
#
#                       measured (x86-64, OpenCV 4.6)                 required
#   left   SE found     25 of 28, |error| <= 1 mph                    >= 21
#          NW found     32 of 33, |error| <= 2 mph                    >= 28
#   right  SE found     28 of 28, |error| <= 2 mph                    >= 24
#          NW found     27 of 33, |error| <= 2 mph                    >= 23
#   both   |speed error| median <= 1, p90 <= 2, max <= 4 mph; unmatched rows <= 3
# Before the fix both sides timed 0 of 61.
#
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir> -DSIDE=<left|right>.
cmake_policy(VERSION 3.16)

file(MAKE_DIRECTORY ${OUT})
set(SAMPLE ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi)
file(READ ${SRC}/bin/VST.cfg cfg)
# The sample's box is x 10..1279 with speed lines at 350 and 909 inside it (x 360 and 919 on the picture).
if(SIDE STREQUAL "left")
  set(box "AnalysisBoxLeft = 360\nAnalysisBoxWidth = 919\nspeedLineLeft = 0\nspeedLineRight = 559\nobstruction = [0,0]")
  set(needs SE:21 NW:28)
elseif(SIDE STREQUAL "right")
  set(box "AnalysisBoxLeft = 10\nAnalysisBoxWidth = 909\nspeedLineLeft = 350\nspeedLineRight = 909\nobstruction = [251,311]")
  set(needs SE:24 NW:23)
else()
  message(FATAL_ERROR "SIDE must be left or right, not '${SIDE}'")
endif()
# The sample's own geometry lines come out (a key set twice draws a warning), and the cut box goes in.
string(REGEX REPLACE "\n(AnalysisBoxLeft|AnalysisBoxWidth|speedLineLeft|speedLineRight|obstruction) =[^\n]*" "" cfg "${cfg}")
set(CFG ${OUT}/flush_${SIDE}.cfg)
file(WRITE ${CFG} "${cfg}\n# tests/flush_zone.cmake: the box cut at the ${SIDE} speed line\n${box}\n")

set(CSV ${OUT}/stats_flush_${SIDE}.csv)
execute_process(
  COMMAND ${VST} --headless --input ${SAMPLE} --config ${CFG} --stats ${CSV}
  RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "VideoSpeedTracker exited with ${rc}:\n${log}")
endif()

# Rows as parallel lists: direction, StartFrame + EndFrame, speed.
function(read_rows path prefix)
  file(STRINGS ${path} lines)
  list(REMOVE_AT lines 0)
  set(dirs)
  set(mids)
  set(speeds)
  foreach(line IN LISTS lines)
    string(REPLACE "\r" "" line "${line}")
    string(REPLACE ", " ";" fields "${line}")
    list(GET fields 3 dir)
    list(GET fields 4 start)
    list(GET fields 5 end)
    list(GET fields 12 speed)
    math(EXPR mid "${start} + ${end}")
    list(APPEND dirs ${dir})
    list(APPEND mids ${mid})
    list(APPEND speeds ${speed})
  endforeach()
  set(${prefix}_dirs "${dirs}" PARENT_SCOPE)
  set(${prefix}_mids "${mids}" PARENT_SCOPE)
  set(${prefix}_speeds "${speeds}" PARENT_SCOPE)
endfunction()
read_rows(${SRC}/tests/baseline_manual_20160130172251.csv ref)
read_rows(${CSV} got)
list(LENGTH ref_dirs n_ref)
list(LENGTH got_dirs n_got)
math(EXPR last_ref "${n_ref} - 1")

# Each row to the nearest unused baseline row in the same direction, middles within 10 frames.
set(used)
set(unmatched 0)
set(errors_SE)
set(errors_NW)
set(report "")
if(n_got GREATER 0)
  math(EXPR last_got "${n_got} - 1")
  foreach(i RANGE ${last_got})
    list(GET got_dirs ${i} dir)
    list(GET got_mids ${i} mid)
    list(GET got_speeds ${i} speed)
    set(best -1)
    set(best_d 21)
    foreach(j RANGE ${last_ref})
      list(GET ref_dirs ${j} rdir)
      list(FIND used ${j} taken)
      if(NOT rdir STREQUAL dir OR NOT taken EQUAL -1)
        continue()
      endif()
      list(GET ref_mids ${j} rmid)
      math(EXPR d "${mid} - ${rmid}")
      if(d LESS 0)
        math(EXPR d "-${d}")
      endif()
      if(d LESS best_d)
        set(best ${j})
        set(best_d ${d})
      endif()
    endforeach()
    math(EXPR frame "${mid} / 2")
    if(best EQUAL -1)
      math(EXPR unmatched "${unmatched} + 1")
      string(APPEND report "  unmatched: ${dir} around frame ${frame}, ${speed} mph\n")
      continue()
    endif()
    list(APPEND used ${best})
    list(GET ref_speeds ${best} rspeed)
    math(EXPR e "${speed} - ${rspeed}")
    if(e LESS 0)
      math(EXPR e "-${e}")
    endif()
    list(APPEND errors_${dir} ${e})
    if(e GREATER 1)
      string(APPEND report "  ${dir} around frame ${frame}: ${speed} mph, full view ${rspeed}\n")
    endif()
  endforeach()
endif()

set(failures "")
set(all_errors)
foreach(dir_need IN LISTS needs)
  string(REPLACE ":" ";" dir_need ${dir_need})
  list(GET dir_need 0 dir)
  list(GET dir_need 1 need)
  set(n_dir 0)
  foreach(d IN LISTS ref_dirs)
    if(d STREQUAL dir)
      math(EXPR n_dir "${n_dir} + 1")
    endif()
  endforeach()
  list(LENGTH errors_${dir} found)
  message(STATUS "${SIDE} side flush, ${dir}: ${found} of ${n_dir} vehicles timed")
  list(APPEND all_errors ${errors_${dir}})
  if(found LESS need)
    string(APPEND failures "  ${dir}: timed ${found} of ${n_dir}, need at least ${need}\n")
  endif()
endforeach()
list(LENGTH all_errors n_all)
if(n_all GREATER 0)
  list(SORT all_errors COMPARE NATURAL)
  math(EXPR m "(${n_all} + 1) / 2 - 1")       # nearest-rank median
  math(EXPR p "(9 * ${n_all} + 9) / 10 - 1")  # nearest-rank 90th percentile
  list(GET all_errors ${m} median)
  list(GET all_errors ${p} p90)
  list(GET all_errors -1 worst)
  message(STATUS "|speed error| median ${median}, p90 ${p90}, max ${worst} mph; ${unmatched} unmatched rows")
  if(median GREATER 1 OR p90 GREATER 2 OR worst GREATER 4)
    string(APPEND failures "  |speed error| median ${median}, p90 ${p90}, max ${worst} mph; need <= 1, <= 2, <= 4\n")
  endif()
endif()
if(unmatched GREATER 3)
  string(APPEND failures "  ${unmatched} rows match no full-view vehicle; at most 3 allowed\n")
endif()
if(failures)
  message(FATAL_ERROR "With the speed zone flush with the box's ${SIDE} side:\n${failures}Details:\n${report}")
endif()
