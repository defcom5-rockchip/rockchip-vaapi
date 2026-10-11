/*
 * av1.c — AV1 OBU re-serialisation from VA-API decode parameters (see av1.h)
 *
 * Copyright (C) 2026 defcom5-rockchip
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "av1.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ small helpers */
static int ceil_log2(uint32_t v) { int n = 0; while ((1u << n) < v) n++; return n; }
static size_t leb128_write(uint8_t *p, uint64_t v) { size_t n = 0; do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; p[n++] = b; } while (v); return n; }
static size_t leb128_read(const uint8_t *p, size_t n, uint64_t *v) { *v = 0; for (size_t i = 0; i < 8 && i < n; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (7 * i); if (!(p[i] & 0x80)) return i + 1; } return 0; }
/* number of meaningful bits in an OBU_FRAME_HEADER payload = everything before its trailing 1 bit */
static size_t header_bits(const uint8_t *p, size_t len) { for (size_t i = len; i-- > 0;) if (p[i]) { int b = 0; while (!(p[i] & (1u << b))) b++; return i * 8 + (7 - b); } return 0; }
/* spec get_relative_dist() */
static int rel_dist(const GstAV1SequenceHeaderOBU *seq, int a, int b) {
    if (!seq->enable_order_hint) return 0;
    int bits = seq->order_hint_bits_minus_1 + 1, diff = a - b, m = 1 << (bits - 1);
    diff = (diff & (m - 1)) - (diff & m); return diff;
}

/* ------------------------------------------------------------------ sequence header */
int av1_seq_from_va(const VADecPictureParameterBufferAV1 *pp, GstAV1SequenceHeaderOBU *seq) {
    const uint32_t s = pp->seq_info_fields.value; (void)s;
    memset(seq, 0, sizeof *seq);
    if (pp->profile != 0) return -1;                                   /* Profile0 (4:2:0) only; MPP has no 4:4:4 */
    if (pp->seq_info_fields.fields.mono_chrome) return -1;             /* MPP rejects monochrome */
    if (pp->bit_depth_idx > 1) return -1;                              /* 12-bit */
    seq->seq_profile = 0;
    seq->still_picture = pp->seq_info_fields.fields.still_picture;
    seq->reduced_still_picture_header = 0;
    seq->max_frame_width_minus_1 = pp->frame_width_minus1;             /* the surface size: constant for the sequence */
    seq->max_frame_height_minus_1 = pp->frame_height_minus1;
    seq->frame_width_bits_minus_1 = (uint8_t)(ceil_log2(pp->frame_width_minus1 + 1u) ? ceil_log2(pp->frame_width_minus1 + 1u) - 1 : 0);
    seq->frame_height_bits_minus_1 = (uint8_t)(ceil_log2(pp->frame_height_minus1 + 1u) ? ceil_log2(pp->frame_height_minus1 + 1u) - 1 : 0);
    if ((1u << (seq->frame_width_bits_minus_1 + 1)) <= pp->frame_width_minus1) seq->frame_width_bits_minus_1++;
    if ((1u << (seq->frame_height_bits_minus_1 + 1)) <= pp->frame_height_minus1) seq->frame_height_bits_minus_1++;
    seq->frame_id_numbers_present_flag = 0;
    seq->use_128x128_superblock = pp->seq_info_fields.fields.use_128x128_superblock;
    seq->enable_filter_intra = pp->seq_info_fields.fields.enable_filter_intra;
    seq->enable_intra_edge_filter = pp->seq_info_fields.fields.enable_intra_edge_filter;
    seq->enable_interintra_compound = pp->seq_info_fields.fields.enable_interintra_compound;
    seq->enable_masked_compound = pp->seq_info_fields.fields.enable_masked_compound;
    seq->enable_warped_motion = 1;                                      /* not in VA: allow, frames say whether they use it */
    seq->enable_dual_filter = pp->seq_info_fields.fields.enable_dual_filter;
    seq->enable_order_hint = pp->seq_info_fields.fields.enable_order_hint;
    seq->enable_jnt_comp = pp->seq_info_fields.fields.enable_jnt_comp;
    seq->enable_ref_frame_mvs = seq->enable_order_hint;                /* not in VA: frames carry use_ref_frame_mvs */
    seq->seq_choose_screen_content_tools = 1;                          /* SELECT: every frame codes its own flags */
    seq->seq_force_screen_content_tools = GST_AV1_SELECT_SCREEN_CONTENT_TOOLS;
    seq->seq_choose_integer_mv = 1;
    seq->seq_force_integer_mv = GST_AV1_SELECT_INTEGER_MV;
    seq->order_hint_bits_minus_1 = pp->order_hint_bits_minus_1;
    seq->order_hint_bits = seq->enable_order_hint ? pp->order_hint_bits_minus_1 + 1 : 0;
    seq->enable_superres = 1;
    seq->enable_cdef = pp->seq_info_fields.fields.enable_cdef;
    seq->enable_restoration = 1;
    seq->film_grain_params_present = pp->seq_info_fields.fields.film_grain_params_present;
    seq->operating_points_cnt_minus_1 = 0;
    seq->operating_points[0].idc = 0;
    seq->operating_points[0].seq_level_idx = 31;                        /* "maximum parameters" */
    seq->operating_points[0].seq_tier = 0;
    seq->decoder_model_info_present_flag = 0;
    seq->initial_display_delay_present_flag = 0;
    seq->timing_info_present_flag = 0;
    seq->bit_depth = pp->bit_depth_idx ? 10 : 8;
    seq->color_config.high_bitdepth = pp->bit_depth_idx ? 1 : 0;
    seq->color_config.twelve_bit = 0;
    seq->color_config.mono_chrome = 0;
    seq->color_config.color_description_present_flag = 0;
    seq->color_config.color_primaries = 2; seq->color_config.transfer_characteristics = 2; seq->color_config.matrix_coefficients = 2;
    seq->color_config.color_range = pp->seq_info_fields.fields.color_range;
    seq->color_config.subsampling_x = pp->seq_info_fields.fields.subsampling_x;
    seq->color_config.subsampling_y = pp->seq_info_fields.fields.subsampling_y;
    if (!(seq->color_config.subsampling_x && seq->color_config.subsampling_y)) return -1;   /* 4:2:2 / 4:4:4 */
    seq->color_config.chroma_sample_position = 0;
    seq->color_config.separate_uv_delta_q = 1;                          /* diff_uv_delta is then coded per frame */
    seq->num_planes = 3;
    return 0;
}

