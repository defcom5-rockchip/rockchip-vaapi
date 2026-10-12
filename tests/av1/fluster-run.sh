#!/bin/bash
cd /home/plaiddesktop/fluster
export LIBVA_DRIVERS_PATH=/tmp/av1/drv-full LIBVA_DRIVER_NAME=rockchip
echo "=== download (cached) $(date -Is)"
./fluster.py download -j 4 AV1-TEST-VECTORS CHROMIUM-8bit-AV1-TEST-VECTORS CHROMIUM-10bit-AV1-TEST-VECTORS 2>&1 | tail -5
for ts in AV1-TEST-VECTORS CHROMIUM-8bit-AV1-TEST-VECTORS CHROMIUM-10bit-AV1-TEST-VECTORS; do
  echo "=== run $ts $(date -Is)"
  ./fluster.py run -d FFmpeg-AV1-VAAPI -ts $ts -j 1 -t 120 -k -f csv -so /tmp/av1/fluster-$ts.csv 2>&1 | tail -4
done
echo "=== done $(date -Is)"
