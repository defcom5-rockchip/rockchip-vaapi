/*
 * hevc.c — H.265/HEVC Annex B VPS/SPS/PPS reconstruction from VA-API structs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 defcom5-rockchip
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
 * Why this file exists: VA-API hands a decoder the *parsed* parameter sets
 * (VAPictureParameterBufferHEVC) plus raw slice NALUs, never the VPS/SPS/PPS
 * bytes themselves.  Rockchip MPP is a bitstream decoder: it needs Annex B
 * parameter sets ahead of the slices or it decodes nothing (zero-filled
 * output — the solid green).  So we re-synthesise them.  The VA struct
 * mirrors SPS/PPS syntax almost 1:1; a minimal VPS is invented because
 * VA-API carries no VPS fields at all.
 *
 * Deliberate simplifications (documented, testable):
 *  - num_short_term_ref_pic_sets = 0: slices then carry their own RPS
 *    inline (short_term_ref_pic_set_sps_flag=0), which is what every
 *    encoder we have met emits (pp->st_rps_bits > 0 confirms per frame).
 *    A stream whose slices reference SPS-level RPS entries cannot be
 *    reconstructed from VA-API (the tables are not in the struct).
 *  - scaling lists: default tables only (sps/pps_scaling_list_data_present=0).
 *  - conformance_window_flag = 0: VA gives coded sizes; cropping is the
 *    client's problem, exactly as the H.264 path does.
 *  - no VUI, no extensions, single layer, single temporal sub-layer.
 */

#include "hevc.h"
#include "h264.h"   /* emulation_prevent() */
#include "bs.h"
#include <string.h>
#include <stdlib.h>

#define NAL_VPS 32
#define NAL_SPS 33
#define NAL_PPS 34

/* 2-byte HEVC NAL header: forbidden_zero(1) type(6) layer_id(6)=0 tid_plus1(3)=1 */
static void nal_header(BSWriter *bs, int type) {
    bs_write(bs, 0, 1);
    bs_write(bs, (uint32_t)type, 6);
    bs_write(bs, 0, 6);
    bs_write(bs, 1, 3);
}

/* profile_tier_level(profilePresent=1, maxNumSubLayersMinus1=0), 7.3.3 */
static void profile_tier_level(BSWriter *bs, int is_main10) {
    int idc = is_main10 ? 2 : 1;
    bs_write(bs, 0, 2);                 /* general_profile_space */
    bs_write(bs, 0, 1);                 /* general_tier_flag */
    bs_write(bs, (uint32_t)idc, 5);     /* general_profile_idc */
    for (int j = 0; j < 32; j++) {      /* general_profile_compatibility_flag[j] */
        int f = (j == idc) || (idc == 1 && j == 2);  /* Main is also Main10-compatible */
        bs_write1(bs, f);
    }
    bs_write(bs, 1, 1);                 /* general_progressive_source_flag */
    bs_write(bs, 0, 1);                 /* general_interlaced_source_flag */
    bs_write(bs, 1, 1);                 /* general_non_packed_constraint_flag */
    bs_write(bs, 1, 1);                 /* general_frame_only_constraint_flag */
    bs_write(bs, 0, 32);                /* general_reserved_zero_43bits (part 1) */
    bs_write(bs, 0, 11);                /* general_reserved_zero_43bits (part 2) */
    bs_write(bs, 0, 1);                 /* general_inbld_flag / reserved_zero_bit */
    bs_write(bs, 153, 8);               /* general_level_idc = 5.1 (generous; MPP is lenient) */
    /* maxNumSubLayersMinus1 == 0: no sub_layer flags, no alignment bits */
}

static int finish(BSWriter *bs, const uint8_t *raw, uint8_t *buf, size_t buf_size) {
    bs_rbsp_trailing(bs);
    size_t raw_sz = bs_bytes(bs);
    if (4 + raw_sz * 2 > buf_size) return -1;
    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}

