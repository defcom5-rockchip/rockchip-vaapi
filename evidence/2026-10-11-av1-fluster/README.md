# AV1 conformance, 2026-10-11 — rockchip-vaapi 3.0.0 candidate on an Orange Pi 5B, kernel 6.1.189 vendor

Fluster (fluendo, 7e4d85b) with its `FFmpeg-AV1-VAAPI` decoder (`ffmpeg -hwaccel vaapi`, md5 of the whole output),
driver built from the `av1` branch, `LIBVA_DRIVER_NAME=rockchip`.

| suite | result |
|---|---|
| AV1-TEST-VECTORS (libaom) | 213 / 242 |
| CHROMIUM-10bit-AV1-TEST-VECTORS | 22 / 23 |
| CHROMIUM-8bit-AV1-TEST-VECTORS | 9 / 13 |

Files: `fluster-*.csv` (per vector), `fluster6.log` (the run), `attrib3.out` (every failed vector decoded three ways:
dav1d, the driver, and the raw MPP decoder fed the original stream — `tests/av1/attrib.sh`; columns are frames
produced and frames differing from dav1d). Attribution of the remaining failures is in KNOWN-ISSUES KI-13:
frames <= 66 px and spatial layers fail on the raw decoder too; film grain is not applied by design; monochrome never
reaches the driver (ffmpeg's native decoder); sizeup/sizedown change the frame size inside one VA context; 48delayed
differs only in its last 30 frames, where raw MPP diverges as well.

Also measured the same day (not in these files): a 7-clip regression set (Tears of Steel 480p/1080p, 8/10-bit,
multi-tile, segmentation, film grain) bit-exact against dav1d on every run; mpv `--hwdec=vaapi`, Chrome and Firefox
(with the Mozilla-submitted luma-copy patch) decoding 8- and 10-bit AV1 on the av1d engine with the picture checked by eye.
