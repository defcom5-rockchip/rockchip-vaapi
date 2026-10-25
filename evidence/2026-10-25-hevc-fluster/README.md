# HEVC conformance after the reference-set rewrite — 2026-10-25

Fluster (fluendo/fluster commit 54662f8, 2026-10-23), decoder `FFmpeg-H.265-VAAPI` (ffmpeg 8.0.1-3ubuntu2), Orange Pi 5B
(RK3588S), Rockchip vendor kernel 6.1.189, MPP 1.5.0+git20260805.a8b19653 `+pd35`, driver built from the tree of commit
"HEVC: rewrite slice headers with inline reference picture sets". Baseline: the same suites with driver 3.5.0 and MPP `+pd34`
on 2026-10-23.

| suite | 3.5.0 | this tree | regressions |
|---|---|---|---|
| JCT-VC-HEVC_V1 | 17/147 | 128/147 | 0 |
| JCT-VC-RExt | 2/49 | 2/49 | 0 |

Decoder resets: the kernel logged 52 "reset done" lines for the two rkvdec cores (26 reset events) during the final run, against
178 in the 3.5.0 run; they come from the six vectors that still stall (INITQP_A/B, PPS_A, SLIST_B/D, VPSSPSPPS_A: `Failed to sync
surface`, the PPS class). `kernel-log-hevc-final-run.txt` holds the kernel lines of the run window. Files: `fluster-JCT-VC-HEVC_V1.csv`, `fluster-JCT-VC-RExt.csv` (Fluster's `-f csv -so` output: vector,
result, time). The vendor string in the CSVs still says 3.5.0; the binary is the development tree. Hardware decode was confirmed
on spot checks by the decoder's interrupt counts, as for the H.264 evidence. The remaining
failures are listed in KNOWN-ISSUES KI-5. A first version of the rewrite ordered the long-term entries by POC and regressed
RPS_E_qualcomm_5 (255 of 300 frames correct); the reference lists are built from the entry order, so the entries now keep the
original header's order. The final run (this folder) is the tree after the code review: same 128/147, no vector changed.