int hevc_write_vps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp, int is_main10)
{
    uint8_t raw[256];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    nal_header(&bs, NAL_VPS);

    bs_write(&bs, 0, 4);        /* vps_video_parameter_set_id */
    bs_write(&bs, 1, 1);        /* vps_base_layer_internal_flag */
    bs_write(&bs, 1, 1);        /* vps_base_layer_available_flag */
    bs_write(&bs, 0, 6);        /* vps_max_layers_minus1 */
    bs_write(&bs, 0, 3);        /* vps_max_sub_layers_minus1 */
    bs_write(&bs, 1, 1);        /* vps_temporal_id_nesting_flag */
    bs_write(&bs, 0xFFFF, 16);  /* vps_reserved_0xffff_16bits */
    profile_tier_level(&bs, is_main10);
    bs_write(&bs, 1, 1);        /* vps_sub_layer_ordering_info_present_flag */
    bs_write_ue(&bs, pp->sps_max_dec_pic_buffering_minus1);  /* vps_max_dec_pic_buffering_minus1[0] */
    bs_write_ue(&bs, 0);        /* vps_max_num_reorder_pics[0] = 0: stateless client reorders; MPP must emit eagerly */
    bs_write_ue(&bs, 0);        /* vps_max_latency_increase_plus1[0] */
    bs_write(&bs, 0, 6);        /* vps_max_layer_id */
    bs_write_ue(&bs, 0);        /* vps_num_layer_sets_minus1 */
    bs_write(&bs, 0, 1);        /* vps_timing_info_present_flag */
    bs_write(&bs, 0, 1);        /* vps_extension_flag */
    return finish(&bs, raw, buf, buf_size);
}

int hevc_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp, int is_main10)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    nal_header(&bs, NAL_SPS);

    bs_write(&bs, 0, 4);        /* sps_video_parameter_set_id */
    bs_write(&bs, 0, 3);        /* sps_max_sub_layers_minus1 */
    bs_write(&bs, 1, 1);        /* sps_temporal_id_nesting_flag */
    profile_tier_level(&bs, is_main10);
    bs_write_ue(&bs, 0);        /* sps_seq_parameter_set_id */

    int cfi = pp->pic_fields.bits.chroma_format_idc;
    bs_write_ue(&bs, (uint32_t)cfi);
    if (cfi == 3)
        bs_write(&bs, pp->pic_fields.bits.separate_colour_plane_flag, 1);
    bs_write_ue(&bs, pp->pic_width_in_luma_samples);
    bs_write_ue(&bs, pp->pic_height_in_luma_samples);
    bs_write(&bs, 0, 1);        /* conformance_window_flag */
    bs_write_ue(&bs, pp->bit_depth_luma_minus8);
    bs_write_ue(&bs, pp->bit_depth_chroma_minus8);
    bs_write_ue(&bs, pp->log2_max_pic_order_cnt_lsb_minus4);

    bs_write(&bs, 1, 1);        /* sps_sub_layer_ordering_info_present_flag */
    bs_write_ue(&bs, pp->sps_max_dec_pic_buffering_minus1);  /* sps_max_dec_pic_buffering_minus1[0] */
    bs_write_ue(&bs, 0);        /* sps_max_num_reorder_pics[0] = 0: the VA-API client owns reordering.
                                   A non-zero value makes MPP hold decoded pictures for output
                                   ordering while the client already waits on vaSyncSurface for
                                   that exact picture -> deadline -> "internal decoding error". */
    bs_write_ue(&bs, 0);        /* sps_max_latency_increase_plus1[0] */

    bs_write_ue(&bs, pp->log2_min_luma_coding_block_size_minus3);
    bs_write_ue(&bs, pp->log2_diff_max_min_luma_coding_block_size);
    bs_write_ue(&bs, pp->log2_min_transform_block_size_minus2);
    bs_write_ue(&bs, pp->log2_diff_max_min_transform_block_size);
    bs_write_ue(&bs, pp->max_transform_hierarchy_depth_inter);
    bs_write_ue(&bs, pp->max_transform_hierarchy_depth_intra);

    bs_write(&bs, pp->pic_fields.bits.scaling_list_enabled_flag, 1);
    if (pp->pic_fields.bits.scaling_list_enabled_flag)
        bs_write(&bs, 0, 1);    /* sps_scaling_list_data_present_flag = 0 → default lists */

    bs_write(&bs, pp->pic_fields.bits.amp_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag, 1);

    bs_write(&bs, pp->pic_fields.bits.pcm_enabled_flag, 1);
    if (pp->pic_fields.bits.pcm_enabled_flag) {
        bs_write(&bs, pp->pcm_sample_bit_depth_luma_minus1, 4);
        bs_write(&bs, pp->pcm_sample_bit_depth_chroma_minus1, 4);
        bs_write_ue(&bs, pp->log2_min_pcm_luma_coding_block_size_minus3);
        bs_write_ue(&bs, pp->log2_diff_max_min_pcm_luma_coding_block_size);
        bs_write(&bs, pp->pic_fields.bits.pcm_loop_filter_disabled_flag, 1);
    }

    /* No SPS-level sets and no long-term entries: slice headers that referred to them are rewritten with inline
       sets (hevc_rewrite_slice_rps). */
    bs_write_ue(&bs, 0);        /* num_short_term_ref_pic_sets */
    bs_write(&bs, pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag, 1);
    if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        bs_write_ue(&bs, 0);    /* num_long_term_ref_pics_sps */
    bs_write(&bs, pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.strong_intra_smoothing_enabled_flag, 1);
    bs_write(&bs, 0, 1);        /* vui_parameters_present_flag */
    bs_write(&bs, 0, 1);        /* sps_extension_present_flag */
    return finish(&bs, raw, buf, buf_size);
}

