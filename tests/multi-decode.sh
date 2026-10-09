#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# multi-decode.sh — several decoders in ONE ffmpeg process on ONE VADisplay, every output frame compared with a
# software decode of the same clip. This is the shape that broke before v2.5.0: Firefox's media process shares one
# VADisplay between all its decoders, and decoders starting together raced for the driver's slots (KI-12).
#
# Usage:  tests/multi-decode.sh clip [clip ...]      (only each clip's first video stream is compared)
# Env:    ROUNDS=10      runs; a run passes only if every output is complete and bit-identical to software decode
#         FRAMES=120     frames compared per clip
#         FORCED=1       keep frames in VA surfaces and download them explicitly, so a silent fallback to software
#                        decoding fails the run (use 0 when a clip is expected to fall back, e.g. more clips than
#                        the driver's 8 context slots)
#         DRIVERS=dir    test the driver in dir (LIBVA_DRIVERS_PATH); unset = the installed driver
#         RENDER=/dev/dri/renderD128   TIMEOUT=120 (seconds per run)
#         RK_VAAPI_LOG   honoured as usual (logging widens race windows: run with and without it)
# Needs ffmpeg 7 or newer: older ffmpeg decodes every input on one thread, so the decoders never overlap.
# Exit:   0 when every run passed.
set -u
[ $# -ge 1 ] || { sed -n 3,17p "$0"; exit 2; }
ROUNDS=${ROUNDS:-10}; FRAMES=${FRAMES:-120}; FORCED=${FORCED:-1}; RENDER=${RENDER:-/dev/dri/renderD128}
TIMEOUT=${TIMEOUT:-120}
for v in ROUNDS FRAMES TIMEOUT; do
    [[ ${!v} =~ ^[1-9][0-9]*$ ]] || { echo "$v must be a whole number of at least 1 (got '${!v}')"; exit 2; }
done
major=$(ffmpeg -hide_banner -version 2>/dev/null | sed -n 's/^ffmpeg version n\{0,1\}\([0-9]*\).*/\1/p')
[ -n "$major" ] && [ "$major" -lt 7 ] && echo "warning: ffmpeg $major runs all decoders on one thread; this test cannot show the race"
W=$(mktemp -d /tmp/multi-decode.XXXXXX)
trap 'echo "interrupted (work dir $W)"; exit 130' INT TERM
C=("$@")
fmt() {   # hardware download format : software comparison format
    local pf; pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$1") || return 1
    [ -n "$pf" ] || return 1
    case "$pf" in *p10*|*p010*) echo p010le:yuv420p10le ;; *) echo nv12:yuv420p ;; esac
}
F=()
for i in "${!C[@]}"; do
    [ -r "${C[$i]}" ] || { echo "cannot read ${C[$i]}"; exit 2; }
    F[$i]=$(fmt "${C[$i]}") || { echo "ffprobe found no video stream in ${C[$i]}"; exit 2; }
    ffmpeg -y -hide_banner -nostdin -loglevel error -noautorotate -i "${C[$i]}" -map 0:v:0 -frames:v "$FRAMES" \
        -pix_fmt "${F[$i]#*:}" -f framemd5 "$W/ref-$i.md5" || { echo "software reference failed for ${C[$i]}"; exit 2; }
    grep -qv '^#' "$W/ref-$i.md5" || { echo "software reference for ${C[$i]} has no frames"; exit 2; }
done
env=(LIBVA_DRIVER_NAME=rockchip)
[ -n "${DRIVERS:-}" ] && env+=(LIBVA_DRIVERS_PATH="$DRIVERS")
ok=0; fail=0
for r in $(seq 1 "$ROUNDS"); do
    a=()
    for c in "${C[@]}"; do
        a+=(-hwaccel vaapi -hwaccel_device va)
        [ "$FORCED" = 1 ] && a+=(-hwaccel_output_format vaapi)
        a+=(-noautorotate -i "$c")
    done
    for i in "${!C[@]}"; do
        a+=(-map "$i:v:0" -frames:v "$FRAMES")
        [ "$FORCED" = 1 ] && a+=(-vf "hwdownload,format=${F[$i]%%:*}")
        a+=(-pix_fmt "${F[$i]#*:}" -f framemd5 "$W/out-$r-$i.md5")
    done
    env "${env[@]}" timeout --foreground -k 5 "$TIMEOUT" ffmpeg -y -hide_banner -nostdin -loglevel error \
        -init_hw_device "vaapi=va:$RENDER" "${a[@]}" >/dev/null 2>"$W/err-$r.txt"
    rc=$?; bad=""
    for i in "${!C[@]}"; do
        cmp -s <(grep -v '^#' "$W/ref-$i.md5") <(grep -v '^#' "$W/out-$r-$i.md5" 2>/dev/null) || bad+=" $(basename "${C[$i]}")"
    done
    if [ $rc -eq 0 ] && [ -z "$bad" ]; then ok=$((ok+1)); else
        fail=$((fail+1))
        case $rc in 124|137) why="timed out after ${TIMEOUT}s" ;; *) why="rc=$rc, wrong:$bad" ;; esac
        echo "run $r: $why (stderr kept in $W/err-$r.txt)"
    fi
done
echo "${#C[@]} decoders, forced=$FORCED, $FRAMES frames: $ok ok / $fail fail   (work dir $W)"
[ $fail -eq 0 ]
