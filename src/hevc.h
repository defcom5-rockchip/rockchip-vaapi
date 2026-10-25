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

/* Rewrite one slice segment NAL (escaped, no start code) so that its reference picture set is carried inline, built
   from pp->ReferenceFrames, instead of referring to the SPS-level short-term sets and long-term entries that VA-API
   does not carry; the SPS we send declares none (hevc_write_sps with num_st_rps = 0).  data_byte_offset is the
   client's slice_data_byte_offset (bytes into the unescaped NAL, as ffmpeg, Chromium and GStreamer all count it).
   Returns the length of the rewritten header written to out (escaped, no start code) and in *data_start the offset
   in nal of the slice data that follows it unchanged; 0 when the NAL needs no change (IDR, dependent segment); -1
   when it cannot be rewritten (the caller refuses the picture: the original slice against the sent SPS would decode
   with wrong references or stall). */
int hevc_rewrite_slice_rps(const uint8_t *nal, size_t len, const VAPictureParameterBufferHEVC *pp,
                           const uint8_t ref_list[2][15],
                           uint32_t data_byte_offset, uint8_t *out, size_t out_cap, size_t *data_start);