int hevc_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferHEVC *pp)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    nal_header(&bs, NAL_PPS);

    bs_write_ue(&bs, 0);        /* pps_pic_parameter_set_id */
    bs_write_ue(&bs, 0);        /* pps_seq_parameter_set_id */
    bs_write(&bs, pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.output_flag_present_flag, 1);
    bs_write(&bs, pp->num_extra_slice_header_bits, 3);
    bs_write(&bs, pp->pic_fields.bits.sign_data_hiding_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.cabac_init_present_flag, 1);
    bs_write_ue(&bs, pp->num_ref_idx_l0_default_active_minus1);
    bs_write_ue(&bs, pp->num_ref_idx_l1_default_active_minus1);
    bs_write_se(&bs, pp->init_qp_minus26);
    bs_write(&bs, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.transform_skip_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.cu_qp_delta_enabled_flag, 1);
    if (pp->pic_fields.bits.cu_qp_delta_enabled_flag)
        bs_write_ue(&bs, pp->diff_cu_qp_delta_depth);
    bs_write_se(&bs, pp->pps_cb_qp_offset);
    bs_write_se(&bs, pp->pps_cr_qp_offset);
    bs_write(&bs, pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_bipred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.transquant_bypass_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.tiles_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_sync_enabled_flag, 1);
    if (pp->pic_fields.bits.tiles_enabled_flag) {
        int nc = pp->num_tile_columns_minus1, nr = pp->num_tile_rows_minus1;
        bs_write_ue(&bs, (uint32_t)nc);
        bs_write_ue(&bs, (uint32_t)nr);
        bs_write(&bs, 0, 1);    /* uniform_spacing_flag = 0: emit explicit sizes from pp */
        for (int i = 0; i < nc; i++) bs_write_ue(&bs, pp->column_width_minus1[i]);
        for (int i = 0; i < nr; i++) bs_write_ue(&bs, pp->row_height_minus1[i]);
        bs_write(&bs, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag, 1);
    }
    bs_write(&bs, pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag, 1);
    bs_write(&bs, 1, 1);        /* deblocking_filter_control_present_flag */
    bs_write(&bs, pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag, 1);
    if (!pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
        bs_write_se(&bs, pp->pps_beta_offset_div2);
        bs_write_se(&bs, pp->pps_tc_offset_div2);
    }
    bs_write(&bs, 0, 1);        /* pps_scaling_list_data_present_flag */
    bs_write(&bs, pp->slice_parsing_fields.bits.lists_modification_present_flag, 1);
    bs_write_ue(&bs, pp->log2_parallel_merge_level_minus2);
    bs_write(&bs, pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag, 1);
    bs_write(&bs, 0, 1);        /* pps_extension_present_flag */
    return finish(&bs, raw, buf, buf_size);
}


