/*
 * h264.c — H.264 Annex B SPS/PPS reconstruction from VA-API structs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "h264.h"
#include "bs.h"
#include <string.h>
#include <stdint.h>

/* Insert emulation prevention bytes (0x03) to avoid 0x000001/0x000002 sequences */
size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                                uint8_t *out, size_t out_cap) {
    size_t out_pos = 0;
    int zeros = 0;
    for (size_t i = 0; i < in_sz; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (out_pos >= out_cap) return 0;
            out[out_pos++] = 0x03;
            zeros = 0;
        }
        if (out_pos >= out_cap) return 0;
        out[out_pos++] = in[i];
        zeros = (in[i] == 0) ? zeros + 1 : 0;
    }
    return out_pos;
}

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=7 */
    bs_write(&bs, 0x67, 8);

    bs_write(&bs, (uint32_t)profile_idc, 8);

    /* constraint_set flags (derive from profile) + reserved.
     * Must be exactly 8 bits: constraint_set0..5 (6) + reserved_zero_2bits (2). */
    int c0 = (profile_idc == 66) ? 1 : 0;
    int c1 = (profile_idc == 66 || profile_idc == 77) ? 1 : 0;
    bs_write(&bs, c0, 1);
    bs_write(&bs, c1, 1);
    bs_write(&bs, 0, 1); /* constraint_set2 */
    bs_write(&bs, 0, 5); /* constraint_set3..5 (3) + reserved_zero_2bits (2) */

    bs_write(&bs, 51, 8); /* level_idc = 5.1 (safe for all content) */

    bs_write_ue(&bs, 0); /* seq_parameter_set_id */

    /* High-profile family gets chroma/bit-depth fields */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128) {
        int cfi = pp->seq_fields.bits.chroma_format_idc;
        bs_write_ue(&bs, (uint32_t)cfi);
        if (cfi == 3)
            bs_write(&bs, pp->seq_fields.bits.residual_colour_transform_flag, 1);
        bs_write_ue(&bs, pp->bit_depth_luma_minus8);
        bs_write_ue(&bs, pp->bit_depth_chroma_minus8);
        bs_write(&bs, 0, 1); /* qpprime_y_zero_transform_bypass_flag */
        bs_write(&bs, 0, 1); /* seq_scaling_matrix_present_flag = 0 */
    }

    bs_write_ue(&bs, pp->seq_fields.bits.log2_max_frame_num_minus4);

    int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    bs_write_ue(&bs, (uint32_t)poc_type);
    if (poc_type == 0) {
        bs_write_ue(&bs, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (poc_type == 1) {
        bs_write(&bs, pp->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
        bs_write_se(&bs, 0); /* offset_for_non_ref_pic */
        bs_write_se(&bs, 0); /* offset_for_top_to_bottom_field */
        bs_write_ue(&bs, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }

    bs_write_ue(&bs, pp->num_ref_frames);
    bs_write(&bs, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);
    bs_write_ue(&bs, pp->picture_width_in_mbs_minus1);

    int fmo = pp->seq_fields.bits.frame_mbs_only_flag;
    uint32_t ph_units = fmo ? (uint32_t)pp->picture_height_in_mbs_minus1
                            : ((uint32_t)pp->picture_height_in_mbs_minus1 + 1) / 2 - 1;
    bs_write_ue(&bs, ph_units);
    bs_write(&bs, fmo, 1);
    if (!fmo)
        bs_write(&bs, pp->seq_fields.bits.mb_adaptive_frame_field_flag, 1);

    bs_write(&bs, pp->seq_fields.bits.direct_8x8_inference_flag, 1);
    bs_write(&bs, 0, 1); /* frame_cropping_flag = 0 */
    bs_write(&bs, 0, 1); /* vui_parameters_present_flag = 0 */

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}

/* Zig-zag scan: entry k is the raster index of the k-th coefficient in the coded order (7.3.2.1.1.1 writes
   the lists in zig-zag frame order; VA-API hands them over in raster order). */
static const uint8_t zz4x4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t zz8x8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

int h264_iq_is_flat(const VAIQMatrixBufferH264 *iq)
{
    if (!iq) return 1;
    for (int i = 0; i < 6; i++) for (int j = 0; j < 16; j++) if (iq->ScalingList4x4[i][j] != 16) return 0;
    for (int i = 0; i < 2; i++) for (int j = 0; j < 64; j++) if (iq->ScalingList8x8[i][j] != 16) return 0;
    return 1;
}

/* scaling_list() of 7.3.2.1.1.1 for one list given in raster order: delta_scale per coefficient in zig-zag order.
   Every entry is 1..255, so nextScale is never 0 and useDefaultScalingMatrixFlag never triggers. */
static void write_scaling_list(BSWriter *bs, const uint8_t *raster, const uint8_t *zz, int n)
{
    int last = 8;
    for (int k = 0; k < n; k++) {
        int v = raster[zz[k]];
        int delta = ((v - last + 128) & 255) - 128;      /* wrapped to -128..127; (last + delta + 256) % 256 == v */
        bs_write_se(bs, delta);
        last = v;
    }
}

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp, int pps_id,
                   const VAIQMatrixBufferH264 *iq,
                   int l0_default_minus1, int l1_default_minus1)
{
    /* Eight explicit scaling lists are up to 224 delta_scale values of up to 17 bits each (a conformance stream
       alternates 255 and 1): about 440 bytes before emulation prevention, and the size check below reserves twice
       that, so the buffers are sized well above. */
    uint8_t raw[1024];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=8 */
    bs_write(&bs, 0x68, 8);

    bs_write_ue(&bs, (uint32_t)(pps_id < 0 ? 0 : pps_id)); /* pic_parameter_set_id: the one the slices name */
    bs_write_ue(&bs, 0); /* seq_parameter_set_id */
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_mode_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.pic_order_present_flag, 1);
    bs_write_ue(&bs, 0); /* num_slice_groups_minus1 = 0 */

    /* num_ref_idx_lX_default_active_minus1 -- NOT a "conservative default".  A
       P/B slice without num_ref_idx_active_override_flag inherits these, and
       encoders rely on them exactly once their DPB is full, so writing 0 here
       made every stream whose PPS said otherwise (most broadcast H.264, any
       x264 --ref >= 2) drift from the frame the DPB fills.  Proven with hybrid
       streams: original SPS + this PPS + the original slices drifts in a
       software decoder too.  VA-API does not carry the values; the caller
       learns them from override-free slices (h264_peek_ref_override) and
       re-emits this PPS when they change. */
    bs_write_ue(&bs, (uint32_t)(l0_default_minus1 < 0 ? 0 : l0_default_minus1));
    bs_write_ue(&bs, (uint32_t)(l1_default_minus1 < 0 ? 0 : l1_default_minus1));

    bs_write(&bs, pp->pic_fields.bits.weighted_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_bipred_idc, 2);
    bs_write_se(&bs, pp->pic_init_qp_minus26);
    bs_write_se(&bs, pp->pic_init_qs_minus26);
    bs_write_se(&bs, pp->chroma_qp_index_offset);
    bs_write(&bs, pp->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.redundant_pic_cnt_present_flag, 1);

    /* More-data present if high-profile extensions needed.  The scaling lists VA-API carries are the ones in
       effect for this picture (ffmpeg resolves the SPS/PPS fall-back rules before it fills the buffer), so they
       are written here as explicit PPS lists whenever they are not flat; a flat set is what "no matrix" means. */
    const int t8x8 = pp->pic_fields.bits.transform_8x8_mode_flag;
    /* A 0 entry would end the list early in the decoder (nextScale == 0 means "use the default / hold the last") while
       we keep writing deltas, and every field after it would be misread; no client sends one, but a zeroed buffer must
       not desynchronise the PPS: such a buffer is treated as "no usable matrix". */
    int usable = iq && !h264_iq_is_flat(iq);
    if (usable) {
        for (int i = 0; i < 6 && usable; i++) for (int j = 0; j < 16; j++) if (!iq->ScalingList4x4[i][j]) { usable = 0; break; }
        for (int i = 0; i < 2 && usable; i++) for (int j = 0; j < 64; j++) if (!iq->ScalingList8x8[i][j]) { usable = 0; break; }
    }
    const int have_lists = usable;
    if (t8x8 || have_lists ||
        pp->second_chroma_qp_index_offset != pp->chroma_qp_index_offset) {
        bs_write(&bs, t8x8, 1);
        bs_write(&bs, have_lists, 1); /* pic_scaling_matrix_present_flag */
        if (have_lists) {
            const int cfi = pp->seq_fields.bits.chroma_format_idc;
            const int n8 = t8x8 ? ((cfi != 3) ? 2 : 6) : 0;
            for (int i = 0; i < 6; i++) {
                bs_write(&bs, 1, 1); /* pic_scaling_list_present_flag[i] */
                write_scaling_list(&bs, iq->ScalingList4x4[i], zz4x4, 16);
            }
            for (int i = 0; i < n8; i++) {
                if (i < 2) {        /* Intra Y, Inter Y: the two lists VA-API carries */
                    bs_write(&bs, 1, 1);
                    write_scaling_list(&bs, iq->ScalingList8x8[i], zz8x8, 64);
                } else {
                    bs_write(&bs, 0, 1); /* 4:4:4 chroma 8x8 lists: not carried; fall-back rule */
                }
            }
        }
        bs_write_se(&bs, pp->second_chroma_qp_index_offset);
    }

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}


/* ---- small RBSP bit reader, local to the H.264 path (hevc.c keeps its own) ---- */
#include <stdlib.h>
typedef struct { const uint8_t *p; size_t n; size_t pos; } H264BR;   /* pos in bits */
static uint32_t h264_br_u(H264BR *b, int k)
{
    uint32_t v = 0;
    for (int i = 0; i < k; i++) {
        size_t byte = b->pos >> 3;
        int bit = (byte < b->n) ? ((b->p[byte] >> (7 - (b->pos & 7))) & 1) : 0;
        v = (v << 1) | (uint32_t)bit; b->pos++;
    }
    return v;
}
static uint32_t h264_br_ue(H264BR *b)
{
    int lz = 0;
    while (lz < 32 && h264_br_u(b, 1) == 0) lz++;
    if (lz >= 32) return 0;                                 /* all-zero payload: no code word */
    return lz == 0 ? 0 : ((1u << lz) - 1) + h264_br_u(b, lz);
}
static int32_t h264_br_se(H264BR *b)
{
    uint32_t k = h264_br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}
static size_t h264_rbsp_unescape(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0; int zeros = 0;
    for (size_t i = 0; i < n && o < cap; i++) {
        uint8_t c = in[i];
        if (zeros >= 2 && c == 0x03) { zeros = 0; continue; }
        out[o++] = c; zeros = (c == 0) ? zeros + 1 : 0;
    }
    return o;
}

/* num_ref_idx_active_override_flag of a P/SP/B slice (escaped NAL), slice_type%5
 * in *st5.  Returns the flag, or -1 for I/SI or unparsable.
 *
 * Why: when the flag is 0 the slice inherits the PPS's
 * num_ref_idx_lX_default_active, which VA-API does not carry -- but ffmpeg's
 * VASliceParameterBufferH264 num_ref_idx_lX_active_minus1 is then exactly that
 * default, so an override-free slice teaches us the encoder's real PPS value.
 * Encoders override while their DPB is filling and use the default once it is
 * full, which is why a wrong default shows up as a picture that is bit-exact for
 * the first dozen frames and drifts from then on (the old KI-1 "ghosting"). */
int h264_peek_ref_override(const uint8_t *nal, size_t len,
                           const VAPictureParameterBufferH264 *pp, int *st5)
{
    if (!nal || len < 2 || !pp) return -1;
    const int nal_type = nal[0] & 0x1F;
    if (nal_type != 1 && nal_type != 5) return -1;
    uint8_t tmp[96];
    size_t n = h264_rbsp_unescape(nal + 1, len - 1 < sizeof tmp ? len - 1 : sizeof tmp, tmp, sizeof tmp);
    H264BR r = { tmp, n, 0 };
    (void)h264_br_ue(&r);                                   /* first_mb_in_slice */
    uint32_t st = h264_br_ue(&r) % 5;
    if (st5) *st5 = (int)st;
    if (st != 0 && st != 1 && st != 3) return -1;           /* only P, B, SP carry it */
    (void)h264_br_ue(&r);                                   /* pps_id */
    (void)h264_br_u(&r, pp->seq_fields.bits.log2_max_frame_num_minus4 + 4);
    int field_pic = 0;
    if (!pp->seq_fields.bits.frame_mbs_only_flag) {
        field_pic = (int)h264_br_u(&r, 1);
        if (field_pic) (void)h264_br_u(&r, 1);
    }
    if (nal_type == 5) (void)h264_br_ue(&r);                /* idr_pic_id */
    const int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    if (poc_type == 0) {
        (void)h264_br_u(&r, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pp->pic_fields.bits.pic_order_present_flag && !field_pic) (void)h264_br_se(&r);
    } else if (poc_type == 1 && !pp->seq_fields.bits.delta_pic_order_always_zero_flag) {
        (void)h264_br_se(&r);
        if (pp->pic_fields.bits.pic_order_present_flag && !field_pic) (void)h264_br_se(&r);
    }
    if (pp->pic_fields.bits.redundant_pic_cnt_present_flag) (void)h264_br_ue(&r);
    if (st == 1) (void)h264_br_u(&r, 1);                    /* direct_spatial_mv_pred_flag */
    return (int)h264_br_u(&r, 1);                           /* num_ref_idx_active_override_flag */
}

/* pic_parameter_set_id of a slice NAL: the third ue(v) of the slice header. */
int h264_peek_pps_id(const uint8_t *nal, size_t len)
{
    if (!nal || len < 2) return -1;
    const int nal_type = nal[0] & 0x1F;
    if (nal_type != 1 && nal_type != 5) return -1;
    uint8_t tmp[32];
    size_t n = h264_rbsp_unescape(nal + 1, len - 1 < sizeof tmp ? len - 1 : sizeof tmp, tmp, sizeof tmp);
    H264BR r = { tmp, n, 0 };
    (void)h264_br_ue(&r);                                   /* first_mb_in_slice */
    (void)h264_br_ue(&r);                                   /* slice_type */
    uint32_t id = h264_br_ue(&r);
    return id > 255 ? -1 : (int)id;
}