bool av1_seq_matches(const VADecPictureParameterBufferAV1 *pp, const GstAV1SequenceHeaderOBU *seq) {
    return pp->profile == 0 && seq->max_frame_width_minus_1 == pp->frame_width_minus1 && seq->max_frame_height_minus_1 == pp->frame_height_minus1 &&
           seq->bit_depth == (pp->bit_depth_idx ? 10 : 8) &&
           seq->use_128x128_superblock == pp->seq_info_fields.fields.use_128x128_superblock &&
           seq->enable_order_hint == pp->seq_info_fields.fields.enable_order_hint &&
           seq->order_hint_bits_minus_1 == pp->order_hint_bits_minus_1 &&
           seq->enable_cdef == pp->seq_info_fields.fields.enable_cdef &&
           seq->enable_filter_intra == pp->seq_info_fields.fields.enable_filter_intra &&
           seq->enable_intra_edge_filter == pp->seq_info_fields.fields.enable_intra_edge_filter &&
           seq->enable_interintra_compound == pp->seq_info_fields.fields.enable_interintra_compound &&
           seq->enable_masked_compound == pp->seq_info_fields.fields.enable_masked_compound &&
           seq->enable_dual_filter == pp->seq_info_fields.fields.enable_dual_filter &&
           seq->enable_jnt_comp == pp->seq_info_fields.fields.enable_jnt_comp &&
           seq->film_grain_params_present == pp->seq_info_fields.fields.film_grain_params_present &&
           seq->color_config.color_range == pp->seq_info_fields.fields.color_range &&
           seq->still_picture == pp->seq_info_fields.fields.still_picture;
}

/* ------------------------------------------------------------------ tile info */
/* spec tile_log2(): smallest k with blkSize << k >= target */
static int tile_log2(int blk, int target) { int k = 0; while ((blk << k) < target) k++; return k; }

