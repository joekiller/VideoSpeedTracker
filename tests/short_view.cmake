# Short-view check: can VST time cars on a narrow street seen through only FEET of frontage (20 by default; ctest also
# runs 15), where a car fills most or all of the picture and crosses the view in under half a second?  ffmpeg crops the
# bundled 2016 sample to a FEET-wide window (FEET x 11.5 px around the middle of its speed zone, the full height of the
# street; VST scales that to 1280x720) and VST runs on it with a config this script writes from the sample's own,
# with subFrameTiming on.  Its speeds are then compared with the full view's (tests/baseline_manual_20160130172251.csv)
# for the same vehicles, matched by direction and by the middle of their crossing (StartFrame + EndFrame) to within 10
# frames.
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir> -DFFMPEG=<ffmpeg, or empty> [-DFEET=<ft>].
#
# The limits, from the runs on x86-64 (OpenCV 4.6) when this test was written, leave room for a few vehicles' worth of
# arm64 drift.  The same limits hold from 10 to 20 ft; at 8 ft about half the cars are lost.
#                     measured at 20 ft            at 15 ft                 required
#   SE (L2R) found    28 of 28                     28 of 28                 >= 23 (80%)
#   NW (R2L) found    28 of 33                     29 of 33                 >= 25 (75%)  (the missed merge with a walker, or another car)
#   |speed error|     median 0, p90 1, max 2       median 0, p90 1, max 2   median <= 1, p90 <= 2, max <= 4 mph
#   unmatched rows    1                            1                        <= 3  (a real car at frame 7638 the full view couldn't time)
# The full view's own speeds come from whole frame pairs over a 559 px zone, about +-1 mph, so a p90 of 1-2 mph is as
# close as the comparison can show.  Without subFrameTiming the 20 ft clip times no vehicles at all; with the sample's
# own margins (bin/VST.cfg scaled for geometry only) it times 14, 6 to 12 mph off.
# The sample's cars do 20-40 mph.  Its 15 ft crop at half the frame rate (each car moving twice as far per frame, as at
# 40-80 mph at 30 fps) timed all the cars up to about 45 mph equivalent, half of those at 50-59 and few above 60, each
# within 2 mph: the limit the site planner quotes for 15 ft at 30 fps.
#
# Wrong-way: the config makes NW the legal way, so every SE row must have been called out WRONG WAY and no NW one.
cmake_policy(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/manifest.cmake)

if(NOT FFMPEG)
  message("SHORT_VIEW_SKIPPED: ffmpeg was not found when this build was configured; it is needed to cut the short-view clip from the sample")
  return()
endif()

if(NOT FEET)
  set(FEET 20)
