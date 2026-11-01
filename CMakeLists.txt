# Cross-platform build for VideoSpeedTracker (VST) and the highlights processor (ProcessHiLites).
# Builds the original sources in src/Wintel64 against OpenCV 4 or 5 on Linux, macOS or Windows.
cmake_minimum_required(VERSION 3.16)
project(VideoSpeedTracker CXX)
# install() normalizes DESTINATION paths (./companion becomes companion): the same install tree, minus a dev
# warning per install() on CMake 3.31+.
if(POLICY CMP0177)
  cmake_policy(SET CMP0177 NEW)
endif()
# MSVC compiles a target's sources one at a time unless told otherwise; /MP spreads them over every core.
# /utf-8 makes the source and execution character sets UTF-8 for every target, not just VST Studio's.
if(MSVC)
  add_compile_options(/MP /utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# OpenCV 5 (release builds, Homebrew) or 4 (distro packages for local builds). 5.x moved
# boundingRect() into the new geometry module, which is linked when present.
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)
if(OpenCV_VERSION VERSION_LESS 4)
  message(FATAL_ERROR "OpenCV 4 or newer required, found ${OpenCV_VERSION}")
elseif(OpenCV_VERSION VERSION_GREATER_EQUAL 5)
  find_package(OpenCV REQUIRED COMPONENTS core imgproc geometry highgui videoio)
endif()
message(STATUS "Using OpenCV ${OpenCV_VERSION} (${OpenCV_INSTALL_PATH})")

set(VST_DIR ${CMAKE_SOURCE_DIR}/src/Wintel64/VideoSpeedTracker)
set(COMMON_DIR ${CMAKE_SOURCE_DIR}/src/Wintel64/Common)

# Per-user config/data locations (XDG, %APPDATA%, ~/Library/Application Support), shared by both programs.
add_library(vst_common STATIC ${COMMON_DIR}/UserPaths.cpp)
target_include_directories(vst_common PUBLIC ${COMMON_DIR})
if(WIN32)
  target_link_libraries(vst_common PUBLIC shell32 ole32)
endif()
add_executable(VideoSpeedTracker
  ${VST_DIR}/videoSpeedTracker.cpp
  ${VST_DIR}/Globals.cpp
  ${VST_DIR}/VehicleDynamics.cpp
  ${VST_DIR}/Projection.cpp
  ${VST_DIR}/Snapshot.cpp
  ${VST_DIR}/PiCamera.cpp
  ${VST_DIR}/FrameInput.cpp)
target_include_directories(VideoSpeedTracker PRIVATE ${VST_DIR})
target_link_libraries(VideoSpeedTracker PRIVATE vst_common ${OpenCV_LIBS})
# Keep a*b+c as two roundings everywhere. GCC on arm64 and Clang fuse them into FMA by default,
# which nudges tracked pixel positions by one and makes speeds differ from the x86 baseline.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(VideoSpeedTracker PRIVATE -ffp-contract=off)
endif()

# Software levelling for a camera not mounted level (Level.h): a tilted curb comes out level, about the centre, with
# black corners.  Header only, OpenCV core and imgproc.
add_executable(level_test tests/level_test.cpp)
target_include_directories(level_test PRIVATE ${VST_DIR})
target_link_libraries(level_test PRIVATE ${OpenCV_LIBS})
# The frame input port (FrameInput.h): source names, passwords kept out of labels, and every adapter (the sample through
# VideoCapture, a photo, a pushed feed) read through the port alone.
add_executable(frame_input_test tests/frame_input_test.cpp ${VST_DIR}/FrameInput.cpp ${VST_DIR}/PiCamera.cpp)
target_include_directories(frame_input_test PRIVATE ${VST_DIR})
target_link_libraries(frame_input_test PRIVATE ${OpenCV_LIBS})
# Angled lane lines and a receding street (Perspective.h, Globals::lane / vanishingX / zoneFraction): equal steps along a
# simulated street come out as equal steps of zoneFraction, and VST.cfg's checks refuse an unusable street edge.
add_executable(perspective_test tests/perspective_test.cpp ${VST_DIR}/Globals.cpp)
target_include_directories(perspective_test PRIVATE ${VST_DIR})

# Interactive editor that trims a VST highlights video down to the vehicles worth posting.
# It reads the highlights video through the same frame input port as the tracker (FrameInput.h).
add_executable(ProcessHiLites ${CMAKE_SOURCE_DIR}/src/Wintel64/ProcessHiLites/ProcessHilites.cpp
  ${VST_DIR}/FrameInput.cpp ${VST_DIR}/PiCamera.cpp)
