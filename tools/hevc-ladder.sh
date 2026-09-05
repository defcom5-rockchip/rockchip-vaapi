#!/bin/bash
# hevc-ladder.sh — decode <video> through the TEST driver (env-dir) with mpv vaapi-copy,
# dump N frames, decode the same frames in software, compare pixels objectively.
#   Usage: tools/hevc-ladder.sh <video> [frames=20] [start=3]
#   Env:   DRV=~/deepink-test   (dir holding rockchip_drv_video.so)
# The verdict ALWAYS reports hwdec-current, so a software run can never masquerade
# as a hardware one, and every temp dir is cleaned first so a stale dump from a
# previous build can never be re-scored as this build's result.
set -u
F="$1"; N="${2:-20}"; S="${3:-3}"; DRV="${DRV:-$HOME/deepink-test}"
L=$(basename "$F" | tr -c 'A-Za-z0-9\n' '_')
HW=/tmp/hw-$L; SW=/tmp/sw-$L
export LIBVA_DRIVERS_PATH="$DRV" LIBVA_DRIVER_NAME=rockchip RKVA_ADVERTISE_ALL=1 \
       RK_VAAPI_LOG=/tmp/rkva-$L.log RKVA_DUMP_HEVC=/tmp/pkt-$L.hevc
# CLEAN FIRST — stale frames from an earlier build would otherwise be re-scored.
rm -rf "$HW" "$SW"; rm -f /tmp/rkva-$L.log /tmp/pkt-$L.hevc; mkdir -p "$HW" "$SW"
MPV=$(command -v mpv); [ -x /opt/mpv038/bin/mpv ] && MPV=/opt/mpv038/bin/mpv
echo "## $F"
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,width,height,has_b_frames -of csv=p=0 "$F" 2>/dev/null | sed 's/^/   stream: /'
# hardware pass — capture the decode path mpv ACTUALLY used
HWDEC=$(timeout 200 $MPV --no-config --hwdec=vaapi-copy --vo=image --vo-image-outdir="$HW" --vo-image-format=png \
  --ao=null --start=$S --frames=$N --msg-level=all=error,vd=v \
  --term-status-msg='HWDEC=${hwdec-current}' "$F" 2>&1 | tee /tmp/mpvhw-$L.log | grep -oE 'HWDEC=[a-z0-9-]+' | tail -1 | cut -d= -f2)
[ -z "$HWDEC" ] && HWDEC=$(grep -oE 'Using (hardware decoding \([a-z0-9-]+\)|software decoding)' /tmp/mpvhw-$L.log | tail -1 | grep -oE '\([a-z0-9-]+\)|software' | tr -d '()')
echo "   hwdec-current: ${HWDEC:-unknown}"
case "${HWDEC:-}" in ""|no|software) echo "   ⚠ SOFTWARE PATH — a pixel MATCH here proves nothing about hardware decode";; esac
timeout 200 $MPV --no-config --hwdec=no --vo=image --vo-image-outdir="$SW" --vo-image-format=png \
  --ao=null --start=$S --frames=$N --msg-level=all=error "$F" >/dev/null 2>&1
echo "   frames: HW=$(ls "$HW" | wc -l) SW=$(ls "$SW" | wc -l)"
python3 "$(dirname "$0")/hw-vs-sw-verdict.py" "$HW" "$SW"; RC=$?
echo "   log: sends=$(grep -c sending /tmp/rkva-$L.log 2>/dev/null) put_fail=$(grep -c 'decode_put_packet failed' /tmp/rkva-$L.log 2>/dev/null) synth=$(grep -c 'VPS/SPS/PPS synth' /tmp/rkva-$L.log 2>/dev/null)"
grep -E 'RPS layout|NOTE|WARNING' /tmp/rkva-$L.log 2>/dev/null | head -2 | sed 's/^/   /'
if [ $RC -ne 0 ] && [ -s /tmp/pkt-$L.hevc ]; then
  echo "   ffmpeg parser on the assembled stream:"
  ffmpeg -v error -f hevc -i /tmp/pkt-$L.hevc -f null - 2>&1 | sort | uniq -c | sort -rn | head -5 | sed 's/^/     /'
fi
exit $RC
