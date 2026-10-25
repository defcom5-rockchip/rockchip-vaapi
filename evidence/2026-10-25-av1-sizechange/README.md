# AV1 resolution change inside one VA context — 2026-10-25

libaom's `av1-1-b8-03-sizeup` (10 frames 352×288, then a key frame with a new sequence header and 10 frames 1280×720) and
`av1-1-b8-03-sizedown` (the reverse), on an Orange Pi 5B (RK3588S), vendor kernel 6.1.189, MPP `+pd35`, ffmpeg 8.0.1 with
libdav1d as the reference, driver built from the v4.0.0 development tree (commit "AV1: a picture larger than its surface").

Every comparison below is per frame at the frame's own size (`-noautoscale`; ffmpeg otherwise rescales frames after a size
change to the first frame's size, which made an earlier comparison report false mismatches).

| path | sizedown | sizeup |
|---|---|---|
| MPP alone (`mpi_dec_test`, develop build) | 20/20 bit-exact | 20/20 bit-exact |
| driver, ffmpeg download path (`-hwaccel vaapi`, `vaGetImage`) | 20/20 bit-exact | 10 small frames bit-exact; the 7 coded 1280×720 pictures refused (`resolution not supported`, counted with `-threads 1`); ffmpeg still emits 3 further frames (show-existing headers, not seen by the driver), which are wrong — 13 frames out; 10 of 10 runs of Fluster's command identical, no crash (3.5.0: 3 of 20 runs segfaulted, measured 2026-10-24, maintainer's notes) |
| driver, exporting client (mpv `hwdec=vaapi`, zero-copy, screenshots of the sampled frames) | frames 5/12/15/20: 45.9 / 42.4 / 42.4 / 42.4 dB | frames 5/12/15/20: 42.4 / 45.9 / 45.9 / 45.8 dB — the 1280×720 frames come through grown surfaces (8 grows, 0 refusals in the driver log) |

The mpv numbers are PSNR of a GPU screenshot (`screenshot-to-file … video`) against libdav1d's frame; the GPU conversion
leaves about 45.9 dB on a 1280×720 frame and 42.4 dB on a 352×288 one for frames known to be right (frame 5 of each
vector, before its size change), so equal numbers after the change mean the picture is right, not merely present.

Files: `compare-noautoscale.txt` (MPP-alone and download-path per-frame results), `mpv-psnr.txt` (the mpv run), `final-run.txt`
(the 10-run determinism check of Fluster's command and the per-frame counts on the final build), `driver-log-mpv-sizeup.txt` (the driver's log of the mpv run:
eight `surface_grow` lines, no refusal), `driver-log-ffmpeg-sizeup.txt` (the download-path run: one refusal line).
The vectors are libaom's test data (`storage.googleapis.com/aom-test-data/`), not kept here.
