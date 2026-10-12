# AV1 conformance, 2026-10-11 — rockchip-vaapi 3.0.0 candidate on an Orange Pi 5B, kernel 6.1.189 vendor

Fluster (fluendo, 7e4d85b) with its `FFmpeg-AV1-VAAPI` decoder (`ffmpeg -hwaccel vaapi`; a pass = the whole output
matches the suite's reference checksum), driver built from the `av1` branch, `LIBVA_DRIVER_NAME=rockchip`. The CSVs'
"Backend-VA-API" line still reads 2.5.0: the run was made from the branch before the version-string bump, with the
same decode code (the later commits changed only RKVA_HIDE_10BIT handling, a surface-attribute entry and the string).

| suite | result |
|---|---|
| AV1-TEST-VECTORS (libaom) | 213 / 242 |
| CHROMIUM-10bit-AV1-TEST-VECTORS | 22 / 23 |
| CHROMIUM-8bit-AV1-TEST-VECTORS | 9 / 13 |

Files: `fluster-*.csv` (per vector), `fluster6.log` (the run), `attrib.out` (every failed vector of these CSVs decoded
three ways with the release build: dav1d with film grain off, the driver, and the raw MPP decoder fed the original
stream — `tests/av1/attrib.sh`; columns are frames produced and frames differing from dav1d for the driver and for
raw MPP; monochrome vectors are skipped by the script, `sizedown` is an .mkv it does not pick up),
`48delayed-frames.txt` (the frame indices that differ for that vector: the driver in its last 30 frames, the raw
decoder there too and earlier). The attribution behind KNOWN-ISSUES KI-13.

Also measured the same day (not in these files): a 7-clip regression set (Tears of Steel 480p/1080p, 8/10-bit,
multi-tile, segmentation, film grain) matching dav1d on every run (the grain clip against dav1d with grain off);
decode on the av1d engine confirmed by interrupt counts in mpv, Chrome and Firefox (with the luma-copy patch for
10-bit), the picture checked by eye in Chrome and Firefox.