endif()
file(MAKE_DIRECTORY ${OUT})
set(SAMPLE ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi)
# The crop: FEET of street at the sample's 11.5 px/ft, centred on x 640 (the middle of the sample's speed zone), the
# full height of the street band (y 200..420).
math(EXPR W "(${FEET} * 23 + 1) / 2")
math(EXPR X0 "640 - ${W} / 2")
set(CLIP ${OUT}/short_view_${W}x220+${X0}+200.mkv)  # named for the crop, so a changed crop makes a new clip
if(NOT EXISTS ${CLIP})
  # Every frame kept (passthrough) so frame numbers line up with the full view's; near-lossless so the crop, not the
  # encoder, is what VST sees.
  execute_process(
    COMMAND ${FFMPEG} -v error -y -i ${SAMPLE} -an -vf crop=${W}:220:${X0}:200 -fps_mode passthrough
            -c:v libx264 -preset ultrafast -crf 12 ${CLIP}.tmp.mkv
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ffmpeg couldn't cut the short-view clip (${rc}):\n${err}")
  endif()
  file(RENAME ${CLIP}.tmp.mkv ${CLIP})
endif()

# The config: the sample's own (bin/VST.cfg) moved into the crop and scaled to 1280x720, x by 1280/W and y by
# 720/220 = 3.273.  Everything that depends on y is the same for every width.
math(EXPR maxDist "(75 * 1280 + ${W} / 2) / ${W}")       # two frames at about 70 mph; subFrameTiming scales its margins by this / 75
math(EXPR slop "(15 * 1280 + ${W} / 2) / ${W}")
math(EXPR lookBack "(1000 * 230 + ${W} / 2) / ${W}")     # about a car length: 1000 at 20 ft, the box's width when a car fills the view
if(lookBack GREATER 1279)
  set(lookBack 1279)
endif()
# Calibration: the sample's 35 (L2R) and 40 (R2L) frames at 25 mph over its 559 px zone, scaled to this zone (speed lines
# 10% in from each side: 1023 of 1280, so 1023 W / 1280 sample px).  The keys are whole numbers, so they are the frames at
# 2.5 mph (ten times as many), which is the same calibration.
math(EXPR calL2R "(350 * 1023 * ${W} + 559 * 640) / (559 * 1280)")
math(EXPR calR2L "(400 * 1023 * ${W} + 559 * 640) / (559 * 1280)")
set(CFG ${OUT}/short_view_${FEET}ft.cfg)
file(WRITE ${CFG} "# Written by tests/short_view.cmake: the sample cropped to ${FEET} ft (${W} px), scaled to 1280x720.
L2RDirection = \"SE\"
R2LDirection = \"NW\"
LegalDirection = NW
obstruction = [0,0]
AnalysisBoxLeft = 0
AnalysisBoxTop = 65
AnalysisBoxWidth = 1279
AnalysisBoxHeight = 622
speedLineLeft = 128
speedLineRight = 1151
R2LStreetY = 399
L2RStreetY = 517
nextHeight = 278
maxL2RDistOnEntry = ${maxDist}
maxR2LDistOnEntry = ${maxDist}
entryLookBack = ${lookBack}
SLOP = ${slop}
largeVehicleArea = 100
CalibrationSpeed = 2.5
CalibrationFramesL2R = ${calL2R}
CalibrationFramesR2L = ${calR2L}
CalibrationFPS = 30
subFrameTiming = 1
")

set(CSV ${OUT}/stats_short_view_${FEET}ft.csv)
execute_process(
  COMMAND ${VST} --headless --input ${CLIP} --config ${CFG} --stats ${CSV}
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

# Match each short-view row to the nearest unused full-view row in the same direction.
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
    set(best_d 21)  # (StartFrame + EndFrame) within 20, so the middles within 10 frames
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
foreach(dir_need SE:23 NW:25)
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
  set(summary "${dir}: ${found} of ${n_dir} vehicles timed")
  if(found GREATER 0)
    set(e ${errors_${dir}})
    list(SORT e COMPARE NATURAL)
    math(EXPR m "(${found} + 1) / 2 - 1")       # nearest-rank median
    math(EXPR p "(9 * ${found} + 9) / 10 - 1")  # nearest-rank 90th percentile
    list(GET e ${m} median)
    list(GET e ${p} p90)
    list(GET e -1 worst)
    string(APPEND summary ", |speed error| median ${median}, p90 ${p90}, max ${worst} mph")
    list(APPEND all_errors ${e})
  endif()
  message(STATUS ${summary})
  if(found LESS need)
    string(APPEND failures "  ${dir}: timed ${found} of ${n_dir}, need at least ${need}\n")
  endif()
endforeach()
list(LENGTH all_errors n_all)
if(n_all GREATER 0)
  list(SORT all_errors COMPARE NATURAL)
  math(EXPR m "(${n_all} + 1) / 2 - 1")
  math(EXPR p "(9 * ${n_all} + 9) / 10 - 1")
  list(GET all_errors ${m} median)
  list(GET all_errors ${p} p90)
  list(GET all_errors -1 worst)
  message(STATUS "Both: |speed error| median ${median}, p90 ${p90}, max ${worst} mph; ${unmatched} unmatched rows")
  if(median GREATER 1 OR p90 GREATER 2 OR worst GREATER 4)
    string(APPEND failures "  |speed error| median ${median}, p90 ${p90}, max ${worst} mph; need <= 1, <= 2, <= 4\n")
  endif()
endif()
if(unmatched GREATER 3)
  string(APPEND failures "  ${unmatched} rows match no full-view vehicle; at most 3 allowed\n")
endif()

# Wrong-way: one "WRONG WAY: L2R" line per SE row, none for R2L.
set(n_SE 0)
foreach(d IN LISTS got_dirs)
  if(d STREQUAL "SE")
    math(EXPR n_SE "${n_SE} + 1")
  endif()
endforeach()
string(REGEX MATCHALL "WRONG WAY: L2R" ww_l2r "${log}")
string(REGEX MATCHALL "WRONG WAY: R2L" ww_r2l "${log}")
list(LENGTH ww_l2r n_ww_l2r)
list(LENGTH ww_r2l n_ww_r2l)
if(NOT n_ww_l2r EQUAL n_SE OR NOT n_ww_r2l EQUAL 0)
  string(APPEND failures "  wrong-way calls: ${n_ww_l2r} L2R and ${n_ww_r2l} R2L, expected ${n_SE} (one per SE row) and 0\n")
endif()

# The manifest records the setting and logs as many vehicles as the csv has.
read_manifest(${OUT}/run_short_view_${FEET}ft.json M)
if(NOT M_config_subFrameTiming EQUAL 1 OR NOT M_logged EQUAL n_got)
  string(APPEND failures "  manifest: subFrameTiming ${M_config_subFrameTiming} (expected 1), logged ${M_logged} (csv has ${n_got})\n")
endif()

if(failures)
  message(FATAL_ERROR "Short-view run (${CSV}) is off:\n${failures}${report}")
endif()
if(report)
  message(STATUS "Rows off by more than 1 mph, or unmatched:\n${report}")
endif()
