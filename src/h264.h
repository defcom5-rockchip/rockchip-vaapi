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

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int l0_default_minus1, int l1_default_minus1);
/* num_ref_idx_active_override_flag of a P/SP/B slice, or -1; slice_type%5 in *st5. */
int h264_peek_ref_override(const uint8_t *nal, size_t len,
                           const VAPictureParameterBufferH264 *pp, int *st5);

/* Shared with hevc.c: insert 0x03 emulation-prevention bytes. */
size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                         uint8_t *out, size_t out_cap);