/* ------------------------------------------------------------------------ */
/* Small RBSP bit reader for the slice-header rewrite                          */

typedef struct { const uint8_t *p; size_t n; size_t pos; } BR;   /* pos in bits */
static uint32_t br_u(BR *b, int k) {
    uint32_t v = 0;
    while (k-- > 0) {
        size_t byte = b->pos >> 3; int bit = 7 - (int)(b->pos & 7);
        v = (v << 1) | (byte < b->n ? (uint32_t)((b->p[byte] >> bit) & 1) : 0u);
        b->pos++;
    }
    return v;
}
static uint32_t br_ue(BR *b) {
    int lz = 0;
    while (lz < 32 && br_u(b, 1) == 0) lz++;
    if (lz == 0) return 0;
    return ((1u << lz) - 1u) + br_u(b, lz);
}
/* Strip emulation-prevention bytes so the header parses as RBSP. */
static size_t rbsp_unescape(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0; int zeros = 0;
    for (size_t i = 0; i < n && o < cap; i++) {
        if (zeros >= 2 && in[i] == 0x03) { zeros = 0; continue; }
        out[o++] = in[i];
        zeros = (in[i] == 0) ? zeros + 1 : 0;
    }
    return o;
}

/* ------------------------------------------------------------------------ */
/* Slice-header rewrite: inline reference picture set (7.3.6.1, 7.3.7)        */

static int ceil_log2(uint32_t v) { int n = 0; while ((1u << n) < v) n++; return n; }

/* copy bits [from, to) of the reader's buffer into the writer */
static void copy_bits(BSWriter *w, const uint8_t *src, size_t from, size_t to)
{
    for (size_t pos = from; pos < to; pos++)
        bs_write1(w, (src[pos >> 3] >> (7 - (pos & 7))) & 1);
}

typedef struct { int poc; int used; } RpsPic;
static int cmp_desc(const void *a, const void *b) { return ((const RpsPic *)b)->poc - ((const RpsPic *)a)->poc; }
static int cmp_asc(const void *a, const void *b)  { return ((const RpsPic *)a)->poc - ((const RpsPic *)b)->poc; }

