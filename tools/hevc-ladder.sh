#!/bin/bash
# hevc-ladder.sh — decode <video> through the TEST driver, dump N frames, decode the
# same frames in software, compare pixels.  Usage: tools/hevc-ladder.sh <video> [frames] [start]
# Env: DRV=~/deepink-test
#
# HARD RULE: a pixel verdict is only printed if the hardware pass PROVABLY used
# hardware.  Three independent signals must agree, because any one of them can lie:
#   1. hwdec-current from the status line (can be stale/absent without a TTY)
#   2. NO fallback marker anywhere in the run ("Attempting next decoding method",
#      "Using software decoding", "Could not copy back") -- catches a MID-RUN fallback,
#      which is the case that slipped through and produced a software-vs-software
#      "0.00 MATCH" for PC Claude
#   3. the driver actually assigned frames (assign_mpp_frame count > 0 in RK_VAAPI_LOG)
set -u
F="$1"; N="${2:-48}"; S="${3:-3}"; DRV="${DRV:-$HOME/deepink-test}"
L=$(basename "$F" | tr -c 'A-Za-z0-9\n' '_'); HW=/tmp/hw-$L; SW=/tmp/sw-$L
export LIBVA_DRIVERS_PATH="$DRV" LIBVA_DRIVER_NAME=rockchip RKVA_ADVERTISE_ALL=1 \
       RK_VAAPI_LOG=/tmp/rkva-$L.log RKVA_DUMP_HEVC=/tmp/pkt-$L.hevc
rm -rf "$HW" "$SW"; rm -f /tmp/rkva-$L.log /tmp/pkt-$L.hevc /tmp/mpvhw-$L.log; mkdir -p "$HW" "$SW"
MPV=$(command -v mpv); [ -x /opt/mpv038/bin/mpv ] && MPV=/opt/mpv038/bin/mpv
echo "## $F"
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,width,height,has_b_frames -of csv=p=0 "$F" 2>/dev/null | sed 's/^/   stream: /'
timeout 200 $MPV --no-config --hwdec=vaapi-copy --vo=image --vo-image-outdir="$HW" --vo-image-format=png \
  --ao=null --start=$S --frames=$N --msg-level=all=error,vd=v \
  --term-status-msg='HWDEC=${hwdec-current}' "$F" >/tmp/mpvhw-$L.log 2>&1
HWDEC=$(grep -oE 'HWDEC=[a-z0-9-]+' /tmp/mpvhw-$L.log | tail -1 | cut -d= -f2)
[ -z "$HWDEC" ] && HWDEC=$(grep -oE 'Using (hardware decoding \([a-z0-9-]+\)|software decoding)' /tmp/mpvhw-$L.log | tail -1 | grep -oE '\([a-z0-9-]+\)|software' | tr -d '()')
FELL=$(grep -cE 'Attempting next decoding method|Using software decoding|Could not copy back' /tmp/mpvhw-$L.log)
ASSIGNED=$(grep -c assign_mpp_frame /tmp/rkva-$L.log 2>/dev/null || echo 0)
echo "   hwdec-current=${HWDEC:-unknown}  fallback-markers=$FELL  frames-assigned=$ASSIGNED"
BAD=0
case "${HWDEC:-}" in ""|no|unknown|software) BAD=1;; esac
[ "$FELL" -gt 0 ] && BAD=1
[ "$ASSIGNED" -eq 0 ] && BAD=1
if [ $BAD -ne 0 ]; then
  echo "   ⛔ NOT A HARDWARE RUN — refusing to print a pixel verdict."
  echo "      (a software-vs-software comparison will happily say MATCH and mean nothing)"
  grep -m2 -E 'Attempting next decoding method|Failed to sync|Could not copy back' /tmp/mpvhw-$L.log | sed 's/^/      /'
  [ -s /tmp/pkt-$L.hevc ] && { echo "      ffmpeg parser on the assembled stream:"; ffmpeg -v error -f hevc -i /tmp/pkt-$L.hevc -f null - 2>&1 | sort | uniq -c | sort -rn | head -4 | sed 's/^/        /'; }
  exit 2
fi
timeout 200 $MPV --no-config --hwdec=no --vo=image --vo-image-outdir="$SW" --vo-image-format=png \
  --ao=null --start=$S --frames=$N --msg-level=all=error "$F" >/dev/null 2>&1
NH=$(ls "$HW" | wc -l); NS=$(ls "$SW" | wc -l)
echo "   frames: HW=$NH SW=$NS"
[ "$NH" -ne "$NS" ] && echo "   ⚠ frame-count mismatch — preroll differs (open-GOP + hr-seek); index-paired diffs may be meaningless"
python3 "$(dirname "$0")/hw-vs-sw-verdict.py" "$HW" "$SW"; RC=$?
echo "   log: sends=$(grep -c sending /tmp/rkva-$L.log 2>/dev/null) put_fail=$(grep -c 'decode_put_packet failed' /tmp/rkva-$L.log 2>/dev/null)"
grep -E 'RPS layout|NOTE|WARNING|refs_are_stale' /tmp/rkva-$L.log 2>/dev/null | head -2 | sed 's/^/   /'
exit $RC