static int fill_tile_info(const VADecPictureParameterBufferAV1 *pp, const GstAV1SequenceHeaderOBU *seq, GstAV1FrameHeaderOBU *fh) {
    GstAV1TileInfo *ti = &fh->tile_info;
    int mi_cols = 2 * ((fh->frame_width + 7) >> 3), mi_rows = 2 * ((fh->frame_height + 7) >> 3);
    int sb128 = seq->use_128x128_superblock, sb_shift = sb128 ? 5 : 4, sb_size = sb_shift + 2;
    int sb_cols = sb128 ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4), sb_rows = sb128 ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
    int max_tile_width_sb = 4096 >> sb_size, max_tile_area_sb = (4096 * 2304) >> (2 * sb_size);
    int min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
    int max_log2_tile_cols = tile_log2(1, sb_cols < 64 ? sb_cols : 64);
    int max_log2_tile_rows = tile_log2(1, sb_rows < 64 ? sb_rows : 64);
    int min_log2_tiles = min_log2_tile_cols > tile_log2(max_tile_area_sb, sb_rows * sb_cols) ? min_log2_tile_cols : tile_log2(max_tile_area_sb, sb_rows * sb_cols);
    int cols = pp->tile_cols, rows = pp->tile_rows;
    if (cols < 1 || rows < 1 || cols > 64 || rows > 64) return -1;
    memset(ti, 0, sizeof *ti);
    ti->uniform_tile_spacing_flag = pp->pic_info_fields.bits.uniform_tile_spacing_flag;
    ti->tile_cols = cols; ti->tile_rows = rows;
    ti->context_update_tile_id = pp->context_update_tile_id;
    ti->tile_size_bytes_minus_1 = 3; ti->tile_size_bytes = 4;
    if (ti->uniform_tile_spacing_flag) {
        /* find the log2 increments that reproduce the client's tile counts (spec 5.9.15 uniform case) */
        int lc = -1, lr = -1;
        for (int k = min_log2_tile_cols; k <= max_log2_tile_cols; k++) {
            int tw = (sb_cols + (1 << k) - 1) >> k, n = 0; for (int st = 0; st < sb_cols; st += tw) n++;
            if (n == cols) { lc = k; break; }
        }
        if (lc < 0) return -1;
        int min_log2_tile_rows = min_log2_tiles - lc > 0 ? min_log2_tiles - lc : 0;
        for (int k = min_log2_tile_rows; k <= max_log2_tile_rows; k++) {
            int th = (sb_rows + (1 << k) - 1) >> k, n = 0; for (int st = 0; st < sb_rows; st += th) n++;
            if (n == rows) { lr = k; break; }
        }
        if (lr < 0) return -1;
        ti->tile_cols_log2 = lc; ti->tile_rows_log2 = lr;
        int tw = (sb_cols + (1 << lc) - 1) >> lc, th = (sb_rows + (1 << lr) - 1) >> lr, i = 0;
        for (int st = 0; st < sb_cols; st += tw) ti->mi_col_starts[i++] = st << sb_shift; ti->mi_col_starts[i] = mi_cols;
        i = 0; for (int st = 0; st < sb_rows; st += th) ti->mi_row_starts[i++] = st << sb_shift; ti->mi_row_starts[i] = mi_rows;
    } else {
        int start = 0;
        for (int i = 0; i < cols; i++) { ti->mi_col_starts[i] = start << sb_shift; start += pp->width_in_sbs_minus_1[i] + 1; }
        ti->mi_col_starts[cols] = mi_cols;
        start = 0;
        for (int i = 0; i < rows; i++) { ti->mi_row_starts[i] = start << sb_shift; start += pp->height_in_sbs_minus_1[i] + 1; }
        ti->mi_row_starts[rows] = mi_rows;
        ti->tile_cols_log2 = tile_log2(1, cols); ti->tile_rows_log2 = tile_log2(1, rows);
    }
    return 0;
}

