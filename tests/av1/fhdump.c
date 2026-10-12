/* fhdump.c — print every frame header field of an IVF or raw OBU stream (gstav1parser), one frame per block, for diffing
 * original vs re-serialised streams.
 * Build (from src/av1 with this file copied there): gcc -O1 -DGST_USE_UNSTABLE_API -I. -o fhdump fhdump.c \
 *   $(pkg-config --cflags --libs gstreamer-codecparsers-1.0) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gstav1bitwriter.h"
#define P(f) printf("  " #f "=%d\n", (int)(fh->f))
#define PA(f, n) do { printf("  " #f "="); for (int _i = 0; _i < (n); _i++) printf("%d,", (int)(fh->f[_i])); printf("\n"); } while (0)
static void dump(const GstAV1FrameHeaderOBU *fh, int idx) {
    printf("frame %d\n", idx);
    P(show_existing_frame); if (fh->show_existing_frame) { P(frame_to_show_map_idx); return; }
    P(frame_type); P(show_frame); P(showable_frame); P(error_resilient_mode); P(disable_cdf_update); P(allow_screen_content_tools);
    P(force_integer_mv); P(frame_size_override_flag); P(order_hint); P(primary_ref_frame); P(refresh_frame_flags);
    PA(ref_order_hint, 8); P(allow_intrabc); P(frame_refs_short_signaling); PA(ref_frame_idx, 7); P(allow_high_precision_mv);
    P(is_filter_switchable); P(interpolation_filter); P(is_motion_mode_switchable); P(use_ref_frame_mvs); P(reduced_tx_set);
    P(allow_warped_motion); P(reference_select); P(skip_mode_present); P(frame_width); P(frame_height); P(upscaled_width);
    P(render_width); P(render_height); P(use_superres); P(superres_denom); P(tx_mode); P(disable_frame_end_update_cdf);
    P(skip_mode_frame[0]); P(skip_mode_frame[1]); P(coded_lossless); P(all_lossless);
    const GstAV1QuantizationParams *q = &fh->quantization_params;
    printf("  q: base=%d dy=%d du=%d ddu=%d dv=%d ddv=%d diff_uv=%d qm=%d qmy=%d qmu=%d qmv=%d  delta_q_present=%d delta_q_res=%d\n",
        q->base_q_idx, q->delta_q_y_dc, q->delta_q_u_dc, q->delta_q_u_ac, q->delta_q_v_dc, q->delta_q_v_ac, q->diff_uv_delta, q->using_qmatrix, q->qm_y, q->qm_u, q->qm_v,
        q->delta_q_present, q->delta_q_res);
    const GstAV1SegmenationParams *s = &fh->segmentation_params;
    printf("  seg: enabled=%d update_map=%d temporal=%d update_data=%d preskip=%d last_active=%d\n", s->segmentation_enabled, s->segmentation_update_map, s->segmentation_temporal_update, s->segmentation_update_data, s->seg_id_pre_skip, s->last_active_seg_id);
    if (s->segmentation_enabled) for (int i = 0; i < 8; i++) { printf("   seg%d:", i); for (int j = 0; j < 8; j++) printf(" %d/%d", s->feature_enabled[i][j], s->feature_data[i][j]); printf("\n"); }
    const GstAV1LoopFilterParams *l = &fh->loop_filter_params;
    printf("  lf: level=%d,%d,%d,%d sharp=%d delta_en=%d delta_upd=%d dlf_present=%d dlf_res=%d dlf_multi=%d ref_deltas=", l->loop_filter_level[0], l->loop_filter_level[1], l->loop_filter_level[2], l->loop_filter_level[3], l->loop_filter_sharpness, l->loop_filter_delta_enabled, l->loop_filter_delta_update, l->delta_lf_present, l->delta_lf_res, l->delta_lf_multi);
    for (int i = 0; i < 8; i++) printf("%d,", l->loop_filter_ref_deltas[i]); printf(" mode_deltas=%d,%d\n", l->loop_filter_mode_deltas[0], l->loop_filter_mode_deltas[1]);
    const GstAV1CDEFParams *c = &fh->cdef_params;
    printf("  cdef: damping=%d bits=%d y=", c->cdef_damping, c->cdef_bits); for (int i = 0; i < 8; i++) printf("%d/%d,", c->cdef_y_pri_strength[i], c->cdef_y_sec_strength[i]); printf(" uv="); for (int i = 0; i < 8; i++) printf("%d/%d,", c->cdef_uv_pri_strength[i], c->cdef_uv_sec_strength[i]); printf("\n");
    const GstAV1LoopRestorationParams *r = &fh->loop_restoration_params;
    printf("  lr: uses=%d type=%d,%d,%d size=%d,%d,%d\n", r->uses_lr, r->frame_restoration_type[0], r->frame_restoration_type[1], r->frame_restoration_type[2], r->loop_restoration_size[0], r->loop_restoration_size[1], r->loop_restoration_size[2]);
    const GstAV1TileInfo *t = &fh->tile_info;
    printf("  tiles: uniform=%d cols=%d rows=%d log2=%d,%d ctx_upd=%d size_bytes=%d col_start=", t->uniform_tile_spacing_flag, t->tile_cols, t->tile_rows, t->tile_cols_log2, t->tile_rows_log2, t->context_update_tile_id, t->tile_size_bytes_minus_1 + 1);
    for (int i = 0; i <= t->tile_cols && i < 65; i++) printf("%d,", t->mi_col_starts[i]); printf(" row_start="); for (int i = 0; i <= t->tile_rows && i < 65; i++) printf("%d,", t->mi_row_starts[i]); printf("\n");
    const GstAV1GlobalMotionParams *g = &fh->global_motion_params;
    printf("  gm:"); for (int i = 1; i < 8; i++) { printf(" [%d]t=%d", i, g->gm_type[i]); if (g->gm_type[i]) { printf(":"); for (int j = 0; j < 6; j++) printf("%d,", g->gm_params[i][j]); } } printf("\n");
    const GstAV1FilmGrainParams *f = &fh->film_grain_params;
    printf("  fg: apply=%d seed=%d update=%d ny=%d\n", f->apply_grain, f->grain_seed, f->update_grain, f->num_y_points);
}
int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"); uint8_t hdr[32]; int ivf = 0; if (!in) { perror("open"); return 1; }
    if (fread(hdr, 1, 32, in) == 32 && !memcmp(hdr, "DKIF", 4)) ivf = 1; else fseek(in, 0, SEEK_SET);
    GstAV1Parser *p = gst_av1_parser_new(); GstAV1SequenceHeaderOBU seq; int fidx = 0;
    uint8_t *buf = malloc(16 << 20); size_t sz = 0;
    if (!ivf) sz = fread(buf, 1, 16 << 20, in);
    for (;;) {
        if (ivf) { uint8_t fh[12]; if (fread(fh, 1, 12, in) != 12) break; sz = fh[0] | fh[1] << 8 | fh[2] << 16 | (uint32_t)fh[3] << 24; if (fread(buf, 1, sz, in) != sz) break; }
        size_t pos = 0;
        while (pos < sz) {
            GstAV1OBU obu; guint32 consumed = 0;
            GstAV1ParserResult r = gst_av1_parser_identify_one_obu(p, buf + pos, sz - pos, &obu, &consumed);
            if (r != GST_AV1_PARSER_OK) { if (r == GST_AV1_PARSER_DROP && consumed) { pos += consumed; continue; } fprintf(stderr, "identify failed %d at %zu\n", r, pos); return 1; }
            if (obu.obu_type == GST_AV1_OBU_SEQUENCE_HEADER) {
                gst_av1_parser_parse_sequence_header_obu(p, &obu, &seq);
                printf("SEQ profile=%u still=%u reduced=%u max=%ux%u order_hint_bits=%u sb128=%u filter_intra=%u intra_edge=%u interintra=%u masked=%u warped=%u dual=%u order_hint=%u jnt=%u ref_mvs=%u sf=%u/%u/%u superres=%u cdef=%u lr=%u fg=%u bits=%u mono=%u range=%u ss=%u,%u csp=%u sep_delta_q=%u\n",
                    seq.seq_profile, seq.still_picture, seq.reduced_still_picture_header, seq.max_frame_width_minus_1 + 1, seq.max_frame_height_minus_1 + 1, seq.order_hint_bits_minus_1 + 1, seq.use_128x128_superblock, seq.enable_filter_intra, seq.enable_intra_edge_filter, seq.enable_interintra_compound, seq.enable_masked_compound, seq.enable_warped_motion, seq.enable_dual_filter, seq.enable_order_hint, seq.enable_jnt_comp, seq.enable_ref_frame_mvs, seq.seq_force_screen_content_tools, seq.seq_force_integer_mv, seq.seq_choose_screen_content_tools, seq.enable_superres, seq.enable_cdef, seq.enable_restoration, seq.film_grain_params_present, seq.bit_depth, seq.color_config.mono_chrome, seq.color_config.color_range, seq.color_config.subsampling_x, seq.color_config.subsampling_y, seq.color_config.chroma_sample_position, seq.color_config.separate_uv_delta_q);
            } else if (obu.obu_type == GST_AV1_OBU_FRAME || obu.obu_type == GST_AV1_OBU_FRAME_HEADER) {
                GstAV1FrameOBU fr; GstAV1FrameHeaderOBU fhd, *fh;
                if (obu.obu_type == GST_AV1_OBU_FRAME) { if (gst_av1_parser_parse_frame_obu(p, &obu, &fr) != GST_AV1_PARSER_OK) { fprintf(stderr, "frame %d parse failed\n", fidx); return 1; } fh = &fr.frame_header; }
                else { if (gst_av1_parser_parse_frame_header_obu(p, &obu, &fhd) != GST_AV1_PARSER_OK) { fprintf(stderr, "frame hdr %d parse failed\n", fidx); return 1; } fh = &fhd; }
                dump(fh, fidx); gst_av1_parser_reference_frame_update(p, fh); fidx++;
            }
            pos += consumed;
        }
        if (!ivf) break;
    }
    return 0;
}
