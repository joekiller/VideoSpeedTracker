# Regression check: run VST headless on the bundled 2016 Locust Ave sample and compare
# its csv to the baseline recorded from the first Linux/OpenCV 4 port.
# Invoked by ctest with -DVST=<exe> -DSRC=<repo root> -DOUT=<work dir>.
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
execute_process(
  COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
          ${OUT}/sample.csv ${SRC}/tests/baseline_manual_20160130172251.csv
  RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "Speeds differ from tests/baseline_manual_20160130172251.csv")
endif()
