#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 defcom5-rockchip
# fluster-run.sh — run the three AV1 conformance suites through ffmpeg -hwaccel vaapi with this driver.
# usage: FLUSTER=~/fluster LIBVA_DRIVERS_PATH=<dir holding rockchip_drv_video.so> OUT=<dir> ./fluster-run.sh
#   Downloads the suites on first use (Fluster: https://github.com/fluendo/fluster), one CSV per suite in OUT.
FLUSTER=${FLUSTER:-$HOME/fluster}; OUT=${OUT:-$PWD}
: "${LIBVA_DRIVERS_PATH:?set LIBVA_DRIVERS_PATH to the directory holding rockchip_drv_video.so}"
export LIBVA_DRIVERS_PATH LIBVA_DRIVER_NAME=rockchip
cd "$FLUSTER" || { echo "no Fluster checkout at $FLUSTER"; exit 1; }
echo "=== download $(date -Is)"
./fluster.py download -j 4 AV1-TEST-VECTORS CHROMIUM-8bit-AV1-TEST-VECTORS CHROMIUM-10bit-AV1-TEST-VECTORS 2>&1 | tail -5
for ts in AV1-TEST-VECTORS CHROMIUM-8bit-AV1-TEST-VECTORS CHROMIUM-10bit-AV1-TEST-VECTORS; do
  echo "=== run $ts $(date -Is)"
  ./fluster.py run -d FFmpeg-AV1-VAAPI -ts $ts -j 1 -t 120 -k -f csv -so "$OUT/fluster-$ts.csv" 2>&1 | tail -4
done
echo "=== done $(date -Is)"
