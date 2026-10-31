#!/usr/bin/env bash
# Stand-in for rpicam-vid, so the Pi camera path (src/Wintel64/VideoSpeedTracker/PiCamera.cpp) can be tested without a
# Raspberry Pi.  VST runs it through VST_RPICAM_VID.  Controlled by environment variables:
#   FAKE_RPICAM_FFMPEG   ffmpeg to play it with (default: ffmpeg on PATH)
#   FAKE_RPICAM_SENSOR   sensor to list (default imx296, the Global Shutter Camera); "none" lists no camera
#   FAKE_RPICAM_VIDEO    video to play as the camera's picture
#   FAKE_RPICAM_SECONDS  how much of it to play before "the camera" stops (default 10)
#   FAKE_RPICAM_FAIL     busy | nopictures: fail the way a real camera does when another program has it, or hangs
#   FAKE_RPICAM_ARGS     file each recording's arguments are appended to, one line per start
#   FAKE_RPICAM_REALTIME 1: send frames at the video's own rate (30 fps), whatever --framerate asked for, like a sensor
#                        mode that can't keep up; otherwise as fast as VST reads them
# Recording writes raw I420 frames at --width x --height to stdout, as `rpicam-vid --codec yuv420 -o -` does.
set -u
sensor=${FAKE_RPICAM_SENSOR:-imx296}
if [[ " $* " == *" --list-cameras "* ]]; then
	if [[ $sensor == none ]]; then echo "No cameras available!"; exit 0; fi
	echo "Available cameras"
	echo "-----------------"
	case $sensor in
	imx296) echo "0 : imx296 [1456x1088 10-bit RGGB] (/base/axi/pcie@120000/rp1/i2c@88000/imx296@1a)"
	        echo "    Modes: 'SRGGB10_CSI2P' : 1456x1088 [60.38 fps - (0, 0)/1456x1088 crop]" ;;
	imx708) echo "0 : imx708 [4608x2592 10-bit RGGB] (/base/axi/pcie@120000/rp1/i2c@88000/imx708@1a)"
	        echo "    Modes: 'SRGGB10_CSI2P' : 1536x864 [120.13 fps - (768, 432)/3072x1728 crop]"
	        echo "                             2304x1296 [56.03 fps - (0, 0)/4608x2592 crop]"
	        echo "                             4608x2592 [14.35 fps - (0, 0)/4608x2592 crop]" ;;
	imx219) echo "0 : imx219 [3280x2464 8-bit RGGB] (/base/soc/i2c0mux/i2c@1/imx219@10)"
	        echo "    Modes: 'SRGGB10_CSI2P' : 640x480 [206.65 fps - (1000, 752)/1280x960 crop]"
	        echo "                             1640x1232 [41.85 fps - (0, 0)/3280x2464 crop]"
	        echo "                             1920x1080 [47.57 fps - (680, 692)/1920x1080 crop]"
	        echo "                             3280x2464 [21.19 fps - (0, 0)/3280x2464 crop]" ;;
	esac
	exit 0
fi
[[ -n ${FAKE_RPICAM_ARGS:-} ]] && echo "$*" >> "$FAKE_RPICAM_ARGS"
case ${FAKE_RPICAM_FAIL:-} in
busy) echo "ERROR: *** failed to acquire camera /base/axi/pcie@120000/rp1/i2c@88000/imx296@1a ***" >&2
      echo "Device or resource busy" >&2; exit 1 ;;
nopictures) sleep 30; exit 1 ;;
esac
w=1280 h=720
while (($#)); do
	case $1 in --width) w=$2; shift ;; --height) h=$2; shift ;; esac
	shift
done
re=()
[[ ${FAKE_RPICAM_REALTIME:-} == 1 ]] && re=(-re)
exec "${FAKE_RPICAM_FFMPEG:-ffmpeg}" -nostdin -hide_banner -loglevel error "${re[@]}" -t "${FAKE_RPICAM_SECONDS:-10}" -i "$FAKE_RPICAM_VIDEO" -an \
	-vf "scale=$w:$h" -pix_fmt yuv420p -f rawvideo -
