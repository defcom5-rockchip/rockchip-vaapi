/*
 * av1rt.c — round-trip test for the vendored AV1 OBU writer (experiment 2, our port).
 * For every sequence header and frame header in an IVF file: parse it with GStreamer's AV1 parser (system
 * libgstcodecparsers 1.28.2), re-write it with rockchip-vaapi's vendored gstav1bitwriter, and compare the bytes with
 * the original OBU. A frame header written by us is an OBU_FRAME_HEADER; when the original was an OBU_FRAME the
 * comparison is against the leading bytes of its payload (frame header + byte alignment, before the tile group).
 *
 * Build on the board (from rockchip-vaapi/src/av1):
 *   gcc -O1 -g -DGST_USE_UNSTABLE_API -I. -o av1rt av1rt.c gstav1bitwriter.c gstcompat.c \
 *       $(pkg-config --libs gstreamer-codecparsers-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gstav1bitwriter.h"

/* SavedGmParams per reference slot (spec 7.20/7.21): what the decoder uses as PrevGmParams via primary_ref_frame */
static int32_t saved_gm[8][8][6]; static int saved_gm_init = 0;
static void gm_defaults(int32_t gm[8][6]) { for (int r = 0; r < 8; r++) for (int i = 0; i < 6; i++) gm[r][i] = (i % 3 == 2) ? (1 << 16) : 0; }
static void gm_prepare(GstAV1FrameHeaderOBU *fh) {   /* fill ref_global_motion_params from the primary reference's saved params */
    if (!saved_gm_init) { for (int s = 0; s < 8; s++) gm_defaults(saved_gm[s]); saved_gm_init = 1; }
    if (fh->primary_ref_frame == GST_AV1_PRIMARY_REF_NONE || fh->frame_is_intra) gm_defaults(fh->ref_global_motion_params.gm_params);
    else memcpy(fh->ref_global_motion_params.gm_params, saved_gm[fh->ref_frame_idx[fh->primary_ref_frame]], sizeof saved_gm[0]);
}
static void gm_save(const GstAV1FrameHeaderOBU *fh) {   /* after the frame: slots it refreshes get its gm params (identity for intra frames) */
    int32_t cur[8][6]; if (fh->frame_is_intra) gm_defaults(cur); else memcpy(cur, fh->global_motion_params.gm_params, sizeof cur);
    unsigned m = (fh->frame_type == GST_AV1_KEY_FRAME && fh->show_frame) ? 0xff : fh->refresh_frame_flags;
    for (int b = 0; b < 8; b++) if (m & (1u << b)) memcpy(saved_gm[b], cur, sizeof cur);
}