target_include_directories(ProcessHiLites PRIVATE ${VST_DIR})
target_link_libraries(ProcessHiLites PRIVATE vst_common ${OpenCV_LIBS})

enable_testing()
add_test(NAME sample_video_baseline
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-out
    -P ${CMAKE_SOURCE_DIR}/tests/run_sample.cmake)
# minLoggedSpeed off (0) and raised (30), checked against the default run's manifest from sample_video_baseline.
# One test per value: each is a full pass over the sample, so under ctest -j the two run side by side.
set_tests_properties(sample_video_baseline PROPERTIES FIXTURES_SETUP sample_run)
foreach(min 0 30)
  add_test(NAME min_logged_speed_${min}
    COMMAND ${CMAKE_COMMAND}
      -DVST=$<TARGET_FILE:VideoSpeedTracker>
      -DSRC=${CMAKE_SOURCE_DIR}
      -DOUT=${CMAKE_BINARY_DIR}/test-min-speed
      -DBASE=${CMAKE_BINARY_DIR}/test-out
      -DMIN=${min}
      -P ${CMAKE_SOURCE_DIR}/tests/min_logged_speed.cmake)
  set_tests_properties(min_logged_speed_${min} PROPERTIES FIXTURES_REQUIRED sample_run)
endforeach()
add_test(NAME software_level COMMAND level_test)
add_test(NAME frame_input COMMAND frame_input_test ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/test-frame-input)
add_test(NAME perspective COMMAND perspective_test)
add_test(NAME config_parsing
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-config
    -P ${CMAKE_SOURCE_DIR}/tests/config_cases.cmake)
# Windows resolves %APPDATA% through the Known Folder API, which an env override can't redirect,
# so the first-run test would touch the real profile there; it runs on Linux and macOS only.
if(NOT WIN32)
  add_test(NAME first_run_user_config
    COMMAND ${CMAKE_COMMAND}
      -DBUILD=${CMAKE_BINARY_DIR}
      -DSRC=${CMAKE_SOURCE_DIR}
      -DOUT=${CMAKE_BINARY_DIR}/first-run
      -P ${CMAKE_SOURCE_DIR}/tests/first_run.cmake)
endif()

# Packaging for release builds (.github/workflows/release.yml).
# Linux installs into the usual bin/ + share/ tree so `cpack -G DEB` can build a .deb whose OpenCV
# runtime dependencies are filled in by dpkg-shlibdeps for the distro it was built on.
# Windows and macOS install flat (exe next to VST.cfg) into a folder that gets zipped as-is.
set(VST_VERSION "0.0.0" CACHE STRING "Version stamped into release packages")
# The engine writes it into each run manifest (run_<name>.json, "vst").
target_compile_definitions(VideoSpeedTracker PRIVATE VST_VERSION="${VST_VERSION}")
include(GNUInstallDirs)
if(WIN32 OR APPLE)
  set(VST_BIN_DIR .)
  set(VST_DATA_DIR .)
else()
  set(VST_BIN_DIR ${CMAKE_INSTALL_BINDIR})
  set(VST_DATA_DIR ${CMAKE_INSTALL_DATADIR}/videospeedtracker)
endif()
install(TARGETS VideoSpeedTracker ProcessHiLites RUNTIME DESTINATION ${VST_BIN_DIR})
# VST.cfg / ProcessHiLites.cfg are read-only templates; on first run each program copies its own into the
# per-user config folder (src/Wintel64/Common/UserPaths.h), so installs never need a writable program folder.
# The phone companion (companion/README.md) is static files; VST Studio serves them when Remote companion is on.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/companion/ DESTINATION ${VST_DATA_DIR}/companion
  PATTERN dev EXCLUDE PATTERN test EXCLUDE PATTERN README.md EXCLUDE)
install(FILES ${CMAKE_SOURCE_DIR}/bin/VST.cfg ${CMAKE_SOURCE_DIR}/bin/ProcessHiLites.cfg ${CMAKE_SOURCE_DIR}/README.md DESTINATION ${VST_DATA_DIR})
install(FILES ${CMAKE_SOURCE_DIR}/docs/getting-started.md DESTINATION ${VST_DATA_DIR} RENAME GETTING-STARTED.md)
# The 2016 sample clip (~80 MB) that the VST.cfg template is calibrated for, so `VideoSpeedTracker --demo` (or a
# bare interactive run with no videos yet) has something to analyze; userpaths::sampleVideo() finds it here.
option(VST_BUNDLE_SAMPLE "Install the sample video with the programs" ON)
if(VST_BUNDLE_SAMPLE)
  install(FILES ${CMAKE_SOURCE_DIR}/sampleData/IPCam/20160130/manual_20160130172251.avi DESTINATION ${VST_DATA_DIR}/sample)
