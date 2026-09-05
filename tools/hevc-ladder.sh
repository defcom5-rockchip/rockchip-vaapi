#!/bin/bash
# hevc-ladder.sh — decode <file> through the TEST driver (env-dir) with mpv vaapi-copy,
# dump N frames, decode the same frames in software, and compare pixels objectively.
# Usage: tools/hevc-ladder.sh <video> [frames=20] [start=3]
# Env: DRV=~/deepink-test (dir holding rockchip_drv_video.so)
set -u
F="$1"; N="${2:-20}"; S="${3:-3}"; DRV="${DRV:-$HOME/deepink-test}"; L=$(basename "$F" | tr -c 'A-Za-z0-9\n' '_')
export LIBVA_DRIVERS_PATH="$DRV" LIBVA_DRIVER_NAME=rockchip RKVA_ADVERTISE_ALL=1 RK_VAAPI_LOG=/tmp/rkva-$L.log RKVA_DUMP_HEVC=/tmp/pkt-$L.hevc
rm -f /tmp/rkva-$L.log /tmp/pkt-$L.hevc; rm -rf /tmp/hw-$L /tmp/sw-$L; mkdir -p /tmp/hw-$L /tmp/sw-$L
MPV=$(command -v mpv); [ -x /opt/mpv038/bin/mpv ] && MPV=/opt/mpv038/bin/mpv
echo "## $F"; ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,width,height -of csv=p=0 "$F"
timeout 200 $MPV --no-config --hwdec=vaapi-copy --vo=image --vo-image-outdir=/tmp/hw-$L --vo-image-format=png --ao=null --start=$S --frames=$N --msg-level=all=error,vd=v "$F" 2>&1 | grep -iE 'Using (hardware|software) decoding|internal decoding|Attempting next'
timeout 200 $MPV --no-config --hwdec=no --vo=image --vo-image-outdir=/tmp/sw-$L --vo-image-format=png --ao=null --start=$S --frames=$N --msg-level=all=error "$F" >/dev/null 2>&1
python3 "$(dirname "$0")/hw-vs-sw-verdict.py" /tmp/hw-$L /tmp/sw-$L; RC=$?
echo "log: sends=$(grep -c sending /tmp/rkva-$L.log) put_fail=$(grep -c 'decode_put_packet failed' /tmp/rkva-$L.log) synth=$(grep -c 'VPS/SPS/PPS synth' /tmp/rkva-$L.log)"; grep -E 'RPS layout|WARNING' /tmp/rkva-$L.log | head -2
[ $RC -ne 0 ] && [ -s /tmp/pkt-$L.hevc ] && { echo "ffmpeg parser on the assembled stream:"; ffmpeg -v error -f hevc -i /tmp/pkt-$L.hevc -f null - 2>&1 | sort | uniq -c | sort -rn | head -5; }
exit $RC
