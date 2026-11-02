#!/usr/bin/env bash
# Builds a small, LGPL-only, shared FFmpeg and installs it under $1, for the macOS app to carry inside its bundle.
#
# Why: the macOS release used to link Homebrew's OpenCV, so VSTStudio.app only opened after `brew install opencv`
# (and failed with no message otherwise). release.yml now links OpenCV statically (build-opencv.sh, like Linux and
# Windows) against this FFmpeg, and copies its five dylibs into VSTStudio.app/Contents/Frameworks.
#
# What is built, and why each choice:
#   - Shared libraries (--enable-shared --disable-static) with install names "@rpath/libX.N.dylib", so the copies
#     in Contents/Frameworks are found through the executables' @executable_path/../Frameworks rpath (set in
#     CMakeLists.txt) and stay separately replaceable, as the LGPL asks.
#   - No --enable-gpl / --enable-nonfree: the result is LGPL 2.1, the same terms as OpenCV's Windows FFmpeg plugin.
#   - --disable-autodetect, then only what macOS itself provides (zlib, VideoToolbox/AudioToolbox, Secure Transport
#     for https/rtsps cameras), so nothing from Homebrew on the build machine leaks in as a dependency.
#   - All of FFmpeg's own decoders, encoders, demuxers and protocols stay in: VST reads camera files (H.264/H.265,
#     MJPEG, AVI/MP4/MKV), RTSP and HTTP streams, and writes mp4v highlight clips.
#   - libavcodec, libavformat, libavutil, libswscale (OpenCV's videoio needs these) and libswresample; no programs,
#     docs, avdevice, avfilter or postproc.
# On Linux the same script works for trying the macOS configuration locally (the Linux packages themselves use the
# distro's FFmpeg); the Apple-only options are skipped there.
#
# Usage: build-ffmpeg.sh <install prefix>      (FFMPEG_VERSION overrides the tag, default n7.1.1)
set -euo pipefail

prefix="$1"
version="${FFMPEG_VERSION:-n7.1.1}"
work="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/ffmpeg-$version"

rm -rf "$work"
for attempt in 1 2 3; do
  git clone --depth 1 --branch "$version" https://github.com/FFmpeg/FFmpeg.git "$work" && break
  [ "$attempt" = 3 ] && exit 1
  rm -rf "$work"; sleep $((attempt * 15))
done

opts=(--prefix="$prefix" --enable-shared --disable-static --disable-programs --disable-doc
      --disable-avdevice --disable-avfilter --disable-postproc --disable-autodetect --enable-zlib)
case "$(uname -s)" in
  Darwin) opts+=(--install-name-dir='@rpath' --enable-videotoolbox --enable-audiotoolbox --enable-securetransport) ;;
  # x86 assembly needs nasm; without it FFmpeg still builds, just slower to decode.
  *) command -v nasm >/dev/null || opts+=(--disable-x86asm) ;;
esac

cd "$work"
./configure "${opts[@]}"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
make -j"$jobs"
make install

# The license text and the exact source the dylibs were built from, for licenses/ffmpeg in the bundle.
mkdir -p "$prefix/share/licenses/ffmpeg"
cp COPYING.LGPLv2.1 "$prefix/share/licenses/ffmpeg/COPYING.LGPLv2.1"
printf 'FFmpeg %s, LGPL 2.1, built by packaging/build-ffmpeg.sh from https://github.com/FFmpeg/FFmpeg/tree/%s\nconfigure %s\n' \
  "$version" "$version" "${opts[*]}" > "$prefix/share/licenses/ffmpeg/SOURCE.txt"
cd /
rm -rf "$work"
