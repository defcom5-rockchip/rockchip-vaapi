# Full conformance regression after the 4.0.0 fixes — 2026-10-25

Fluster, `ffmpeg -hwaccel vaapi` (ffmpeg 8.0.1), Orange Pi 5B (RK3588S), vendor kernel 6.1.189, MPP `+pd35`, driver built
from the 4.0.0 development tree after the H.264, HEVC, High 10 and AV1 size-change commits (board build `77807efc`, the MD5
of the `.so`). `LOG.txt` is the run script's summary. The H.264 and HEVC suites from the same build are in
`../2026-10-25-h264-fluster/item3-high10-initqp/` and `../2026-10-25-hevc-fluster/`.

| suite | 3.5.0 | this tree | per-vector changes |
|---|---|---|---|
| AV1-TEST-VECTORS (libaom) | 213/242 | 215/242 | `av1-1-b8-23-film_grain-50`, `av1-1-b10-23-film_grain-50` Fail → Success; nothing else |
| CHROMIUM-8bit-AV1 | 9/13 | 11/13 | `av1-1-b8-23-filmgrain-50`, `ccvbfilmgrain` Fail → Success |
| CHROMIUM-10bit-AV1 | 22/23 | 23/23 | `av1-1-b10-23-filmgrain-50` Fail → Success |
| VP9-TEST-VECTORS | 189/305 | 189/305 | none |
| VP9-TEST-VECTORS-HIGH | 3/6 | 3/6 | none |

The AV1 baselines are the 2026-10-11 run in `../2026-10-11-av1-fluster/` (driver 2.5.0/3.0.0 tree, MPP `+pd34`); the three
film-grain vectors pass now because MPP `+pd35` applies the grain (KI-13), not because of a driver change. The VP9
baselines were run today with the installed 3.5.0 package on the same board, same suites, directly after the new-driver run
(`fluster-VP9-TEST-VECTORS-3.5.0.csv`, `fluster-VP9-TEST-VECTORS-HIGH-3.5.0.csv`): the 116 + 3 failures are identical vector
by vector (tiny frames below 16 px, the `resize` family, 351×287-class sizes, `tile-4x4`, `show-existing-frame`). They are
the VP9 status quo, not touched by these changes, and are the next thing to look at.

Decoder resets: the kernel's `reset done` counter rose by 12 during the new-driver run and by 0 during the 3.5.0 VP9 run.
All 12 fall inside the libaom `av1-1-b8-01-size-*` vectors (16:44:48–16:45:02 of the run; one `size-16x64` run alone
produces two) — the tiny-frame class that fails on the raw decoder as well (KI-13). The size-change vectors produce none.
`sizedown` and `sizeup` still count as Fail under Fluster's command, which rescales frames after a size change before the
MD5; the per-frame results are in `../2026-10-25-av1-sizechange/`.

The vendor string in the CSVs says 3.5.0 for both runs; the new-driver run is the development tree (`LOG.txt` names the build).

Also here: `browsers.txt` (Firefox and Chromium on the board session with this build) and `mpv-4k60-ab.txt` (mpv 4K60 drop
counts, installed 3.5.0 vs this build: equal within noise on 8-bit H.264, 10-bit HEVC and 10-bit VP9).
