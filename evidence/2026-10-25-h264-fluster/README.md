# H.264 conformance after the 4.0.0 H.264 fixes — 2026-10-25

Fluster (fluendo/fluster, current main), decoder `FFmpeg-H.264-VAAPI` (ffmpeg 8.0.1), on an Orange Pi 5B (RK3588S),
Rockchip vendor kernel 6.1.189, MPP `+pd35`, driver built from the tree of commit "H.264: per-picture PPS with scaling
lists, field pictures, CAVLC multi-slice". Baseline: the same suites with driver 3.5.0 on 2026-10-23.

| suite | 3.5.0 | this tree | regressions |
|---|---|---|---|
| JVT-AVC_V1 | 74/135 | 111/135 | 0 |
| JVT-FR-EXT | 27/69 | 67/69 | 0 |

Files: `fluster-JVT-AVC_V1.csv`, `fluster-JVT-FR-EXT.csv` (Fluster's `-f csv -so` output: vector, result, time).
The remaining failures are listed in KNOWN-ISSUES KI-17, KI-20 and KI-21.

Method notes, for whoever repeats this: a hardware pass is one whose per-frame MD5 equals the libdav1d/ffmpeg software
reference; the hardware interrupt count was checked on spot tests to make sure the hardware decoded (no software fallback).
A fourth fix came from the review: monochrome (4:0:0) streams crashed the client because the driver read a chroma plane
MPP's luma-only frame does not have. The three main causes were isolated with small tools kept outside the repo: a program that feeds MPP directly (one packet per
picture, deinterlacer on or off, immediate output on or off), the driver's `RKVA_DUMP_H264=file` stream dump decoded in
software, and hybrid streams (our SPS/PPS with the original slices) which showed that the slice data, not the headers,
differed for the CAVLC multi-slice vectors.

## The 58 vectors whose names mark field, PAFF or MBAFF coding (the count in KI-20)

CABREF3_Sand_D CAFI1_SVA_C CAMA1_Sony_C CAMA1_TOSHIBA_B CAMA3_Sand_E CAMACI3_Sony_C CAMANL1_TOSHIBA_B CAMANL2_TOSHIBA_B
CAMANL3_Sand_E CAMASL3_Sony_B CAMP_MOT_MBAFF_L30 CAMP_MOT_MBAFF_L31 CANLMA2_Sony_C CANLMA3_Sony_C CAPA1_TOSHIBA_B
CAPAMA3_Sand_F CAPM3_Sony_D CVCANLMA2_Sony_C CVFI1_SVA_C CVFI1_Sony_D CVFI2_SVA_C CVFI2_Sony_H CVMA1_Sony_D CVMA1_TOSHIBA_B
CVMANL1_TOSHIBA_B CVMANL2_TOSHIBA_B CVMAPAQP3_Sony_E CVMAQP2_Sony_G CVMAQP3_Sony_D CVMP_MOT_FLD_L30_B CVNLFI1_Sony_C
CVNLFI2_Sony_H CVPA1_TOSHIBA_B FI1_Sony_E HCAFF1_HHI_B HCAMFF1_HHI_B HPCAFLNL_BRCM_C HPCAFL_BRCM_C HPCAMAPALQ_BRCM_B
HPCVFLNL_BRCM_A HPCVFL_BRCM_A HVLCFI0_Sony_B HVLCMFF0_Sony_B HVLCPFF0_Sony_B Sharp_MP_Field_1_B Sharp_MP_Field_2_B
Sharp_MP_Field_3_B Sharp_MP_PAFF_1r2 Sharp_MP_PAFF_2r cabac_mot_fld0_full cabac_mot_mbaff0_full cabac_mot_picaff0_full
cama1_vtc_c cama2_vtc_b cama3_vtc_b cavlc_mot_fld0_full_B cavlc_mot_mbaff0_full_B cavlc_mot_picaff0_full_B

52 pass with this tree, 31 of them for the first time (21 MBAFF frame vectors already passed with 3.5.0); the 6 that fail
are CVFI2_SVA_C and the five Sharp_MP_* vectors (picture order count type 1).