int hevc_rewrite_slice_rps(const uint8_t *nal, size_t len, const VAPictureParameterBufferHEVC *pp,
                           const uint8_t ref_list[2][15],
                           uint32_t data_byte_offset, uint8_t *out, size_t out_cap, size_t *data_start)
{
    if (!nal || len < 3 || !pp || !out || !data_start) return -1;
    int nal_type = (nal[0] >> 1) & 0x3F;
    if (nal_type > 21) return 0;                               /* not a slice */
    if (nal_type == 19 || nal_type == 20) return 0;            /* IDR: no RPS in the header */
    if (data_byte_offset < 3 || data_byte_offset > len) return -1;

    /* Unescape the header: data_byte_offset unescaped bytes (the client counts them that way), which also gives the
       escaped index of the first data byte.  The data itself is copied as the client gave it. */
    size_t hcap = (size_t)data_byte_offset + 8;
    uint8_t *hdr = malloc(hcap); size_t *esc_pos = malloc(hcap * sizeof *esc_pos);
    uint8_t *raw = NULL; int ret = -1;
    if (!hdr || !esc_pos) goto out;
    size_t n = 0, i = 0; int zeros = 0;
    for (; i < len && n < (size_t)data_byte_offset; i++) {
        if (zeros >= 2 && nal[i] == 0x03) { zeros = 0; continue; }
        esc_pos[n] = i; hdr[n++] = nal[i];
        zeros = (nal[i] == 0) ? zeros + 1 : 0;
    }
    if (n < (size_t)data_byte_offset) goto out;
    size_t data_esc = esc_pos[data_byte_offset - 1] + 1;       /* the header's last (alignment) byte is never zero
                                                                  (7.3.2.11), so no escape byte can follow it */
    BR b = { hdr, n, 16 };
    int first = (int)br_u(&b, 1);
    if (nal_type >= 16) br_u(&b, 1);                           /* no_output_of_prior_pics_flag */
    br_ue(&b);                                                 /* slice_pic_parameter_set_id */
    int dependent = 0;
    if (!first) {
        if (pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag) dependent = (int)br_u(&b, 1);
        int ctb_log2 = pp->log2_min_luma_coding_block_size_minus3 + 3 + pp->log2_diff_max_min_luma_coding_block_size;
        uint32_t ctb = 1u << ctb_log2;
        uint32_t wc = (pp->pic_width_in_luma_samples + ctb - 1) / ctb, hc = (pp->pic_height_in_luma_samples + ctb - 1) / ctb;
        br_u(&b, ceil_log2(wc * hc));                          /* slice_segment_address */
    }
    if (dependent) { ret = 0; goto out; }                      /* no RPS in a dependent segment */
    for (int k = 0; k < pp->num_extra_slice_header_bits; k++) br_u(&b, 1);
    br_ue(&b);                                                 /* slice_type */
    if (pp->slice_parsing_fields.bits.output_flag_present_flag) br_u(&b, 1);
    if (pp->pic_fields.bits.separate_colour_plane_flag) br_u(&b, 2);
    int poc_bits = pp->log2_max_pic_order_cnt_lsb_minus4 + 4;
    uint32_t max_lsb = 1u << poc_bits;
    br_u(&b, poc_bits);                                        /* slice_pic_order_cnt_lsb */
    size_t prefix_end = b.pos;                                 /* everything up to here is copied verbatim */

    /* skip the original short-term part */
    int sps_flag = (int)br_u(&b, 1);
    if (sps_flag) {
        if (pp->num_short_term_ref_pic_sets > 1) br_u(&b, ceil_log2(pp->num_short_term_ref_pic_sets));
    } else {
        if (pp->st_rps_bits == 0) goto out;                    /* inline set of unknown length */
        b.pos += pp->st_rps_bits;
    }
    /* read the original long-term part: its entry order is the order of RefPicSetLtCurr, which the reference lists
       are built from, so it is kept; entries coded through the SPS list (lt_idx_sps) have no LSBs we can see */
    typedef struct { int sps; int lsb; int used; int msb; } LtOrig;
    LtOrig orig[32]; int n_orig = 0;
    if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
        uint32_t n_lt_sps = 0;
        if (pp->num_long_term_ref_pic_sps > 0) n_lt_sps = br_ue(&b);
        uint32_t n_lt_pics = br_ue(&b);
        if (n_lt_sps > 16 || n_lt_pics > 16) goto out;
        for (uint32_t k = 0; k < n_lt_sps + n_lt_pics; k++) {
            LtOrig *e = &orig[n_orig++];
            e->sps = k < n_lt_sps; e->lsb = -1; e->used = -1;
            if (e->sps) {
                if (pp->num_long_term_ref_pic_sps > 1) br_u(&b, ceil_log2(pp->num_long_term_ref_pic_sps));
            } else {
                e->lsb = (int)br_u(&b, poc_bits); e->used = (int)br_u(&b, 1);
            }
            e->msb = (int)br_u(&b, 1);
            if (e->msb) br_ue(&b);                             /* delta_poc_msb_cycle_lt */
        }
    }
    size_t rest_start = b.pos;
    if (rest_start > (size_t)data_byte_offset * 8) goto out;
    /* the header ends with byte_alignment(): a one bit, then up to seven zeros to the byte boundary */
    size_t end_bits = (size_t)data_byte_offset * 8, q = end_bits;
    while (q > rest_start && !((hdr[(q - 1) >> 3] >> (7 - ((q - 1) & 7))) & 1)) q--;
    if (q == rest_start || end_bits - q > 7) goto out;
    q--;                                                       /* position of the alignment one bit */

    /* The set from the client's reference list.  Short-term entries are ordered by the standard (closest first). */
    RpsPic neg[16], pos_[16]; int nn = 0, np = 0;
    int cur = pp->CurrPic.pic_order_cnt;
    int lt_cand[16], n_cand = 0;                               /* ReferenceFrames indices of the long-term pictures */
    for (int k = 0; k < 15; k++) {
        const VAPictureHEVC *r = &pp->ReferenceFrames[k];
        if (r->picture_id == VA_INVALID_ID || (r->flags & VA_PICTURE_HEVC_INVALID)) continue;
        int used_st = !!(r->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER));
        if (r->flags & (VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR)) {
            if (n_cand < 16) lt_cand[n_cand++] = k;
        } else if (r->pic_order_cnt < cur) {
            if (nn < 16) neg[nn++] = (RpsPic){ r->pic_order_cnt, used_st };
        } else if (r->pic_order_cnt > cur) {
            if (np < 16) pos_[np++] = (RpsPic){ r->pic_order_cnt, used_st };
        } else goto out;                                       /* a short-term reference with the current POC */
    }
    qsort(neg, (size_t)nn, sizeof neg[0], cmp_desc);
    qsort(pos_, (size_t)np, sizeof pos_[0], cmp_asc);
    if (n_cand > 0 && !pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag) goto out;

    /* Long-term entries, in the original header's order.  An explicit entry is matched to a candidate by its POC LSBs
       (and its MSB cycle when the header gave one); the SPS-coded entries, which come first, are filled with the
       remaining candidates: the used ones in the order the client's RefPicList names them, then the unused ones.
       Pictures the client lists but the header did not name are appended (as far as this driver can tell, that does
       not happen: ReferenceFrames is the picture's set). */
    RpsPic lt[16]; int cyc[16], need_msb[16], nl = 0; int taken[16] = {0};
    int lsb_mask = (int)(max_lsb - 1), cur_lsb = cur & lsb_mask;
    int cycle_of[16];
    for (int c2 = 0; c2 < n_cand; c2++) {
        int poc = pp->ReferenceFrames[lt_cand[c2]].pic_order_cnt;
        int d = cur - poc - (cur_lsb - (poc & lsb_mask));
        cycle_of[c2] = (d % (int)max_lsb) ? -1000000 : d / (int)max_lsb;   /* a non-multiple cannot be coded */
    }
    int unknown_slots = 0;
    for (int e = 0; e < n_orig; e++) {
        if (orig[e].sps) { unknown_slots++; continue; }
        int found = -1;
        for (int c2 = 0; c2 < n_cand && found < 0; c2++) {
            if (taken[c2]) continue;
            if ((pp->ReferenceFrames[lt_cand[c2]].pic_order_cnt & lsb_mask) == orig[e].lsb) found = c2;
        }
        if (found < 0) goto out;                               /* the header names a picture the client did not give */
        taken[found] = 1; orig[e].used = orig[e].used;         /* keep the header's used flag */
    }
    /* assign candidates to slots: first the SPS-coded slots (unknown pictures), then the matched explicit ones */
    int fill_order[16], nf = 0; int placed[16] = {0};
    for (int l = 0; l < 2; l++)                                /* used candidates in RefPicList order */
        for (int k = 0; k < 15; k++) {
            int idx = ref_list ? ref_list[l][k] : 0xff;
            if (idx >= 15) continue;
            for (int c2 = 0; c2 < n_cand; c2++)
                if (lt_cand[c2] == idx && !taken[c2] && !placed[c2]) { fill_order[nf++] = c2; placed[c2] = 1; }
        }
    for (int c2 = 0; c2 < n_cand; c2++) if (!taken[c2] && !placed[c2]) { fill_order[nf++] = c2; placed[c2] = 1; }
    int fi = 0, cum = 0;
    for (int e = 0; e < n_orig; e++) {
        int c2;
        if (orig[e].sps) {
            if (fi >= nf) continue;                            /* fewer candidates than slots: the slot is dropped */
            c2 = fill_order[fi++];
        } else {
            c2 = -1;
            for (int t = 0; t < n_cand; t++)
                if (taken[t] && (pp->ReferenceFrames[lt_cand[t]].pic_order_cnt & lsb_mask) == orig[e].lsb && taken[t] != 2) { c2 = t; break; }
            if (c2 < 0) goto out;
            taken[c2] = 2;
        }
        const VAPictureHEVC *r = &pp->ReferenceFrames[lt_cand[c2]];
        lt[nl] = (RpsPic){ r->pic_order_cnt, !!(r->flags & VA_PICTURE_HEVC_RPS_LT_CURR) };
        cyc[nl] = cycle_of[c2];
        /* The MSB cycle whenever it can be coded (cycles must not decrease along the entries that carry one): the
           decoder then matches the picture by its full POC, which no picture still waiting in its DPB can be
           confused with (MPP matches LSB-only entries against every slot).  Without it, the LSBs must at least be
           unique among the listed references. */
        int collide = 0;
        for (int t = 0; t < 15 && !collide; t++) {
            const VAPictureHEVC *o = &pp->ReferenceFrames[t];
            if (t == lt_cand[c2] || o->picture_id == VA_INVALID_ID || (o->flags & VA_PICTURE_HEVC_INVALID)) continue;
            if ((o->pic_order_cnt & lsb_mask) == (r->pic_order_cnt & lsb_mask)) collide = 1;
        }
        need_msb[nl] = cyc[nl] >= cum && cyc[nl] > -1000000;
        if (collide && !need_msb[nl]) goto out;                /* ambiguous and not codable */
        if (need_msb[nl]) cum = cyc[nl];
        nl++;
    }
    for (; fi < nf; fi++) {                                    /* listed but never named: append */
        int c2 = fill_order[fi]; const VAPictureHEVC *r = &pp->ReferenceFrames[lt_cand[c2]];
        lt[nl] = (RpsPic){ r->pic_order_cnt, !!(r->flags & VA_PICTURE_HEVC_RPS_LT_CURR) };
        cyc[nl] = cycle_of[c2]; need_msb[nl] = cyc[nl] >= cum && cyc[nl] > -1000000; if (need_msb[nl]) cum = cyc[nl]; nl++;
    }

    size_t rcap = (size_t)data_byte_offset + 256 + 16 * 12;   /* header + the new set, generously */
    raw = malloc(rcap); if (!raw) goto out;
    BSWriter w; bs_init(&w, raw, rcap);
    copy_bits(&w, hdr, 0, prefix_end);
    bs_write1(&w, 0);                                          /* short_term_ref_pic_set_sps_flag */
    bs_write_ue(&w, (uint32_t)nn);                             /* st_ref_pic_set(0): no prediction flag with idx 0 */
    bs_write_ue(&w, (uint32_t)np);
    int prev = cur;
    for (int k = 0; k < nn; k++) { bs_write_ue(&w, (uint32_t)(prev - neg[k].poc - 1)); bs_write1(&w, neg[k].used); prev = neg[k].poc; }
    prev = cur;
    for (int k = 0; k < np; k++) { bs_write_ue(&w, (uint32_t)(pos_[k].poc - prev - 1)); bs_write1(&w, pos_[k].used); prev = pos_[k].poc; }
    if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
        bs_write_ue(&w, (uint32_t)nl);                         /* num_long_term_pics (num_long_term_sps absent: 0 in our SPS) */
        int c = 0;
        for (int k = 0; k < nl; k++) {
            bs_write(&w, (uint32_t)(lt[k].poc & lsb_mask), poc_bits);         /* poc_lsb_lt */
            bs_write1(&w, lt[k].used);                                        /* used_by_curr_pic_lt_flag */
            bs_write1(&w, need_msb[k]);                                       /* delta_poc_msb_present_flag */
            if (need_msb[k]) { bs_write_ue(&w, (uint32_t)(cyc[k] - c)); c = cyc[k]; }
        }
    }
    copy_bits(&w, hdr, rest_start, q);                         /* the rest of the header, up to the alignment bit */
    bs_write1(&w, 1);                                          /* byte_alignment() */
    while (w.bit_pos != 0) bs_write1(&w, 0);
    if (w.byte_pos >= w.capacity) goto out;                    /* bs_write1 drops bits silently past the end */
    size_t hdr_sz = bs_bytes(&w);
    if (hdr_sz * 2 > out_cap) goto out;
    size_t ep = emulation_prevent(raw, hdr_sz, out, out_cap);
    if (!ep) goto out;
    *data_start = data_esc;                                    /* the caller appends nal + data_esc .. len as it is */
    ret = (int)ep;
out:
    free(hdr); free(esc_pos); free(raw);
    return ret;
}