/* ------------------------------------------------------------------ frame header */
int av1_frame_from_va(const VADecPictureParameterBufferAV1 *pp, const GstAV1SequenceHeaderOBU *seq,
                      const Av1RefInfo refs[8], uint8_t refresh_frame_flags, GstAV1FrameHeaderOBU *fh) {
    const uint32_t pf = pp->pic_info_fields.value; (void)pf;
    memset(fh, 0, sizeof *fh);
    if (pp->pic_info_fields.bits.large_scale_tile) return -1;
    fh->show_existing_frame = 0;
    fh->frame_type = pp->pic_info_fields.bits.frame_type;
    fh->show_frame = pp->pic_info_fields.bits.show_frame;
    fh->showable_frame = pp->pic_info_fields.bits.showable_frame;
    fh->frame_is_intra = (fh->frame_type == GST_AV1_KEY_FRAME || fh->frame_type == GST_AV1_INTRA_ONLY_FRAME);
    fh->error_resilient_mode = pp->pic_info_fields.bits.error_resilient_mode;
    if (fh->frame_type == GST_AV1_SWITCH_FRAME || (fh->frame_type == GST_AV1_KEY_FRAME && fh->show_frame)) fh->error_resilient_mode = 1;
    fh->disable_cdf_update = pp->pic_info_fields.bits.disable_cdf_update;
    fh->allow_screen_content_tools = pp->pic_info_fields.bits.allow_screen_content_tools;
    fh->force_integer_mv = fh->allow_screen_content_tools ? pp->pic_info_fields.bits.force_integer_mv : 0;
    if (fh->frame_is_intra) fh->force_integer_mv = 1;
    fh->current_frame_id = 0;
    fh->frame_size_override_flag = 0;                                   /* frame size == sequence max (surface size) */
    fh->order_hint = pp->order_hint;
    fh->primary_ref_frame = (fh->frame_is_intra || fh->error_resilient_mode) ? GST_AV1_PRIMARY_REF_NONE : pp->primary_ref_frame;
    fh->buffer_removal_time_present_flag = 0;
    fh->refresh_frame_flags = (fh->frame_type == GST_AV1_KEY_FRAME && fh->show_frame) ? 0xff : refresh_frame_flags;
    if (fh->frame_type == GST_AV1_INTRA_ONLY_FRAME && fh->refresh_frame_flags == 0xff) return -1;   /* spec forbids */
    fh->upscaled_width = pp->frame_width_minus1 + 1;
    fh->frame_height = pp->frame_height_minus1 + 1;
    fh->use_superres = pp->pic_info_fields.bits.use_superres;
    if (fh->use_superres) return -1;                                    /* MPP outputs the downscaled width; refused for now */
    fh->superres_denom = GST_AV1_SUPERRES_NUM;
    fh->frame_width = fh->upscaled_width;
    fh->render_and_frame_size_different = 0;
    fh->render_width = fh->frame_width; fh->render_height = fh->frame_height;
    fh->allow_intrabc = fh->frame_is_intra ? pp->pic_info_fields.bits.allow_intrabc : 0;
    /* references */
    fh->frame_refs_short_signaling = 0;
    for (int i = 0; i < 7; i++) fh->ref_frame_idx[i] = pp->ref_frame_idx[i] & 7;
    for (int i = 0; i < 8; i++) { fh->ref_order_hint[i] = refs[i].valid ? refs[i].order_hint : 0; fh->order_hints[i] = 0; }
    if (!fh->frame_is_intra) {
        for (int i = 0; i < 7; i++) { int s = fh->ref_frame_idx[i]; fh->order_hints[i + 1] = refs[s].valid ? refs[s].order_hint : 0; fh->expected_frame_id[i] = 0; }
        fh->allow_high_precision_mv = fh->force_integer_mv ? 0 : pp->pic_info_fields.bits.allow_high_precision_mv;
        fh->is_filter_switchable = (pp->interp_filter == 4);
        fh->interpolation_filter = fh->is_filter_switchable ? GST_AV1_INTERPOLATION_FILTER_SWITCHABLE : (GstAV1InterpolationFilter)pp->interp_filter;
        fh->is_motion_mode_switchable = pp->pic_info_fields.bits.is_motion_mode_switchable;
        fh->use_ref_frame_mvs = (fh->error_resilient_mode || !seq->enable_ref_frame_mvs) ? 0 : pp->pic_info_fields.bits.use_ref_frame_mvs;
    }
    fh->disable_frame_end_update_cdf = (seq->reduced_still_picture_header || fh->disable_cdf_update) ? 1 : pp->pic_info_fields.bits.disable_frame_end_update_cdf;
    if (fill_tile_info(pp, seq, fh) < 0) return -1;
    /* quantization */
    GstAV1QuantizationParams *q = &fh->quantization_params;
    q->base_q_idx = pp->base_qindex;
    q->delta_q_y_dc = pp->y_dc_delta_q; q->delta_q_u_dc = pp->u_dc_delta_q; q->delta_q_u_ac = pp->u_ac_delta_q;
    q->delta_q_v_dc = pp->v_dc_delta_q; q->delta_q_v_ac = pp->v_ac_delta_q;
    q->diff_uv_delta = (q->delta_q_v_dc != q->delta_q_u_dc || q->delta_q_v_ac != q->delta_q_u_ac);
    q->using_qmatrix = pp->qmatrix_fields.bits.using_qmatrix;
    q->qm_y = pp->qmatrix_fields.bits.qm_y; q->qm_u = pp->qmatrix_fields.bits.qm_u; q->qm_v = pp->qmatrix_fields.bits.qm_v;
    q->delta_q_present = q->base_q_idx > 0 ? pp->mode_control_fields.bits.delta_q_present_flag : 0;
    q->delta_q_res = pp->mode_control_fields.bits.log2_delta_q_res;
    /* segmentation (effective values from VA: update_data = 1 whenever enabled) */
    GstAV1SegmentationParams *sg = &fh->segmentation_params;
    sg->segmentation_enabled = pp->seg_info.segment_info_fields.bits.enabled;
    if (sg->segmentation_enabled) {
        if (fh->primary_ref_frame == GST_AV1_PRIMARY_REF_NONE) { sg->segmentation_update_map = 1; sg->segmentation_temporal_update = 0; }
        else { sg->segmentation_update_map = pp->seg_info.segment_info_fields.bits.update_map; sg->segmentation_temporal_update = sg->segmentation_update_map ? pp->seg_info.segment_info_fields.bits.temporal_update : 0; }
        sg->segmentation_update_data = 1;
        for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) {
            sg->feature_enabled[i][j] = (pp->seg_info.feature_mask[i] >> j) & 1;
            sg->feature_data[i][j] = sg->feature_enabled[i][j] ? pp->seg_info.feature_data[i][j] : 0;
            if (sg->feature_enabled[i][j]) { sg->last_active_seg_id = i; if (j >= 5) sg->seg_id_pre_skip = 1; }
        }
    }
    /* lossless (spec 5.9.12-5.9.13 derivation) */
    fh->coded_lossless = 1;
    for (int i = 0; i < 8; i++) {
        int qindex = q->base_q_idx;
        if (sg->segmentation_enabled && sg->feature_enabled[i][0]) { qindex += sg->feature_data[i][0]; if (qindex < 0) qindex = 0; if (qindex > 255) qindex = 255; }
        fh->lossless_array[i] = (qindex == 0 && q->delta_q_y_dc == 0 && q->delta_q_u_ac == 0 && q->delta_q_u_dc == 0 && q->delta_q_v_ac == 0 && q->delta_q_v_dc == 0);
        if (!fh->lossless_array[i]) fh->coded_lossless = 0;
    }
    fh->all_lossless = fh->coded_lossless && fh->frame_width == fh->upscaled_width;
    /* loop filter + delta lf */
    GstAV1LoopFilterParams *lf = &fh->loop_filter_params;
    lf->delta_lf_present = q->delta_q_present ? (fh->allow_intrabc ? 0 : pp->mode_control_fields.bits.delta_lf_present_flag) : 0;
    lf->delta_lf_res = pp->mode_control_fields.bits.log2_delta_lf_res;
    lf->delta_lf_multi = pp->mode_control_fields.bits.delta_lf_multi;
    if (!(fh->coded_lossless || fh->allow_intrabc)) {
        lf->loop_filter_level[0] = pp->filter_level[0]; lf->loop_filter_level[1] = pp->filter_level[1];
        lf->loop_filter_level[2] = pp->filter_level_u; lf->loop_filter_level[3] = pp->filter_level_v;
        lf->loop_filter_sharpness = pp->loop_filter_info_fields.bits.sharpness_level;
        lf->loop_filter_delta_enabled = pp->loop_filter_info_fields.bits.mode_ref_delta_enabled;
        lf->loop_filter_delta_update = lf->loop_filter_delta_enabled;     /* always write the effective deltas */
        for (int i = 0; i < 8; i++) lf->loop_filter_ref_deltas[i] = pp->ref_deltas[i];
        for (int i = 0; i < 2; i++) lf->loop_filter_mode_deltas[i] = pp->mode_deltas[i];
    }
    /* cdef (VA packs (pri << 2) | sec with the coded 0..3 secondary strength) */
    GstAV1CDEFParams *cd = &fh->cdef_params;
    cd->cdef_damping = pp->cdef_damping_minus_3 + 3; cd->cdef_bits = pp->cdef_bits;
    for (int i = 0; i < 8; i++) {
        cd->cdef_y_pri_strength[i] = pp->cdef_y_strengths[i] >> 2; cd->cdef_y_sec_strength[i] = pp->cdef_y_strengths[i] & 3;
        cd->cdef_uv_pri_strength[i] = pp->cdef_uv_strengths[i] >> 2; cd->cdef_uv_sec_strength[i] = pp->cdef_uv_strengths[i] & 3;
    }
    /* loop restoration */
    GstAV1LoopRestorationParams *lr = &fh->loop_restoration_params;
    static const GstAV1FrameRestorationType va2gst[4] = { GST_AV1_FRAME_RESTORE_NONE, GST_AV1_FRAME_RESTORE_WIENER, GST_AV1_FRAME_RESTORE_SGRPROJ, GST_AV1_FRAME_RESTORE_SWITCHABLE };
    lr->frame_restoration_type[0] = va2gst[pp->loop_restoration_fields.bits.yframe_restoration_type];
    lr->frame_restoration_type[1] = va2gst[pp->loop_restoration_fields.bits.cbframe_restoration_type];
    lr->frame_restoration_type[2] = va2gst[pp->loop_restoration_fields.bits.crframe_restoration_type];
    lr->uses_lr = (lr->frame_restoration_type[0] != GST_AV1_FRAME_RESTORE_NONE || lr->frame_restoration_type[1] != GST_AV1_FRAME_RESTORE_NONE || lr->frame_restoration_type[2] != GST_AV1_FRAME_RESTORE_NONE);
    lr->lr_unit_shift = pp->loop_restoration_fields.bits.lr_unit_shift; lr->lr_uv_shift = pp->loop_restoration_fields.bits.lr_uv_shift;
    /* tx mode, reference mode, skip mode */
    fh->tx_mode = fh->coded_lossless ? GST_AV1_TX_MODE_ONLY_4x4 : (GstAV1TXModes)pp->mode_control_fields.bits.tx_mode;
    fh->tx_mode_select = (fh->tx_mode == GST_AV1_TX_MODE_SELECT);
    fh->reference_select = fh->frame_is_intra ? 0 : pp->mode_control_fields.bits.reference_select;
    fh->skip_mode_present = 0; fh->skip_mode_frame[0] = fh->skip_mode_frame[1] = 0;
    if (!fh->frame_is_intra && fh->reference_select && seq->enable_order_hint) {               /* spec 5.9.22 skipModeAllowed */
        int fwd = -1, bwd = -1, fwd_hint = 0, bwd_hint = 0;
        for (int i = 0; i < 7; i++) {
            int h = fh->order_hints[i + 1];
            if (rel_dist(seq, h, fh->order_hint) < 0) { if (fwd < 0 || rel_dist(seq, h, fwd_hint) > 0) { fwd = i; fwd_hint = h; } }
            else if (rel_dist(seq, h, fh->order_hint) > 0) { if (bwd < 0 || rel_dist(seq, h, bwd_hint) < 0) { bwd = i; bwd_hint = h; } }
        }
        int allowed = 0, a = 0, b = 0;
        if (fwd >= 0 && bwd >= 0) { allowed = 1; a = fwd < bwd ? fwd : bwd; b = fwd < bwd ? bwd : fwd; }
        else if (fwd >= 0) {
            int sf = -1, sf_hint = 0;
            for (int i = 0; i < 7; i++) { int h = fh->order_hints[i + 1]; if (rel_dist(seq, h, fwd_hint) < 0) { if (sf < 0 || rel_dist(seq, h, sf_hint) > 0) { sf = i; sf_hint = h; } } }
            if (sf >= 0) { allowed = 1; a = fwd < sf ? fwd : sf; b = fwd < sf ? sf : fwd; }
        }
        if (allowed) { fh->skip_mode_frame[0] = 1 + a; fh->skip_mode_frame[1] = 1 + b; fh->skip_mode_present = pp->mode_control_fields.bits.skip_mode_present; }
    }
    fh->allow_warped_motion = (fh->frame_is_intra || fh->error_resilient_mode || !seq->enable_warped_motion) ? 0 : pp->pic_info_fields.bits.allow_warped_motion;
    fh->reduced_tx_set = pp->mode_control_fields.bits.reduced_tx_set_used;
    /* global motion: current params from VA, previous from the primary reference's saved params */
    GstAV1GlobalMotionParams *gm = &fh->global_motion_params;
    av1_gm_identity(gm->gm_params);
    for (int r = 1; r < 8; r++) {
        const VAWarpedMotionParamsAV1 *w = &pp->wm[r - 1];
        gm->gm_type[r] = fh->frame_is_intra ? GST_AV1_WARP_MODEL_IDENTITY : (GstAV1WarpModelType)w->wmtype;
        gm->is_global[r] = gm->gm_type[r] != GST_AV1_WARP_MODEL_IDENTITY;
        gm->is_rot_zoom[r] = gm->gm_type[r] == GST_AV1_WARP_MODEL_ROTZOOM;
        gm->is_translation[r] = gm->gm_type[r] == GST_AV1_WARP_MODEL_TRANSLATION;
        gm->invalid[r] = w->invalid;
        if (gm->is_global[r]) for (int i = 0; i < 6; i++) gm->gm_params[r][i] = w->wmmat[i];
    }
    if (fh->primary_ref_frame == GST_AV1_PRIMARY_REF_NONE) av1_gm_identity(fh->ref_global_motion_params.gm_params);
    else { int s = fh->ref_frame_idx[fh->primary_ref_frame]; if (refs[s].valid) memcpy(fh->ref_global_motion_params.gm_params, refs[s].gm, sizeof refs[s].gm); else av1_gm_identity(fh->ref_global_motion_params.gm_params); }
    /* film grain: effective values, always update_grain = 1 */
    GstAV1FilmGrainParams *fg = &fh->film_grain_params;
    if (seq->film_grain_params_present && (fh->show_frame || fh->showable_frame)) {
        const VAFilmGrainStructAV1 *g = &pp->film_grain_info;
        fg->apply_grain = g->film_grain_info_fields.bits.apply_grain;
        if (fg->apply_grain) {
            fg->grain_seed = g->grain_seed; fg->update_grain = 1; fg->film_grain_params_ref_idx = 0;
            fg->num_y_points = g->num_y_points > 14 ? 14 : g->num_y_points;
            for (int i = 0; i < fg->num_y_points; i++) { fg->point_y_value[i] = g->point_y_value[i]; fg->point_y_scaling[i] = g->point_y_scaling[i]; }
            fg->chroma_scaling_from_luma = g->film_grain_info_fields.bits.chroma_scaling_from_luma;
            fg->num_cb_points = g->num_cb_points > 10 ? 10 : g->num_cb_points; fg->num_cr_points = g->num_cr_points > 10 ? 10 : g->num_cr_points;
            if (fg->chroma_scaling_from_luma) fg->num_cb_points = fg->num_cr_points = 0;
            for (int i = 0; i < fg->num_cb_points; i++) { fg->point_cb_value[i] = g->point_cb_value[i]; fg->point_cb_scaling[i] = g->point_cb_scaling[i]; }
            for (int i = 0; i < fg->num_cr_points; i++) { fg->point_cr_value[i] = g->point_cr_value[i]; fg->point_cr_scaling[i] = g->point_cr_scaling[i]; }
            fg->grain_scaling_minus_8 = g->film_grain_info_fields.bits.grain_scaling_minus_8;
            fg->ar_coeff_lag = g->film_grain_info_fields.bits.ar_coeff_lag;
            int npos_luma = 2 * fg->ar_coeff_lag * (fg->ar_coeff_lag + 1), npos_chroma = npos_luma + 1;
            if (fg->num_y_points) for (int i = 0; i < npos_luma; i++) fg->ar_coeffs_y_plus_128[i] = g->ar_coeffs_y[i] + 128;
            if (fg->chroma_scaling_from_luma || fg->num_cb_points) for (int i = 0; i < npos_chroma; i++) fg->ar_coeffs_cb_plus_128[i] = g->ar_coeffs_cb[i] + 128;
            if (fg->chroma_scaling_from_luma || fg->num_cr_points) for (int i = 0; i < npos_chroma; i++) fg->ar_coeffs_cr_plus_128[i] = g->ar_coeffs_cr[i] + 128;
            fg->ar_coeff_shift_minus_6 = g->film_grain_info_fields.bits.ar_coeff_shift_minus_6;
            fg->grain_scale_shift = g->film_grain_info_fields.bits.grain_scale_shift;
            if (fg->num_cb_points) { fg->cb_mult = g->cb_mult; fg->cb_luma_mult = g->cb_luma_mult; fg->cb_offset = g->cb_offset; }
            if (fg->num_cr_points) { fg->cr_mult = g->cr_mult; fg->cr_luma_mult = g->cr_luma_mult; fg->cr_offset = g->cr_offset; }
            fg->overlap_flag = g->film_grain_info_fields.bits.overlap_flag;
            fg->clip_to_restricted_range = g->film_grain_info_fields.bits.clip_to_restricted_range;
        }
    }
    return 0;
}

