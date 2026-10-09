#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# abandon-soak.sh — a player that opens a VA-API decoder and abandons it within a fraction of a second, over and over,
# in one process. Before v2.5.0 every abandoned decoder stranded its MPP buffers, and a looping player exhausted the
# decoder's IOMMU after ~55 attempts (KI-12). Samples the player's dma-buf fds every 30 s: flat = the drain works.
#
# Needs a desktop session (it fails without a display) where mpv's zero-copy attempt reaches the driver and then
# falls back, e.g. an 8-bit clip on a GL driver that cannot import NV12 per plane. Use a clip of a few seconds; each
# play is cut at 3 s anyway. To confirm the abandon path, run once with RK_VAAPI_LOG=/some/file: it must show about
# two "mpp_init OK" lines for every "Playing:" line in the output.
#
# Usage:  tests/abandon-soak.sh clip
# Env:    MINUTES=10, FD_LIMIT=100 (fail if the player ever holds more dma-buf fds), DRIVERS=dir (LIBVA_DRIVERS_PATH;
#         unset = installed driver), MPV=mpv (must exec mpv, or its own fds are counted)
# Exit:   0 when the run lasted, abandoned at least 60 decoders and stayed under FD_LIMIT.
set -u
[ $# -eq 1 ] || { sed -n 3,15p "$0"; exit 2; }
[ -r "$1" ] || { echo "cannot read $1"; exit 2; }
MINUTES=${MINUTES:-10}; FD_LIMIT=${FD_LIMIT:-100}; MPV=${MPV:-mpv}
for v in MINUTES FD_LIMIT; do
    [[ ${!v} =~ ^[1-9][0-9]*$ ]] || { echo "$v must be a whole number of at least 1 (got '${!v}')"; exit 2; }
done
W=$(mktemp -d /tmp/abandon-soak.XXXXXX)
env=(LIBVA_DRIVER_NAME=rockchip)
[ -n "${DRIVERS:-}" ] && env+=(LIBVA_DRIVERS_PATH="$DRIVERS")
env "${env[@]}" "$MPV" --hwdec=vaapi,vaapi-copy --geometry=640x360 --audio=no --end=3 \
    --msg-level=all=no,cplayer=info,vd=info --loop-playlist=force "$1" "$1" > "$W/mpv.out" 2>&1 &
P=$!
trap 'kill $P 2>/dev/null; wait $P 2>/dev/null' EXIT   # don't return while the player is still alive
trap 'exit 130' INT TERM HUP
T0=$(date +%s); max=0; sum=0; n=0; files=0; status=0
while sleep 30; do
    t=$(( $(date +%s) - T0 ))
    if grep -qE "Error opening|No video|video_out" "$W/mpv.out"; then
        echo "t=${t}s: the player cannot open the clip or the display (see $W/mpv.out)"; status=1; break; fi
    kill -0 $P 2>/dev/null || { echo "t=${t}s: player exited early (see $W/mpv.out)"; status=1; break; }
    fds=$(ls -l /proc/$P/fd 2>/dev/null | grep -c 'anon_inode:dmabuf\|/dmabuf:'); files=$(grep -c "Playing:" "$W/mpv.out")
    echo "t=${t}s files=$files dmabuf_fds=$fds"
    n=$((n+1)); sum=$((sum+fds)); [ "$fds" -gt "$max" ] && max=$fds
    [ "$fds" -gt "$FD_LIMIT" ] && { echo "dma-buf fds above $FD_LIMIT: decoder buffers are being stranded"; status=1; break; }
    [ $t -ge $((MINUTES*60)) ] && break
done
[ $n -gt 0 ] && echo "samples=$n files=$files dmabuf_fds max=$max mean=$((sum/n))"
[ $status -eq 0 ] && [ "$files" -lt 60 ] && { echo "only $files decoders abandoned: too few to show a leak"; status=1; }
exit $status
