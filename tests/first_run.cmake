# First-run check for the per-user config (src/Wintel64/Common/UserPaths.h): install the build,
# run the installed VST with an empty home and no --config, and require that it seeds its
# config from the installed template, creates the data folders, and analyzes the sample.
# Invoked by ctest with -DBUILD=<build dir> -DSRC=<repo root> -DOUT=<work dir>.
file(REMOVE_RECURSE ${OUT})
execute_process(COMMAND ${CMAKE_COMMAND} --install ${BUILD} --prefix ${OUT}/prefix
  RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cmake --install failed (${rc})")
endif()
find_program(VST NAMES VideoSpeedTracker PATHS ${OUT}/prefix/bin ${OUT}/prefix NO_DEFAULT_PATH REQUIRED)

file(MAKE_DIRECTORY ${OUT}/home ${OUT}/cwd)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env HOME=${OUT}/home --unset=XDG_CONFIG_HOME --unset=XDG_DATA_HOME
          ${VST} --headless --input ${SRC}/sampleData/IPCam/20160130/manual_20160130172251.avi --stats ${OUT}/s.csv
  WORKING_DIRECTORY ${OUT}/cwd
  RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "First run without --config failed (${rc}):\n${log}")
endif()

if(APPLE)
  set(config ${OUT}/home/Library/Application\ Support/VideoSpeedTracker/VST.cfg)
  set(data ${OUT}/home/Library/Application\ Support/VideoSpeedTracker/Data)
else()
  set(config ${OUT}/home/.config/videospeedtracker/VST.cfg)
  set(data ${OUT}/home/.local/share/videospeedtracker)
endif()
foreach(p "${config}" "${data}/IPCam" "${data}/HiLites/forPosting" "${data}/Stats" "${data}/Trace")
  if(NOT EXISTS "${p}")
    message(FATAL_ERROR "First run didn't create ${p}:\n${log}")
  endif()
endforeach()
file(STRINGS "${config}" prefix_line REGEX "^dataPathPrefix")
string(FIND "${prefix_line}" "dataPathPrefix = ${data}" at)
if(NOT at EQUAL 0)
  message(FATAL_ERROR "Seeded config doesn't point at ${data}: ${prefix_line}")
endif()
