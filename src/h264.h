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
                   const VAPictureParameterBufferH264 *pp);

/* Shared with hevc.c: insert 0x03 emulation-prevention bytes. */
size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                         uint8_t *out, size_t out_cap);