endif()

# The BSD license requires binary redistributions to carry the notice. Linux gets a Debian copyright file;
# Windows also gets the MSVC runtime DLLs (redistributable, app-local) since opencv_world needs them too.
if(WIN32 OR APPLE)
  install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION . RENAME LICENSE.txt)
else()
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/copyright DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/videospeedtracker)
endif()
# A static OpenCV (packaging/build-opencv.sh) is compiled into the exes, so ship its Apache-2.0
# LICENSE and the notices for the third-party code it bundles.
if(NOT OpenCV_SHARED AND EXISTS ${OpenCV_INSTALL_PATH}/LICENSE)
  if(WIN32 OR APPLE)
    set(VST_OPENCV_NOTICE_DIR licenses/opencv)
  else()
    set(VST_OPENCV_NOTICE_DIR ${CMAKE_INSTALL_DATADIR}/doc/videospeedtracker/opencv)
  endif()
  install(FILES ${OpenCV_INSTALL_PATH}/LICENSE DESTINATION ${VST_OPENCV_NOTICE_DIR})
  foreach(dir ${OpenCV_INSTALL_PATH}/share/licenses/opencv5 ${OpenCV_INSTALL_PATH}/etc/licenses)
    if(IS_DIRECTORY ${dir})
      install(DIRECTORY ${dir}/ DESTINATION ${VST_OPENCV_NOTICE_DIR})
    endif()
  endforeach()
endif()
if(WIN32)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  include(InstallRequiredSystemLibraries)
endif()

# VST Studio, the desktop app (src/Gui); fetches its dependencies at configure time.
option(VST_BUILD_GUI "Build VST Studio, the desktop app (fetches Dear ImGui, GLFW and fonts at configure time)" ON)
if(VST_BUILD_GUI)
  add_subdirectory(src/Gui)
endif()

