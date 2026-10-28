# Cross-platform build for VideoSpeedTracker (VST) and the highlights processor.
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

enable_testing()
add_test(NAME sample_video_baseline
  COMMAND ${CMAKE_COMMAND}
    -DVST=$<TARGET_FILE:VideoSpeedTracker>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}/test-out
    -P ${CMAKE_SOURCE_DIR}/tests/run_sample.cmake)
