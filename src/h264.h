/*
 * h264.h — H.264 Annex B SPS/PPS reconstruction
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once
#include <va/va.h>
#include <stdint.h>
#include <stddef.h>

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc);

/* PPS for the picture: pps_id is the id the slices name; iq (may be NULL) is the client's VAIQMatrixBufferH264,
   written as explicit pic_scaling lists when it is not flat. */
/* init_qp_offset: subtracted from pp->pic_init_qp_minus26 and pic_init_qs_minus26 before writing: ffmpeg's VA-API
   client hands over the value plus QpBdOffsetY (6 * bit_depth_luma_minus8), the bitstream wants the coded value. */
int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp, int pps_id,
                   const VAIQMatrixBufferH264 *iq, int init_qp_offset,
                   int l0_default_minus1, int l1_default_minus1);
/* pic_parameter_set_id of a slice NAL (escaped), or -1. */
int h264_peek_pps_id(const uint8_t *nal, size_t len);
/* true when every scaling-list entry is 16 (Flat_4x4_16 / Flat_8x8_16: the same as no matrix). */
int h264_iq_is_flat(const VAIQMatrixBufferH264 *iq);
/* num_ref_idx_active_override_flag of a P/SP/B slice, or -1; slice_type%5 in *st5. */
int h264_peek_ref_override(const uint8_t *nal, size_t len,
                           const VAPictureParameterBufferH264 *pp, int *st5);

/* Shared with hevc.c: insert 0x03 emulation-prevention bytes. */
size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                         uint8_t *out, size_t out_cap);
