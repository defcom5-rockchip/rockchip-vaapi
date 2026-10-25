# HEVC conformance after the v4.0.0 HEVC changes — 2026-10-25

Fluster (fluendo/fluster commit 54662f8, 2026-10-23), decoder `FFmpeg-H.265-VAAPI` (ffmpeg 8.0.1-3ubuntu2), Orange Pi 5B
(RK3588S), Rockchip vendor kernel 6.1.189, MPP 1.5.0+git20260805.a8b19653 `+pd35`, driver built from the tree of the two
HEVC commits ("HEVC: rewrite slice headers with inline reference picture sets" and "HEVC: PPS per picture with the stream's
scaling lists"). Baseline: the same suites with driver 3.5.0 and MPP `+pd34`
on 2026-10-23.

| suite | 3.5.0 | this tree | regressions |
|---|---|---|---|
| JCT-VC-HEVC_V1 | 17/147 | 141/147 | 0 |
| JCT-VC-RExt | 2/49 | 2/49 | 0 |

Decoder resets: none during the final run (`kernel-log-hevc-final-run.txt` holds the kernel lines of the run window, 0 "reset
done"); the run with the reference-set change alone still had 52 "reset done" lines, 26 per core (178 lines in 3.5.0), from the six
PPS-class vectors that stalled until the PPS change. The log carries timestamps so the window can be matched to LOG.txt. Files: `fluster-JCT-VC-HEVC_V1.csv`, `fluster-JCT-VC-RExt.csv` (Fluster's `-f csv -so` output: vector,
result, time). The vendor string in the CSVs still says 3.5.0; the binary is the development tree. Hardware decode was confirmed
on spot checks by the decoder's interrupt counts, as for the H.264 evidence. The remaining
failures are listed in KNOWN-ISSUES KI-5. A first version of the rewrite ordered the long-term entries by POC and regressed
RPS_E_qualcomm_5 (255 of 300 frames correct); the reference lists are built from the entry order, so the entries now keep the
original header's order. The reference-set change alone measured 128/147; the PPS change on top gives 141/147 (this folder). A two-pass x265 clip whose
SPS carries 16 reference sets, every slice indexing them, decodes bit-exact (120 of 120 frames) through the rewrite: `x265-spsrps-framemd5.txt` holds both per-frame MD5 lists
(the clip itself is not in the repository). For scale: ffmpeg's software decoder also scores 141/147 on this suite (its six:
NUT_A, RPS_D, SAODBLK_A/B, TSUNEQBD_A, VPSSPSPPS_A), so the hardware path now passes two vectors software fails (SAODBLK_A/B)
and fails two that software passes (PICSIZE_A/B). The kernel log of the run window holds no decoder lines; its `rga:` lines come from the 2D engine refusing
the 10-bit 416x240 vectors' stride (the driver then converts on the CPU), not from the decoder.
