/*
 * hevc.h — H.265/HEVC Annex B VPS/SPS/PPS reconstruction from VA-API structs
 *
 * Copyright (C) 2026 defcom5-rockchip
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once
#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <stdint.h>
#include <stddef.h>

/* Each writer emits: 4-byte Annex B start code + 2-byte NAL header + RBSP
 * (emulation-prevented).  Returns byte length, or -1 on overflow. */
int hevc_write_vps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp, int is_main10);
/* num_st_rps: 0 or 1.  Encoders whose ORIGINAL SPS carried >=1 short-term RPS
 * sets write every slice-inline RPS as st_ref_pic_set(idx!=0), which carries an
 * inter_ref_pic_set_prediction_flag bit; an SPS with 0 sets makes the decoder
 * skip that bit and misparse the rest of the slice header.  We therefore emit
 * either 0 sets or 1 dummy set to reproduce the original's bit layout. */
int hevc_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp, int is_main10,
                   int num_st_rps);

/* Parse the first slice segment header of a non-IDR picture and decide, by
 * matching the RPS bit length against pp->st_rps_bits, whether the original
 * SPS had 0 short-term RPS sets (returns 0) or >=1 (returns 1).
 * Returns -1 if undecidable (dependent/non-first segment, IDR, no match) and
 * -2 if the slice references SPS-level sets or uses inter-RPS prediction
 * (cannot be reconstructed from VA-API). */
int hevc_detect_num_st_rps(const uint8_t *nal, size_t len,
                           const VAPictureParameterBufferHEVC *pp);
int hevc_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp);
