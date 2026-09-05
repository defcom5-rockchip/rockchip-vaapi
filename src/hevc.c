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
                   const VAPictureParameterBufferHEVC *pp, int is_main10,
                   int num_st_rps)
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

    /* num_short_term_ref_pic_sets: 0, or 1 dummy set (see hevc.h).  The dummy
       set is never referenced by a slice; it only exists so that slice-inline
       RPS parse as st_ref_pic_set(1) and read their inter_ref_pic_set_prediction
       bit, exactly as they did against the encoder's original SPS. */
    bs_write_ue(&bs, (uint32_t)(num_st_rps ? 1 : 0));
    if (num_st_rps) {           /* st_ref_pic_set(0): one used negative pic, delta -1 */
        bs_write_ue(&bs, 1);    /* num_negative_pics */
        bs_write_ue(&bs, 0);    /* num_positive_pics */
        bs_write_ue(&bs, 0);    /* delta_poc_s0_minus1[0] */
        bs_write(&bs, 1, 1);    /* used_by_curr_pic_s0_flag[0] */
    }
    bs_write(&bs, pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag, 1);
    if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        bs_write_ue(&bs, 0);    /* num_long_term_ref_pics_sps = 0 */
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
/* Slice-header probe: which RPS layout did the encoder's SPS imply?         */

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
/* Explicit st_ref_pic_set body (no inter prediction). Returns 0 on nonsense. */
static int parse_explicit_rps(BR *b) {
    uint32_t nneg = br_ue(b), npos = br_ue(b);
    if (nneg > 16 || npos > 16) return 0;
    for (uint32_t i = 0; i < nneg; i++) { br_ue(b); br_u(b, 1); }
    for (uint32_t i = 0; i < npos; i++) { br_ue(b); br_u(b, 1); }
    return 1;
}

int hevc_detect_num_st_rps(const uint8_t *nal, size_t len,
                           const VAPictureParameterBufferHEVC *pp)
{
    uint8_t buf[256];
    size_t n = rbsp_unescape(nal, len < 512 ? len : 512, buf, sizeof(buf));
    if (n < 4) return -1;
    int nal_type = (buf[0] >> 1) & 0x3F;
    if (nal_type == 19 || nal_type == 20) return -1;        /* IDR: no RPS in header */
    if (nal_type > 23) return -1;                           /* not a VCL slice */
    BR b = { buf, n, 16 };                                  /* skip 2-byte NAL header */
    if (!br_u(&b, 1)) return -1;                            /* first_slice_segment_in_pic_flag */
    if (nal_type >= 16) br_u(&b, 1);                        /* no_output_of_prior_pics_flag */
    br_ue(&b);                                              /* slice_pic_parameter_set_id */
    for (int i = 0; i < pp->num_extra_slice_header_bits; i++) br_u(&b, 1);
    br_ue(&b);                                              /* slice_type */
    if (pp->slice_parsing_fields.bits.output_flag_present_flag) br_u(&b, 1);
    if (pp->pic_fields.bits.separate_colour_plane_flag) br_u(&b, 2);
    br_u(&b, pp->log2_max_pic_order_cnt_lsb_minus4 + 4);   /* slice_pic_order_cnt_lsb */
    if (br_u(&b, 1)) return -2;                             /* short_term_ref_pic_set_sps_flag=1 */
    size_t start = b.pos;
    /* Hypothesis A: original SPS had 0 sets -> st_ref_pic_set(0), explicit, no pred bit */
    BR a = b; int okA = parse_explicit_rps(&a); size_t bitsA = a.pos - start;
    /* Hypothesis B: original SPS had >=1 sets -> st_ref_pic_set(idx!=0): pred bit first */
    BR c = b; int pred = (int)br_u(&c, 1); int okB = 0; size_t bitsB = 0;
    if (!pred) { okB = parse_explicit_rps(&c); bitsB = c.pos - start; }
    if (okA && bitsA == pp->st_rps_bits) return 0;
    if (okB && bitsB == pp->st_rps_bits) return 1;
    if (pred) return -2;                                    /* inter-RPS prediction: needs the real sets */
    return -1;
}
