# Cross-platform build for VideoSpeedTracker (VST) and the highlights processor (ProcessHiLites).
# Builds the original sources in src/Wintel64 against OpenCV 4 on Linux, macOS or Windows.
cmake_minimum_required(VERSION 3.16)
project(VideoSpeedTracker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui videoio)

set(VST_DIR ${CMAKE_SOURCE_DIR}/src/Wintel64/VideoSpeedTracker)
add_executable(VideoSpeedTracker
  ${VST_DIR}/videoSpeedTracker.cpp
  ${VST_DIR}/Globals.cpp
  ${VST_DIR}/VehicleDynamics.cpp
  ${VST_DIR}/Projection.cpp
  ${VST_DIR}/Snapshot.cpp)
target_include_directories(VideoSpeedTracker PRIVATE ${VST_DIR})
target_link_libraries(VideoSpeedTracker PRIVATE ${OpenCV_LIBS})
# Keep a*b+c as two roundings everywhere. GCC on arm64 and Clang fuse them into FMA by default,
# which nudges tracked pixel positions by one and makes speeds differ from the x86 baseline.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(VideoSpeedTracker PRIVATE -ffp-contract=off)
endif()

# Interactive editor that trims a VST highlights video down to the vehicles worth posting.
add_executable(ProcessHiLites ${CMAKE_SOURCE_DIR}/src/Wintel64/ProcessHiLites/ProcessHilites.cpp)
target_link_libraries(ProcessHiLites PRIVATE ${OpenCV_LIBS})

enable_testing()
add_test(NAME sample_video_baseline
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-out
    -P ${CMAKE_SOURCE_DIR}/tests/run_sample.cmake)

# Packaging for release builds (.github/workflows/release.yml).
# Linux installs into the usual bin/ + share/ tree so `cpack -G DEB` can build a .deb whose OpenCV
# runtime dependencies are filled in by dpkg-shlibdeps for the distro it was built on.
# Windows and macOS install flat (exe next to VST.cfg) into a folder that gets zipped as-is.
set(VST_VERSION "0.0.0" CACHE STRING "Version stamped into release packages")
include(GNUInstallDirs)
if(WIN32 OR APPLE)
  set(VST_BIN_DIR .)
  set(VST_DATA_DIR .)
else()
  set(VST_BIN_DIR ${CMAKE_INSTALL_BINDIR})
  set(VST_DATA_DIR ${CMAKE_INSTALL_DATADIR}/videospeedtracker)
endif()
install(TARGETS VideoSpeedTracker ProcessHiLites RUNTIME DESTINATION ${VST_BIN_DIR})
install(FILES ${CMAKE_SOURCE_DIR}/bin/VST.cfg ${CMAKE_SOURCE_DIR}/bin/ProcessHiLites.cfg ${CMAKE_SOURCE_DIR}/README.md DESTINATION ${VST_DATA_DIR})

set(CPACK_PACKAGE_NAME videospeedtracker)
set(CPACK_PACKAGE_VERSION ${VST_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Measures vehicle speeds from fixed-camera street video")
set(CPACK_PACKAGE_CONTACT "Joe Lawson <joe@joekiller.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/joekiller/VideoSpeedTracker")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