static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) { *v = 0; for (size_t i = 0; i < 8 && i < n; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (7 * i); if (!(p[i] & 0x80)) return i + 1; } return 0; }
/* payload pointer/length of an OBU we wrote (has_size_field = 1) */
static const uint8_t *payload_of(const uint8_t *obu, size_t len, size_t *plen) {
    int ext = (obu[0] >> 2) & 1; size_t off = 1 + ext; uint64_t sz; size_t l = leb128(obu + off, len - off, &sz); *plen = sz; return obu + off + l;
}
/* number of meaningful header bits in our OBU_FRAME_HEADER payload: everything before the trailing 1 bit */
static size_t header_bits(const uint8_t *p, size_t len) {
    for (size_t i = len; i-- > 0;) if (p[i]) { int b = 0; while (!(p[i] & (1u << b))) b++; return i * 8 + (7 - b); }
    return 0;
}
static int bits_equal(const uint8_t *a, const uint8_t *b, size_t nbits, size_t *bad) {
    size_t full = nbits / 8; if (memcmp(a, b, full)) { for (size_t i = 0; i < full; i++) if (a[i] != b[i]) { *bad = i * 8; return 0; } }
    int rem = nbits % 8; if (rem) { uint8_t m = (uint8_t)(0xff << (8 - rem)); if ((a[full] ^ b[full]) & m) { *bad = full * 8; return 0; } }
    return 1;
}
static void first_diff(const uint8_t *a, const uint8_t *b, size_t n, size_t *byte, int *bit) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) { *byte = i; uint8_t x = a[i] ^ b[i]; for (*bit = 0; !(x & (0x80 >> *bit)); (*bit)++); return; }
    *byte = n; *bit = -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: av1rt in.ivf [verbose]\n"); return 2; }
    int verbose = argc > 2;
    FILE *in = fopen(argv[1], "rb"); if (!in) { perror("open"); return 1; }
    uint8_t hdr[32]; if (fread(hdr, 1, 32, in) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not IVF\n"); return 1; }
    GstAV1Parser *parser = gst_av1_parser_new();
    GstAV1SequenceHeaderOBU seq; int have_seq = 0;
    uint8_t *buf = malloc(4 << 20), *out = malloc(1 << 20);
    int seq_total = 0, seq_ok = 0, fh_total = 0, fh_ok = 0, fh_prefix_short = 0, tu = 0, errs = 0;
    for (;;) {
        uint8_t fh12[12]; if (fread(fh12, 1, 12, in) != 12) break;
        uint32_t sz = fh12[0] | fh12[1] << 8 | fh12[2] << 16 | (uint32_t)fh12[3] << 24;
        if (fread(buf, 1, sz, in) != sz) break;
        tu++;
        uint32_t pos = 0;
        while (pos < sz) {
            GstAV1OBU obu; guint32 consumed = 0;
            GstAV1ParserResult r = gst_av1_parser_identify_one_obu(parser, buf + pos, sz - pos, &obu, &consumed);
            if (r != GST_AV1_PARSER_OK) { if (r == GST_AV1_PARSER_DROP && consumed) { pos += consumed; continue; } fprintf(stderr, "tu %d: identify failed %d at %u\n", tu, r, pos); errs++; break; }
            const uint8_t *orig = buf + pos; size_t orig_len = consumed;
            if (obu.obu_type == GST_AV1_OBU_SEQUENCE_HEADER) {
                r = gst_av1_parser_parse_sequence_header_obu(parser, &obu, &seq);
                if (r != GST_AV1_PARSER_OK) { fprintf(stderr, "seq parse failed %d\n", r); errs++; }
                else {
                    have_seq = 1; seq_total++; guint osz = 1 << 20;
                    if (gst_av1_bit_writer_sequence_header_obu(&seq, TRUE, out, &osz) != GST_AV1_BIT_WRITER_OK) { fprintf(stderr, "seq write failed\n"); errs++; }
                    else if (osz == orig_len && !memcmp(out, orig, osz)) seq_ok++;
                    else { size_t b; int bit; first_diff(out, orig, osz < orig_len ? osz : orig_len, &b, &bit);
                        fprintf(stderr, "tu %d: SEQUENCE HEADER differs: ours %u B, orig %zu B, first diff byte %zu bit %d\n", tu, osz, orig_len, b, bit); }
                }
            } else if (obu.obu_type == GST_AV1_OBU_FRAME || obu.obu_type == GST_AV1_OBU_FRAME_HEADER) {
                GstAV1FrameHeaderOBU fhd; GstAV1FrameOBU frame; const GstAV1FrameHeaderOBU *fp;
                if (obu.obu_type == GST_AV1_OBU_FRAME) { r = gst_av1_parser_parse_frame_obu(parser, &obu, &frame); fp = &frame.frame_header; }
                else { r = gst_av1_parser_parse_frame_header_obu(parser, &obu, &fhd); fp = &fhd; }
                if (r != GST_AV1_PARSER_OK) { fprintf(stderr, "tu %d: frame header parse failed %d\n", tu, r); errs++; }
                else if (have_seq) {
                    fh_total++; guint osz = 1 << 20;
                    if (!fp->show_existing_frame) gm_prepare((GstAV1FrameHeaderOBU *)fp);
                    GstAV1BitWriterResult w = gst_av1_bit_writer_frame_header_obu(fp, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, TRUE, out, &osz);
                    if (w != GST_AV1_BIT_WRITER_OK) { fprintf(stderr, "tu %d: frame header write failed %d (type %u show_existing %u)\n", tu, w, fp->frame_type, fp->show_existing_frame); errs++; }
                    else {
                        size_t plen; const uint8_t *p = payload_of(out, osz, &plen);          /* our frame-header payload */
                        const uint8_t *q = obu.data; size_t qlen = obu.obu_size;              /* original payload (frame hdr [+ tile group]) */
                        size_t nb = header_bits(p, plen), bad = 0; int ok = (nb / 8 < qlen) && bits_equal(p, q, nb, &bad);
                        if (ok) fh_ok++;
                        else { fprintf(stderr, "tu %d: FRAME HEADER differs (type %u, show_frame %u, show_existing %u, ours %zu header bits vs orig payload %zu B): first diff at bit %zu\n",
                                    tu, fp->frame_type, fp->show_frame, fp->show_existing_frame, nb, qlen, bad);
                            if (verbose) {
                                guint qo = 0, so = 0, lo = 0, co = 0, cs = 0, osz2 = 1 << 20; static uint8_t tmp2[1 << 20];
                                gst_av1_bit_writer_frame_header_obu_with_offsets(fp, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, TRUE, 0, &qo, &so, &lo, &co, &cs, tmp2, &osz2);
                                { int ext = (tmp2[0] >> 2) & 1; uint64_t v; size_t l = leb128(tmp2 + 1 + ext, 8, &v); guint hdrbits = (1 + ext + l) * 8; qo -= hdrbits; so -= hdrbits; lo -= hdrbits; co -= hdrbits; }
                                fprintf(stderr, "  section offsets (bits, payload-relative): qindex %u segmentation %u loopfilter %u cdef %u (size %u)\n", qo, so, lo, co, cs);
                                fprintf(stderr, "  fields: primary_ref %u refresh 0x%02x order_hint %u override %u superres %u render_diff %u intrabc %u hpmv %u filt_sw %u interp %u mm_sw %u refmvs %u fe_cdf %u | tiles uniform %u cols_log2 %u rows_log2 %u ctx_tile %u tsb-1 %u | q base %u dq %u diffuv %u qm %u | seg en %u map %u temp %u data %u | dlf %u dq_present %u | lf lvl %u/%u/%u/%u sharp %u den %u dup %u | cdef_bits %u | lr %u/%u/%u | txmode %u refsel %u skipmode %u (%u,%u) warped %u redtx %u | gm types %u%u%u%u%u%u%u | grain %u\n",
                                    fp->primary_ref_frame, fp->refresh_frame_flags, fp->order_hint, fp->frame_size_override_flag, fp->use_superres, fp->render_and_frame_size_different, fp->allow_intrabc, fp->allow_high_precision_mv, fp->is_filter_switchable, fp->interpolation_filter, fp->is_motion_mode_switchable, fp->use_ref_frame_mvs, fp->disable_frame_end_update_cdf,
                                    fp->tile_info.uniform_tile_spacing_flag, fp->tile_info.tile_cols_log2, fp->tile_info.tile_rows_log2, fp->tile_info.context_update_tile_id, fp->tile_info.tile_size_bytes_minus_1,
                                    fp->quantization_params.base_q_idx, fp->quantization_params.delta_q_present, fp->quantization_params.diff_uv_delta, fp->quantization_params.using_qmatrix,
                                    fp->segmentation_params.segmentation_enabled, fp->segmentation_params.segmentation_update_map, fp->segmentation_params.segmentation_temporal_update, fp->segmentation_params.segmentation_update_data,
                                    fp->loop_filter_params.delta_lf_present, fp->quantization_params.delta_q_present,
                                    fp->loop_filter_params.loop_filter_level[0], fp->loop_filter_params.loop_filter_level[1], fp->loop_filter_params.loop_filter_level[2], fp->loop_filter_params.loop_filter_level[3], fp->loop_filter_params.loop_filter_sharpness, fp->loop_filter_params.loop_filter_delta_enabled, fp->loop_filter_params.loop_filter_delta_update,
                                    fp->cdef_params.cdef_bits, fp->loop_restoration_params.frame_restoration_type[0], fp->loop_restoration_params.frame_restoration_type[1], fp->loop_restoration_params.frame_restoration_type[2],
                                    fp->tx_mode, fp->reference_select, fp->skip_mode_present, fp->skip_mode_frame[0], fp->skip_mode_frame[1], fp->allow_warped_motion, fp->reduced_tx_set,
                                    fp->global_motion_params.gm_type[1], fp->global_motion_params.gm_type[2], fp->global_motion_params.gm_type[3], fp->global_motion_params.gm_type[4], fp->global_motion_params.gm_type[5], fp->global_motion_params.gm_type[6], fp->global_motion_params.gm_type[7], fp->film_grain_params.apply_grain);
                                for (int r = 1; r < 8; r++) fprintf(stderr, "  gm ref%d: type %u params %d %d %d %d %d %d | prev %d %d %d %d %d %d\n", r, fp->global_motion_params.gm_type[r],
                                    fp->global_motion_params.gm_params[r][0], fp->global_motion_params.gm_params[r][1], fp->global_motion_params.gm_params[r][2], fp->global_motion_params.gm_params[r][3], fp->global_motion_params.gm_params[r][4], fp->global_motion_params.gm_params[r][5],
                                    fp->ref_global_motion_params.gm_params[r][0], fp->ref_global_motion_params.gm_params[r][1], fp->ref_global_motion_params.gm_params[r][2], fp->ref_global_motion_params.gm_params[r][3], fp->ref_global_motion_params.gm_params[r][4], fp->ref_global_motion_params.gm_params[r][5]);
                                { /* variants: which element explains the extra bit? */
                                  const char *names[] = {"no-skip-mode-bit", "gm LAST=AFFINE", "gm all IDENTITY", "warped=0", "reduced_tx_set=0", "refsel=0", "txmode=LARGEST", "prev=identity", "prev=self", "prev=zeros"};
                                  for (int vi = 0; vi < 10; vi++) {
                                    GstAV1FrameHeaderOBU v = *fp; uint8_t vb[1 << 16]; guint vs = sizeof vb;
                                    if (vi == 0) { v.skip_mode_frame[0] = v.skip_mode_frame[1] = 0; }
                                    if (vi == 1) { v.global_motion_params.gm_type[1] = GST_AV1_WARP_MODEL_AFFINE; }
                                    if (vi == 2) { for (int r = 0; r < 8; r++) v.global_motion_params.gm_type[r] = GST_AV1_WARP_MODEL_IDENTITY; }
                                    if (vi == 3) v.allow_warped_motion = 0;
                                    if (vi == 4) v.reduced_tx_set = 0;
                                    if (vi == 5) v.reference_select = 0;
                                    if (vi == 6) v.tx_mode = GST_AV1_TX_MODE_LARGEST;
                                    if (vi == 7) gm_defaults(v.ref_global_motion_params.gm_params);
                                    if (vi == 8) memcpy(v.ref_global_motion_params.gm_params, v.global_motion_params.gm_params, sizeof v.global_motion_params.gm_params);
                                    if (vi == 9) memset(v.ref_global_motion_params.gm_params, 0, sizeof v.ref_global_motion_params.gm_params);
                                    if (gst_av1_bit_writer_frame_header_obu(&v, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, TRUE, vb, &vs) != GST_AV1_BIT_WRITER_OK) { fprintf(stderr, "  variant %s: write failed\n", names[vi]); continue; }
                                    size_t vl; const uint8_t *vp = payload_of(vb, vs, &vl); size_t vnb = header_bits(vp, vl), vbad = 0;
                                    int vok = bits_equal(vp, q, vnb < qlen * 8 ? vnb : qlen * 8, &vbad);
                                    fprintf(stderr, "  variant %-18s: %zu header bits, %s%zu | bytes16-23:", names[vi], vnb, vok ? "MATCHES all header bits up to " : "first diff at bit ", vok ? vnb : vbad);
                                    for (size_t bi = 16; bi < 24 && bi < vl; bi++) fprintf(stderr, " %02x", vp[bi]); fprintf(stderr, "\n");
                                  }
                                }
                                fprintf(stderr, "  ours:"); for (size_t i = 0; i < plen && i < 24; i++) fprintf(stderr, " %02x", p[i]); fprintf(stderr, "\n  orig:"); for (size_t i = 0; i < qlen && i < 24; i++) fprintf(stderr, " %02x", q[i]); fprintf(stderr, "\n"); }
                        }
                    }
                    /* keep the parser's reference state in step */
                    if (!fp->show_existing_frame) gm_save(fp); else if (fp->frame_type == GST_AV1_KEY_FRAME) { for (int b = 0; b < 8; b++) memcpy(saved_gm[b], saved_gm[fp->frame_to_show_map_idx], sizeof saved_gm[0]); }
                    gst_av1_parser_reference_frame_update(parser, (GstAV1FrameHeaderOBU *)fp);
                }
            }
            pos += consumed;
        }
    }
    printf("temporal_units=%d sequence_headers=%d/%d frame_headers=%d/%d bit-exact%s errors=%d\n", tu, seq_ok, seq_total, fh_ok, fh_total,
           "", errs);
    gst_av1_parser_free(parser);
    return (seq_ok == seq_total && fh_ok == fh_total && !errs) ? 0 : 1;
}
