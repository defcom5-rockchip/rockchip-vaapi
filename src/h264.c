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

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int l0_default_minus1, int l1_default_minus1)
{
    uint8_t raw[256];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=8 */
    bs_write(&bs, 0x68, 8);

    bs_write_ue(&bs, 0); /* pic_parameter_set_id */
    bs_write_ue(&bs, 0); /* seq_parameter_set_id */
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_mode_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.pic_order_present_flag, 1);
    bs_write_ue(&bs, 0); /* num_slice_groups_minus1 = 0 */

    /* num_ref_idx_lX_default_active_minus1.  These were hardcoded to 0 (one
       reference per list), which is silently wrong for any encoder whose PPS says
       otherwise: a P/B slice without num_ref_idx_active_override_flag inherits the
       default, and the encoder starts relying on the default exactly when its DPB
       fills -- so the picture is bit-exact for the first dozen frames and drifts
       from then on (KI-1's "ghosting").  Proven by hybrid streams: original SPS +
       our PPS + original slices drifts in a software decoder too.  VA-API does
       not carry the defaults; the caller learns them from override-free slices
       (h264_peek_ref_override) and re-emits this PPS when they change. */
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

    /* More-data present if high-profile extensions needed */
    if (pp->pic_fields.bits.transform_8x8_mode_flag ||
        pp->second_chroma_qp_index_offset != pp->chroma_qp_index_offset) {
        bs_write(&bs, pp->pic_fields.bits.transform_8x8_mode_flag, 1);
        bs_write(&bs, 0, 1); /* pic_scaling_matrix_present_flag = 0 */
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

/* ===========================================================================
 * Phase 1.8 — CRA -> IDR slice rewrite (KI-1: H.264 open-GOP seeks)
 *
 * MPP will not restart hardware parsing after any discontinuity until it sees a
 * strict IDR, and an open-GOP seek lands on a recovery-point I picture that is
 * NOT an IDR (nal_unit_type 1).  Flushing MPP therefore produces nothing (it
 * waits for an IDR that never comes) and not flushing decodes the new GOP
 * against reference slots left over from before the seek.  Eight remedies on
 * the output side failed; the fix has to be in the bitstream.
 *
 * For an I slice the IDR and non-IDR headers differ in exactly two places:
 *   - idr_pic_id ue(v) is present, right after frame_num / field flags;
 *   - dec_ref_pic_marking() takes the IDR form (no_output_of_prior_pics_flag,
 *     long_term_reference_flag) instead of adaptive_ref_pic_marking_mode_flag
 *     and its MMCO list.
 * Everything else in the header is identical, and the slice data is untouched.
 * Because the header changes length, CABAC slice data has to be re-aligned to
 * a byte boundary (cabac_alignment_one_bit); CAVLC data is bit-copied as-is.
 * The rewrite works on the unescaped RBSP and re-escapes on the way out.
 *
 * A small bit reader lives here rather than being shared with hevc.c so the
 * proven HEVC path is not touched by this phase.
 * ======================================================================== */
#include <stdlib.h>

typedef struct { const uint8_t *p; size_t n; size_t pos; } H264BR;   /* pos in bits */

static uint32_t h264_br_u(H264BR *b, int k)
{
    uint32_t v = 0;
    for (int i = 0; i < k; i++) {
        size_t byte = b->pos >> 3;
        int bit = (byte < b->n) ? ((b->p[byte] >> (7 - (b->pos & 7))) & 1) : 0;
        v = (v << 1) | (uint32_t)bit;
        b->pos++;
    }
    return v;
}
static uint32_t h264_br_ue(H264BR *b)
{
    int lz = 0;
    while (lz < 32 && h264_br_u(b, 1) == 0) lz++;
    if (lz == 0) return 0;
    return ((1u << lz) - 1) + h264_br_u(b, lz);
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
        out[o++] = c;
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return o;
}
static void h264_copy_bits(BSWriter *w, H264BR *r, size_t nbits)
{
    while (nbits >= 8) { bs_write(w, h264_br_u(r, 8), 8); nbits -= 8; }
    if (nbits) bs_write(w, h264_br_u(r, (int)nbits), (int)nbits);
}
static inline size_t h264_bs_bitpos(const BSWriter *w) { return w->byte_pos * 8 + (size_t)w->bit_pos; }

/* slice_type of a slice NAL (escaped, starting at the NAL header), or -1. */
int h264_peek_slice_type(const uint8_t *nal, size_t len)
{
    if (!nal || len < 2) return -1;
    uint8_t tmp[32];
    size_t n = h264_rbsp_unescape(nal + 1, len - 1 < sizeof tmp ? len - 1 : sizeof tmp, tmp, sizeof tmp);
    H264BR r = { tmp, n, 0 };
    (void)h264_br_ue(&r);                 /* first_mb_in_slice */
    return (int)h264_br_ue(&r);           /* slice_type */
}

int h264_rewrite_idr(const uint8_t *in, size_t in_len,
                     const VAPictureParameterBufferH264 *pp,
                     uint32_t idr_pic_id, int keep_frame_num,
                     uint8_t *out, size_t out_cap)
{
    if (!in || in_len < 2 || !pp || !out || out_cap < 8) return -1;
    const uint8_t nal_hdr  = in[0];
    const int nal_ref_idc  = (nal_hdr >> 5) & 3;
    const int nal_type     = nal_hdr & 0x1F;
    if (nal_type != 1) return -2;                          /* only non-IDR slices */

    size_t cap = in_len + 16;
    uint8_t *rbsp = malloc(cap), *tmp = malloc(cap + 64);
    if (!rbsp || !tmp) { free(rbsp); free(tmp); return -3; }
    size_t rn = h264_rbsp_unescape(in + 1, in_len - 1, rbsp, cap);
    H264BR r = { rbsp, rn, 0 };

    /* --- header, up to the point where IDR and non-IDR diverge --- */
    uint32_t first_mb   = h264_br_ue(&r);
    uint32_t slice_type = h264_br_ue(&r);
    if ((slice_type % 5) != 2) { free(rbsp); free(tmp); return -4; }   /* I slices only */
    uint32_t pps_id     = h264_br_ue(&r);
    /* colour_plane_id would follow for separate_colour_plane_flag; VA-API does
       not carry it and 4:4:4 separate-plane content is not advertised. */
    const int log2_mfn  = pp->seq_fields.bits.log2_max_frame_num_minus4 + 4;
    uint32_t frame_num  = h264_br_u(&r, log2_mfn);
    int field_pic = 0, bottom_field = 0;
    if (!pp->seq_fields.bits.frame_mbs_only_flag) {
        field_pic = (int)h264_br_u(&r, 1);
        if (field_pic) bottom_field = (int)h264_br_u(&r, 1);
    }
    /* [IDR would carry idr_pic_id here] */
    size_t poc_start = r.pos;
    const int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    if (poc_type == 0) {
        (void)h264_br_u(&r, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pp->pic_fields.bits.pic_order_present_flag && !field_pic) (void)h264_br_se(&r);
    } else if (poc_type == 1 && !pp->seq_fields.bits.delta_pic_order_always_zero_flag) {
        (void)h264_br_se(&r);
        if (pp->pic_fields.bits.pic_order_present_flag && !field_pic) (void)h264_br_se(&r);
    }
    if (pp->pic_fields.bits.redundant_pic_cnt_present_flag) (void)h264_br_ue(&r);
    size_t poc_end = r.pos;
    /* I slice: no direct_spatial_mv_pred_flag, no num_ref_idx override, no
       ref_pic_list_modification(), no pred_weight_table(). */
    if (nal_ref_idc != 0) {                                 /* dec_ref_pic_marking(), non-IDR form */
        if (h264_br_u(&r, 1)) {                             /* adaptive_ref_pic_marking_mode_flag */
            for (int guard = 0; guard < 64; guard++) {
                uint32_t mmco = h264_br_ue(&r);
                if (mmco == 0) break;
                if (mmco == 1 || mmco == 3) (void)h264_br_ue(&r);   /* difference_of_pic_nums_minus1 */
                if (mmco == 2)              (void)h264_br_ue(&r);   /* long_term_pic_num */
                if (mmco == 3 || mmco == 6) (void)h264_br_ue(&r);   /* long_term_frame_idx */
                if (mmco == 4)              (void)h264_br_ue(&r);   /* max_long_term_frame_idx_plus1 */
            }
        }
    }
    size_t after_marking = r.pos;
    /* remaining header (identical for both forms): no cabac_init_idc for I;
       slice_qp_delta; deblocking; slice groups are assumed off (High profile). */
    (void)h264_br_se(&r);                                   /* slice_qp_delta */
    if (pp->pic_fields.bits.deblocking_filter_control_present_flag) {
        uint32_t idc = h264_br_ue(&r);                      /* disable_deblocking_filter_idc */
        if (idc != 1) { (void)h264_br_se(&r); (void)h264_br_se(&r); }
    }
    size_t header_end = r.pos;
    if (header_end > rn * 8) { free(rbsp); free(tmp); return -5; }   /* ran off the slice */

    /* --- emit --- */
    BSWriter w; bs_init(&w, tmp, cap + 64);
    bs_write_ue(&w, first_mb);
    bs_write_ue(&w, slice_type);
    bs_write_ue(&w, pps_id);
    bs_write(&w, keep_frame_num ? frame_num : 0, log2_mfn);
    if (!pp->seq_fields.bits.frame_mbs_only_flag) {
        bs_write1(&w, field_pic);
        if (field_pic) bs_write1(&w, bottom_field);
    }
    bs_write_ue(&w, idr_pic_id);                            /* the IDR insertion */
    { H264BR c1 = { rbsp, rn, poc_start }; h264_copy_bits(&w, &c1, poc_end - poc_start); }
    bs_write1(&w, 0);                                       /* no_output_of_prior_pics_flag */
    bs_write1(&w, 0);                                       /* long_term_reference_flag */
    { H264BR c2 = { rbsp, rn, after_marking }; h264_copy_bits(&w, &c2, header_end - after_marking); }
    if (pp->pic_fields.bits.entropy_coding_mode_flag) {
        while (h264_bs_bitpos(&w) & 7) bs_write1(&w, 1);    /* cabac_alignment_one_bit */
        size_t data_byte = (header_end + 7) >> 3;           /* original data started on this byte */
        for (size_t i = data_byte; i < rn; i++) bs_write(&w, rbsp[i], 8);
    } else {
        H264BR c3 = { rbsp, rn, header_end };
        h264_copy_bits(&w, &c3, rn * 8 - header_end);       /* data + original trailing bits */
        while (h264_bs_bitpos(&w) & 7) bs_write1(&w, 0);
    }
    size_t rbsp_out = bs_bytes(&w);
    /* new NAL header: IDR is always a reference picture */
    out[0] = (uint8_t)(((nal_ref_idc ? nal_ref_idc : 3) << 5) | 5);
    size_t esc = emulation_prevent(tmp, rbsp_out, out + 1, out_cap - 1);
    free(rbsp); free(tmp);
    if (esc == 0) return -6;
    return (int)(esc + 1);
}

/* Same-width in-place rewrite of frame_num.  After a synthetic IDR (frame_num
 * forced to 0, as the spec requires) every later picture until the next real
 * IDR is renumbered by the same offset so MPP sees a conformant closed GOP:
 * 0, 1, 2, ...  Relative PicNum differences -- MMCO, ref_pic_list_modification
 * -- are preserved, POC is untouched.  Field width does not change, so only
 * the escaping is redone. */
int h264_renumber_frame_num(const uint8_t *in, size_t in_len,
                            const VAPictureParameterBufferH264 *pp,
                            uint32_t offset, uint8_t *out, size_t out_cap)
{
    if (!in || in_len < 2 || !pp || !out || out_cap < 8) return -1;
    const int nal_type = in[0] & 0x1F;
    if (nal_type != 1 && nal_type != 5) return -2;
    size_t cap = in_len + 16;
    uint8_t *rbsp = malloc(cap);
    if (!rbsp) return -3;
    size_t rn = h264_rbsp_unescape(in + 1, in_len - 1, rbsp, cap);
    H264BR r = { rbsp, rn, 0 };
    (void)h264_br_ue(&r); (void)h264_br_ue(&r); (void)h264_br_ue(&r);   /* first_mb, slice_type, pps_id */
    const int log2_mfn = pp->seq_fields.bits.log2_max_frame_num_minus4 + 4;
    const uint32_t mask = (1u << log2_mfn) - 1;
    size_t pos = r.pos;
    uint32_t fn = h264_br_u(&r, log2_mfn);
    uint32_t nfn = (fn - offset) & mask;
    for (int i = 0; i < log2_mfn; i++) {
        size_t bp = pos + (size_t)i; size_t byte = bp >> 3; int sh = 7 - (bp & 7);
        if (byte >= rn) { free(rbsp); return -5; }
        rbsp[byte] = (uint8_t)((rbsp[byte] & ~(1u << sh)) | (((nfn >> (log2_mfn - 1 - i)) & 1u) << sh));
    }
    out[0] = in[0];
    size_t esc = emulation_prevent(rbsp, rn, out + 1, out_cap - 1);
    free(rbsp);
    return esc ? (int)(esc + 1) : -6;
}

/* num_ref_idx_active_override_flag of a P/SP/B slice (escaped NAL), with the
 * slice_type%5 in *st5.  Returns the flag (0/1), or -1 for I/SI or unparsable.
 * Why it matters: when the flag is 0 the slice inherits the PPS's
 * num_ref_idx_lX_default_active, which VA-API does not carry -- but ffmpeg's
 * VASliceParameterBufferH264 num_ref_idx_lX_active_minus1 is then exactly that
 * default, so such a slice teaches us the encoder's real PPS value. */
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
    if (st != 0 && st != 1 && st != 3) return -1;           /* only P (0), B (1), SP (3) carry it */
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
