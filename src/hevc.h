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
int hevc_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp, int is_main10);
int hevc_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp);
