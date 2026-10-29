#!/usr/bin/env bash
# Builds the OpenCV modules VST uses as static libraries and installs them under $1.
#
# Release packages link OpenCV statically so every platform runs the same OpenCV (distros
# still ship 4.x) and installs don't depend on a distro OpenCV package. Only what VST needs
# is built: core, imgproc, geometry (boundingRect lives there in 5.x), videoio with FFmpeg,
# and highgui. FFmpeg and the GUI toolkit stay dynamic system libraries on Linux; on Windows
# OpenCV fetches its prebuilt FFmpeg plugin DLL, which is shipped next to the exes.
# IPP/OpenCL/LAPACK are off so results match across CPUs as closely as OpenCV allows.
#
# Usage: build-opencv.sh <install prefix>      (OPENCV_VERSION overrides the tag, default 5.0.0)
set -euo pipefail

prefix="$1"
version="${OPENCV_VERSION:-5.0.0}"
work="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/opencv-$version"

rm -rf "$work"
git clone --depth 1 --branch "$version" https://github.com/opencv/opencv.git "$work/src"

extra=()
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) extra+=(-A x64 -DBUILD_WITH_STATIC_CRT=OFF) ;;  # match VST's /MD runtime
esac

cmake -S "$work/src" -B "$work/build" "${extra[@]}" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_LIST=core,imgproc,geometry,imgcodecs,videoio,highgui \
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF -DBUILD_DOCS=OFF \
  -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_js=OFF \
  -DWITH_FFMPEG=ON -DWITH_GSTREAMER=OFF -DWITH_1394=OFF \
  -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_LAPACK=OFF -DWITH_EIGEN=OFF -DWITH_PROTOBUF=OFF \
  -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_OPENEXR=OFF -DWITH_AVIF=OFF
# A bare --parallel means unlimited make jobs, which runs the runner out of memory.
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")"
cmake --build "$work/build" --config Release --parallel "$jobs"
cmake --install "$work/build" --config Release

# UNIX installs don't include OpenCV's own LICENSE; put it next to the third-party notices.
cp "$work/src/LICENSE" "$prefix/LICENSE"
rm -rf "$work"