void av1_ref_from_frame(const VADecPictureParameterBufferAV1 *pp, const GstAV1FrameHeaderOBU *fh, Av1RefInfo *out) {
    memset(out, 0, sizeof *out);
    out->valid = true; out->frame_type = fh->frame_type; out->order_hint = fh->order_hint;
    out->width = fh->upscaled_width; out->height = fh->frame_height;
    if (fh->frame_is_intra) av1_gm_identity(out->gm); else memcpy(out->gm, fh->global_motion_params.gm_params, sizeof out->gm);
    for (int r = 0; r < 8; r++) out->gm_type[r] = fh->frame_is_intra ? GST_AV1_WARP_MODEL_IDENTITY : fh->global_motion_params.gm_type[r];
    (void)pp;
}

/* ------------------------------------------------------------------ OBU writers */
size_t av1_write_seq_obu(const GstAV1SequenceHeaderOBU *seq, uint8_t *dst, size_t cap) {
    guint sz = cap > 0xffffffffu ? 0xffffffffu : (guint)cap;
    if (gst_av1_bit_writer_sequence_header_obu(seq, TRUE, dst, &sz) != GST_AV1_BIT_WRITER_OK) return 0;
    return sz;
}

size_t av1_write_frame_obu(const GstAV1FrameHeaderOBU *fh, const GstAV1SequenceHeaderOBU *seq,
                           const Av1Tile *tiles, int ntiles, uint8_t *dst, size_t cap) {
    uint8_t tmp[1 << 16]; guint tsz = sizeof tmp;
    if (ntiles < 1) return 0;
    if (gst_av1_bit_writer_frame_header_obu(fh, seq, 0, 0, TRUE, tmp, &tsz) != GST_AV1_BIT_WRITER_OK) return 0;
    int ext = (tmp[0] >> 2) & 1; size_t off = 1 + ext; uint64_t psz; size_t l = leb128_read(tmp + off, tsz - off, &psz);
    if (!l) return 0;
    const uint8_t *p = tmp + off + l; size_t nb = header_bits(p, psz), hb = (nb + 7) / 8;
    size_t tg_hdr = ntiles > 1 ? 1 : 0;                                 /* tile_start_and_end_present_flag (0) + byte_alignment() */
    size_t payload = hb + tg_hdr; for (int i = 0; i < ntiles; i++) payload += tiles[i].len + (i + 1 < ntiles ? 4 : 0);
    if (payload + 16 > cap) return 0;
    size_t n = 0;
    dst[n++] = (uint8_t)((6 << 3) | 0x02);                              /* OBU_FRAME, has_size_field */
    n += leb128_write(dst + n, payload);
    memcpy(dst + n, p, hb); if (nb % 8) dst[n + hb - 1] &= (uint8_t)(0xff << (8 - nb % 8)); n += hb;
    if (tg_hdr) dst[n++] = 0;
    for (int i = 0; i < ntiles; i++) {
        if (i + 1 < ntiles) { uint32_t s1 = (uint32_t)tiles[i].len - 1; dst[n++] = s1; dst[n++] = s1 >> 8; dst[n++] = s1 >> 16; dst[n++] = s1 >> 24; }
        memcpy(dst + n, tiles[i].data, tiles[i].len); n += tiles[i].len;
    }
    return n;
}

size_t av1_write_show_existing(const GstAV1SequenceHeaderOBU *seq, int slot, uint8_t frame_type, uint8_t *dst, size_t cap) {
    GstAV1FrameHeaderOBU se; memset(&se, 0, sizeof se);
    se.show_existing_frame = 1; se.frame_to_show_map_idx = slot & 7; se.frame_type = frame_type; se.show_frame = 1;
    guint sz = cap > 0xffffffffu ? 0xffffffffu : (guint)cap;
    if (gst_av1_bit_writer_frame_header_obu(&se, seq, 0, 0, TRUE, dst, &sz) != GST_AV1_BIT_WRITER_OK) return 0;
    return sz;
}