# The street report (src/Gui/Report.cpp) built from fixture csvs, manifests and calibration history
# (tests/fixtures/report/) and from the 2016 sample csv: key numbers, graceful handling of old data, no external
# references.  Links only the report library, no OpenGL.
if(TARGET vst_report)
  add_executable(report_test tests/report_test.cpp)
  target_link_libraries(report_test PRIVATE vst_report)
  add_test(NAME street_report
    COMMAND report_test ${CMAKE_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/report-test)

  # Unattended-run health (src/Gui/Health.cpp): pruning video to the disk budget, the "camera moved" check on
  # frames of the sample video, and the daily health.csv row.
  add_executable(health_test tests/health_test.cpp)
  target_link_libraries(health_test PRIVATE vst_report)
  add_test(NAME unattended_health
    COMMAND health_test ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/health-test)

  # The site planner's model (src/Gui/Plan.cpp): street presets, measuring the street from a calibration or from
  # car lengths across the picture, and the verdicts the planner and the phone show.
  add_executable(plan_test tests/plan_test.cpp)
  target_link_libraries(plan_test PRIVATE vst_report)
  add_test(NAME site_planner COMMAND plan_test)

  # Calibrating from a Tesla dashcam clip (src/Gui/DashCam.cpp): reads per-frame speed from synthetic TeslaCam MP4s
  # and picks a pass's speed out of them.
  add_executable(dashcam_test tests/dashcam_test.cpp)
  target_link_libraries(dashcam_test PRIVATE vst_report)
  add_test(NAME dashcam_calibration
    COMMAND dashcam_test ${CMAKE_BINARY_DIR}/dashcam-test)

  # Distance calibration (src/Gui/Calibration.h): cones/pole spacing and a known car's length, with the depth
  # correction for a car parked beyond the traffic lane checked against a pinhole camera.  Header only.
  add_executable(calibration_test tests/calibration_test.cpp)
  target_include_directories(calibration_test PRIVATE ${CMAKE_SOURCE_DIR}/src/Gui)
  add_test(NAME distance_calibration COMMAND calibration_test)
endif()

# Live-stream reconnect: ffmpeg serves the sample as an HTTP stream that drops out and comes back
# (tests/stream_reconnect.sh). Only where ffmpeg and bash are available; about 45 s. With VST Studio
# built, it also checks that Stop ends an embedded run promptly while it is waiting to reconnect.
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(BASH_EXECUTABLE bash)
if(FFMPEG_EXECUTABLE AND BASH_EXECUTABLE AND NOT WIN32)
  set(engine_test "")
  if(VST_BUILD_GUI)
    add_executable(engine_reconnect_test tests/engine_reconnect_test.cpp)
    target_link_libraries(engine_reconnect_test PRIVATE vst_engine)
    set(engine_test $<TARGET_FILE:engine_reconnect_test>)
  endif()
  add_test(NAME live_stream_reconnect
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/stream_reconnect.sh
      $<TARGET_FILE:VideoSpeedTracker> ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/test-reconnect
      ${FFMPEG_EXECUTABLE} ${engine_test})
  # Real-time: ffmpeg gets half a second to bind before VST connects and the dropout waits are wall-clock, so with
  # other tests loading every core (ctest -j) it can miss its cues. It runs on its own, after the parallel ones.
  set_tests_properties(live_stream_reconnect PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

  # Raspberry Pi camera source (PiCamera.cpp) with tests/fake_rpicam_vid.sh playing the sample in place of rpicam-vid:
  # listing, the recorder's arguments, tracking against the baseline, reconnecting, the frame rate check and the failure
  # messages (tests/picam_source.sh). About 5 s. Serial because one step times frames against the wall clock.
  add_test(NAME picam_source
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/picam_source.sh
      $<TARGET_FILE:VideoSpeedTracker> ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/test-picam ${FFMPEG_EXECUTABLE})
  set_tests_properties(picam_source PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

# Short view (subFrameTiming): the sample cropped by ffmpeg to 20 ft and to 15 ft of street (the shortest view VST Studio
# plans for is about 14 ft), each timed against the full view's speeds (tests/short_view.cmake). About 90 s each: cutting
# the clip, then a full pass over it at 1280x720. Without ffmpeg the
# script prints SHORT_VIEW_SKIPPED and ctest reports the test as skipped. Run on its own: under ctest -j it loads every core
# for a minute or more, which can push companion_api_smoke past its time limits.
if(NOT FFMPEG_EXECUTABLE)
  set(FFMPEG_EXECUTABLE "")
endif()
add_test(NAME short_view
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-short-view
    -DFFMPEG=${FFMPEG_EXECUTABLE}
    -P ${CMAKE_SOURCE_DIR}/tests/short_view.cmake)
add_test(NAME short_view_15ft
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-short-view
    -DFFMPEG=${FFMPEG_EXECUTABLE}
    -DFEET=15
    -P ${CMAKE_SOURCE_DIR}/tests/short_view.cmake)
# The 20 ft clip again, warped so the street runs at a slant and recedes (a camera aimed partly along it), timed with
# angled lane lines and a street edge so short-view timing measures along the ground (Perspective.h).
add_test(NAME receding_view
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-short-view
    -DFFMPEG=${FFMPEG_EXECUTABLE}
    -DRECEDE=1
    -P ${CMAKE_SOURCE_DIR}/tests/short_view.cmake)
set_tests_properties(short_view short_view_15ft receding_view PROPERTIES SKIP_REGULAR_EXPRESSION "SHORT_VIEW_SKIPPED" TIMEOUT 400 RUN_SERIAL TRUE)

# VST Studio's companion API (docs/companion-api.md), end to end: the real app under a virtual display, driven with
# curl, run on the sample video and checked against the same baseline as sample_video_baseline. Needs bash, xvfb-run
# and curl; where any of them or the GUI build is missing the script prints API_SMOKE_SKIPPED and ctest reports the
# test as skipped rather than passed. Not on Windows, which has neither bash nor Xvfb.
if(NOT WIN32)
  if(TARGET VSTStudio)
    set(VST_STUDIO_EXE $<TARGET_FILE:VSTStudio>)
  else()
    set(VST_STUDIO_EXE "")
  endif()
  add_test(NAME companion_api_smoke
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/api_smoke.sh "${VST_STUDIO_EXE}" ${CMAKE_SOURCE_DIR}
      ${CMAKE_BINARY_DIR}/api-smoke ${CMAKE_COMMAND})
  set_tests_properties(companion_api_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "API_SMOKE_SKIPPED" TIMEOUT 300)
endif()

set(CPACK_PACKAGE_NAME videospeedtracker)
set(CPACK_PACKAGE_VERSION ${VST_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Measures vehicle speeds from fixed-camera street video")
set(CPACK_PACKAGE_CONTACT "Joe Lawson <joe@joekiller.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/joekiller/VideoSpeedTracker")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION video)
# VST Studio's open/save dialogs use zenity (or kdialog) on Linux; without either it falls back to typed paths.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "zenity | kdialog")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
