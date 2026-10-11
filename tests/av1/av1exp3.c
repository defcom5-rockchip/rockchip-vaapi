/*
 * av1exp3.c — experiments 3 and 4 for the AV1 driver design (refresh_frame_flags deferral; show_existing of a KEY frame).
 *
 * Re-serialises every OBU_FRAME of an IVF stream as the driver will: frame header bits written by our vendored writer
 * (from the parsed GstAV1FrameHeaderOBU), byte-aligned, followed by the original tile bytes, one OBU_FRAME per MPP
 * packet; the sequence header goes once as its own packet; TD/metadata are dropped.
 *
 *   av1exp3 [--mask0 all|N[,N..]] [--out out.nv12] [--report] in.ivf
 *
 *   --mask0 all   every non-key frame with refresh_frame_flags != 0 is submitted twice: first with
 *                 refresh_frame_flags = 0 (the "forced early decode" of the design), then again with its real mask;
 *                 the duplicate output of the second submission is dropped. The output stream must still be
 *                 bit-exact against dav1d: proves the mask-0 picture is right AND MPP's slot state after the re-submit
 *                 equals the normal path.
 *   --mask0 N     only frame N (0-based frame-header index) gets the treatment.
 *   --report      print per-frame header facts (type, show_frame, show_existing, refresh mask, order_hint).
 *
 * Build on the board (from rockchip-vaapi/src/av1 with av1exp3.c copied there):
 *   gcc -O1 -g -DGST_USE_UNSTABLE_API -I. -o av1exp3 av1exp3.c gstav1bitwriter.c gstcompat.c \
 *       $(pkg-config --libs gstreamer-codecparsers-1.0) $(pkg-config --cflags --libs rockchip_mpp)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "gstav1bitwriter.h"
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

/* ---------------- MPP side (as av1mpp.c) ---------------- */
static FILE *out, *idx; static int frames_out, dropped, info_changes, drop_next; static long long cur_pts = -1; static int w, h, hs, vs, fmt10;
static void dump_frame(MppFrame f) {
    MppBuffer buf = mpp_frame_get_buffer(f); if (!buf) return;
    uint8_t *p = mpp_buffer_get_ptr(buf);
    w = mpp_frame_get_width(f); h = mpp_frame_get_height(f); hs = mpp_frame_get_hor_stride(f); vs = mpp_frame_get_ver_stride(f);
    fmt10 = ((mpp_frame_get_fmt(f) & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP_10BIT);
    if (drop_next) { drop_next = 0; dropped++; return; }
    frames_out++;
    if (idx) fprintf(idx, "%lld\n", (long long)mpp_frame_get_pts(f));
    if (!out) return;
    int rowb = fmt10 ? hs : w;
    for (int y = 0; y < h; y++) fwrite(p + (size_t)y * hs, 1, rowb, out);
    uint8_t *uv = p + (size_t)hs * vs;
    for (int y = 0; y < h / 2; y++) fwrite(uv + (size_t)y * hs, 1, rowb, out);
}
static int drain(MppCtx ctx, MppApi *mpi, int wait_ms) {
    MppFrame f = NULL; int got = 0;
    for (;;) {
        MPP_RET r = mpi->decode_get_frame(ctx, &f);
        if (r != MPP_OK || !f) { if (wait_ms > 0) { usleep(1000); wait_ms--; continue; } break; }
        if (mpp_frame_get_info_change(f)) { info_changes++; mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL); }
        else if (mpp_frame_get_eos(f)) { mpp_frame_deinit(&f); return -1; }
        else { dump_frame(f); got++; }
        mpp_frame_deinit(&f); f = NULL;
    }
    return got;
}
static int put(MppCtx ctx, MppApi *mpi, const uint8_t *data, size_t len) {
    MppPacket pkt; mpp_packet_init(&pkt, (void *)data, len); mpp_packet_set_pts(pkt, cur_pts); int tries = 0; MPP_RET r;
    while ((r = mpi->decode_put_packet(ctx, pkt)) != MPP_OK) { if (++tries > 4000) { fprintf(stderr, "put failed %d\n", r); mpp_packet_deinit(&pkt); return -1; } drain(ctx, mpi, 0); usleep(500); }
    mpp_packet_deinit(&pkt); drain(ctx, mpi, 0); return 0;
}
/* wait until the decoder has produced output for a shown frame (or 300 ms pass) */
static void settle(MppCtx ctx, MppApi *mpi, int expect_output) {
    int before = frames_out + dropped;
    for (int i = 0; i < 300; i++) { drain(ctx, mpi, 0); if (!expect_output || frames_out + dropped > before) break; usleep(1000); }
}

/* ---------------- OBU helpers ---------------- */
static size_t leb128_write(uint8_t *p, uint64_t v) { size_t n = 0; do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; p[n++] = b; } while (v); return n; }
static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) { *v = 0; for (size_t i = 0; i < 8 && i < n; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (7 * i); if (!(p[i] & 0x80)) return i + 1; } return 0; }
static size_t header_bits(const uint8_t *p, size_t len) { for (size_t i = len; i-- > 0;) if (p[i]) { int b = 0; while (!(p[i] & (1u << b))) b++; return i * 8 + (7 - b); } return 0; }

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

/* One tile's payload as the driver receives it from VA-API: a span of bytes, no size field. */
typedef struct { const uint8_t *data; size_t len; } Tile;

/* Build an OBU_FRAME packet: header bits from our writer (tile_size_bytes forced to 4), byte-aligned, then every tile
 * except the last prefixed by its 4-byte little-endian size (tile_start_and_end_present_flag = 0: one group, all tiles). */
static size_t build_frame_obu(const GstAV1FrameHeaderOBU *fh_in, const GstAV1SequenceHeaderOBU *seq, uint8_t tid, uint8_t sid,
                              const Tile *tiles, int ntiles, uint8_t *dst, size_t dst_cap, size_t *hdr_bits_out) {
    static uint8_t tmp[1 << 16]; guint tsz = sizeof(tmp);
    GstAV1FrameHeaderOBU fh = *fh_in; fh.tile_info.tile_size_bytes_minus_1 = 3; fh.tile_info.tile_size_bytes = 4;
    if (gst_av1_bit_writer_frame_header_obu(&fh, seq, tid, sid, TRUE, tmp, &tsz) != GST_AV1_BIT_WRITER_OK) return 0;
    int ext = (tmp[0] >> 2) & 1; size_t off = 1 + ext; uint64_t psz; size_t l = leb128(tmp + off, tsz - off, &psz);
    const uint8_t *p = tmp + off + l; size_t nb = header_bits(p, psz), hb = (nb + 7) / 8;
    if (hdr_bits_out) *hdr_bits_out = nb;
    /* tile_group_obu(): with NumTiles > 1 a tile_start_and_end_present_flag bit (0) + byte_alignment() precede the tiles = one zero byte */
    size_t tg_hdr = ntiles > 1 ? 1 : 0;
    size_t payload = hb + tg_hdr; for (int i = 0; i < ntiles; i++) payload += tiles[i].len + (i + 1 < ntiles ? 4 : 0);
    if (payload + 16 > dst_cap) return 0;
    size_t n = 0;
    dst[n++] = (uint8_t)((6 << 3) | (ext << 2) | 0x02);                /* OBU_FRAME, has_size_field */
    if (ext) dst[n++] = tmp[1];
    n += leb128_write(dst + n, payload);
    memcpy(dst + n, p, hb); if (nb % 8) dst[n + hb - 1] &= (uint8_t)(0xff << (8 - nb % 8)); n += hb;   /* byte_alignment(): zeros */
    if (tg_hdr) dst[n++] = 0;
    for (int i = 0; i < ntiles; i++) {
        if (i + 1 < ntiles) { uint32_t sz1 = (uint32_t)tiles[i].len - 1; dst[n++] = sz1; dst[n++] = sz1 >> 8; dst[n++] = sz1 >> 16; dst[n++] = sz1 >> 24; }
        memcpy(dst + n, tiles[i].data, tiles[i].len); n += tiles[i].len;
    }
    return n;
}

/* a FRAME_HEADER OBU with show_existing_frame = 1 for slot `slot`, written by our writer */
static size_t build_show_existing(const GstAV1FrameHeaderOBU *like, const GstAV1SequenceHeaderOBU *seq, int slot, uint8_t *dst, size_t cap) {
    GstAV1FrameHeaderOBU se; memset(&se, 0, sizeof se);
    se.show_existing_frame = 1; se.frame_to_show_map_idx = slot; se.frame_type = like->frame_type; se.show_frame = 1;
    guint sz = cap; if (gst_av1_bit_writer_frame_header_obu(&se, seq, 0, 0, TRUE, dst, &sz) != GST_AV1_BIT_WRITER_OK) return 0; return sz;
}
int main(int argc, char **argv) {
    const char *outp = NULL, *mask0 = NULL; int report = 0, sekey = 0, hidden_se = 0, i;
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--out")) outp = argv[++i]; else if (!strcmp(argv[i], "--mask0")) mask0 = argv[++i];
        else if (!strcmp(argv[i], "--report")) report = 1; else if (!strcmp(argv[i], "--sekey")) sekey = 1; else if (!strcmp(argv[i], "--hidden-se")) hidden_se = 1; else break;
    }
    if (i != argc - 1) { fprintf(stderr, "usage: av1exp3 [--mask0 all|N,..] [--sekey] [--hidden-se] [--out f] [--report] in.ivf\n"); return 2; }
    FILE *in = fopen(argv[i], "rb"); if (!in) { perror("open"); return 1; }
    uint8_t ivf[32]; if (fread(ivf, 1, 32, in) != 32 || memcmp(ivf, "DKIF", 4)) { fprintf(stderr, "not IVF\n"); return 1; }
    if (outp) out = fopen(outp, "wb");
    int mask_all = mask0 && !strcmp(mask0, "all");

    MppCtx ctx; MppApi *mpi; MppDecCfg cfg;
    if (mpp_create(&ctx, &mpi) || mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAV1)) { fprintf(stderr, "mpp init failed\n"); return 1; }
    mpp_dec_cfg_init(&cfg); mpi->control(ctx, MPP_DEC_GET_CFG, cfg); mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1); mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    RK_U32 block = 0; mpi->control(ctx, MPP_SET_OUTPUT_BLOCK, &block);

    GstAV1Parser *parser = gst_av1_parser_new(); GstAV1SequenceHeaderOBU seq; int have_seq = 0;
    uint8_t *buf = malloc(8 << 20), *pkt = malloc(8 << 20), *pkt2 = malloc(8 << 20);
    int tu = 0, fidx = 0, twice = 0, passthrough = 0, se_key = 0, hidden_shown = 0; long long slot_pts[8] = {-1,-1,-1,-1,-1,-1,-1,-1}; FILE *expf = fopen("exp.txt", "w"); idx = fopen("out.idx", "w");
    for (;;) {
        uint8_t fh12[12]; if (fread(fh12, 1, 12, in) != 12) break;
        uint32_t sz = fh12[0] | fh12[1] << 8 | fh12[2] << 16 | (uint32_t)fh12[3] << 24;
        if (fread(buf, 1, sz, in) != sz) break;
        tu++; uint32_t pos = 0;
        while (pos < sz) {
            GstAV1OBU obu; guint32 consumed = 0;
            GstAV1ParserResult r = gst_av1_parser_identify_one_obu(parser, buf + pos, sz - pos, &obu, &consumed);
            if (r != GST_AV1_PARSER_OK) { if (r == GST_AV1_PARSER_DROP && consumed) { pos += consumed; continue; } fprintf(stderr, "identify failed %d\n", r); break; }
            if (obu.obu_type == GST_AV1_OBU_SEQUENCE_HEADER) {
                if (gst_av1_parser_parse_sequence_header_obu(parser, &obu, &seq) == GST_AV1_PARSER_OK) { if (!have_seq) { put(ctx, mpi, buf + pos, consumed); } have_seq = 1; }
            } else if ((obu.obu_type == GST_AV1_OBU_FRAME || obu.obu_type == GST_AV1_OBU_FRAME_HEADER || obu.obu_type == GST_AV1_OBU_TILE_GROUP) && have_seq) {
                /* collect header + tiles; emit when the last tile of the frame has arrived */
                static GstAV1FrameHeaderOBU cur_fh; static int have_fh = 0, ntiles = 0, total_tiles = 0; static Tile tiles[GST_AV1_MAX_TILE_COUNT]; static uint8_t ctid, csid;
                GstAV1TileGroupOBU *tg = NULL; GstAV1FrameOBU frame; GstAV1TileGroupOBU tgo;
                if (obu.obu_type == GST_AV1_OBU_FRAME) {
                    if (gst_av1_parser_parse_frame_obu(parser, &obu, &frame) != GST_AV1_PARSER_OK) { fprintf(stderr, "tu %d: frame parse failed\n", tu); pos += consumed; continue; }
                    cur_fh = frame.frame_header; have_fh = 1; ntiles = 0; tg = &frame.tile_group; ctid = obu.header.obu_temporal_id; csid = obu.header.obu_spatial_id;
                } else if (obu.obu_type == GST_AV1_OBU_FRAME_HEADER) {
                    GstAV1FrameHeaderOBU fh;
                    if (gst_av1_parser_parse_frame_header_obu(parser, &obu, &fh) != GST_AV1_PARSER_OK) { fprintf(stderr, "tu %d: frame header parse failed\n", tu); pos += consumed; continue; }
                    if (fh.show_existing_frame) {
                        if (fh.frame_type == GST_AV1_KEY_FRAME) se_key++;
                        if (report) fprintf(stderr, "frame %d (tu %d): show_existing slot %u (type %u)\n", fidx, tu, fh.frame_to_show_map_idx, fh.frame_type);
                        if (expf) fprintf(expf, "%lld\n", slot_pts[fh.frame_to_show_map_idx]);
                        if (fh.frame_type == GST_AV1_KEY_FRAME) { for (int b = 0; b < 8; b++) { slot_pts[b] = slot_pts[fh.frame_to_show_map_idx]; memcpy(saved_gm[b], saved_gm[fh.frame_to_show_map_idx], sizeof saved_gm[0]); } }
                        gst_av1_parser_reference_frame_update(parser, &fh); fidx++;
                        if (!(hidden_se && fh.frame_type != GST_AV1_KEY_FRAME)) { cur_pts = fidx - 1; put(ctx, mpi, buf + pos, consumed); passthrough++; }
                        pos += consumed; continue;
                    }
                    cur_fh = fh; have_fh = 1; ntiles = 0; ctid = obu.header.obu_temporal_id; csid = obu.header.obu_spatial_id;
                } else { /* TILE_GROUP */
                    if (!have_fh || gst_av1_parser_parse_tile_group_obu(parser, &obu, &tgo) != GST_AV1_PARSER_OK) { fprintf(stderr, "tu %d: tile group parse failed\n", tu); pos += consumed; continue; }
                    tg = &tgo;
                }
                if (tg) {
                    total_tiles = tg->num_tiles;
                    for (unsigned t = tg->tg_start; t <= tg->tg_end && t < GST_AV1_MAX_TILE_COUNT; t++) { tiles[ntiles].data = obu.data + tg->entry[t].tile_offset; tiles[ntiles].len = tg->entry[t].tile_size; ntiles++; }
                    if (tg->tg_end + 1 < total_tiles) { pos += consumed; continue; }   /* more tile groups follow */
                }
                if (!have_fh || ntiles == 0) { pos += consumed; continue; }
                /* ---- a complete frame: header + all tiles ---- */
                GstAV1FrameHeaderOBU *fh = &cur_fh; cur_pts = fidx; have_fh = 0;
                gm_prepare(fh);
                int do_mask0 = 0;
                if (mask0 && fh->frame_type != GST_AV1_KEY_FRAME && fh->refresh_frame_flags) {
                    if (mask_all) do_mask0 = 1; else { char s2[16]; snprintf(s2, sizeof s2, "%d", fidx); const char *q = mask0; size_t ls = strlen(s2);
                        while ((q = strstr(q, s2))) { if ((q == mask0 || q[-1] == ',') && (q[ls] == 0 || q[ls] == ',')) { do_mask0 = 1; break; } q += ls; } }
                }
                if (report || do_mask0) fprintf(stderr, "frame %d (tu %d): type %u show %u refresh 0x%02x order_hint %u tiles %d%s\n", fidx, tu, fh->frame_type, fh->show_frame,
                                           fh->refresh_frame_flags, fh->order_hint, ntiles, do_mask0 ? "  <- mask0 then real" : "");
                size_t n;
                if (do_mask0) {
                    GstAV1FrameHeaderOBU fh0 = *fh; fh0.refresh_frame_flags = 0;
                    size_t n0 = build_frame_obu(&fh0, &seq, ctid, csid, tiles, ntiles, pkt2, 8 << 20, NULL);
                    if (!n0) fprintf(stderr, "mask0 rewrite failed at frame %d\n", fidx);
                    else { put(ctx, mpi, pkt2, n0); settle(ctx, mpi, fh->show_frame); twice++; drop_next = fh->show_frame; }
                }
                if (sekey && fh->frame_type == GST_AV1_KEY_FRAME && fh->show_frame && fidx > 0) {
                    GstAV1FrameHeaderOBU hk = *fh; hk.show_frame = 0; hk.showable_frame = 1;
                    n = build_frame_obu(&hk, &seq, ctid, csid, tiles, ntiles, pkt, 8 << 20, NULL); put(ctx, mpi, pkt, n);
                    size_t m = build_show_existing(fh, &seq, 0, pkt2, 8 << 20); if (m) put(ctx, mpi, pkt2, m); se_key++;
                    fprintf(stderr, "frame %d: KEY rewritten as hidden + show_existing(slot 0)\n", fidx);
                } else {
                    n = build_frame_obu(fh, &seq, ctid, csid, tiles, ntiles, pkt, 8 << 20, NULL);
                    if (!n) { fprintf(stderr, "frame %d: build failed\n", fidx); }
                    else { put(ctx, mpi, pkt, n); if (do_mask0) { settle(ctx, mpi, fh->show_frame); drop_next = 0; } }
                    if (hidden_se && !fh->show_frame && fh->refresh_frame_flags) {
                        int slot = __builtin_ctz(fh->refresh_frame_flags);
                        size_t m = build_show_existing(fh, &seq, slot, pkt2, 8 << 20);
                        if (m) { int before = frames_out; put(ctx, mpi, pkt2, m); settle(ctx, mpi, 1);
                                 if (frames_out == before) fprintf(stderr, "frame %d: hidden, show_existing(slot %d) produced NO output (yet)\n", fidx, slot); else hidden_shown++; }
                    }
                }
                if (fh->show_frame && expf) fprintf(expf, "%d\n", fidx);
                { unsigned m = fh->frame_type == GST_AV1_KEY_FRAME && fh->show_frame ? 0xff : fh->refresh_frame_flags; for (int b = 0; b < 8; b++) if (m & (1u << b)) slot_pts[b] = fidx; }
                gm_save(fh);
                gst_av1_parser_reference_frame_update(parser, fh); fidx++;
            } /* TD, metadata, padding, tile lists: dropped */
            pos += consumed;
        }
    }
    MppPacket eos; mpp_packet_init(&eos, NULL, 0); mpp_packet_set_eos(eos);
    while (mpi->decode_put_packet(ctx, eos) != MPP_OK) { drain(ctx, mpi, 0); usleep(500); }
    mpp_packet_deinit(&eos);
    for (int k = 0; k < 3000; k++) { if (drain(ctx, mpi, 0) < 0) break; usleep(1000); }
    if (out) fclose(out); if (idx) fclose(idx); if (expf) fclose(expf);
    printf("tu=%d frame_headers=%d frames_out=%d dropped_extra=%d submitted_twice=%d passthrough=%d show_existing_key=%d hidden_shown=%d info_changes=%d %dx%d stride=%dx%d %s\n",
           tu, fidx, frames_out, dropped, twice, passthrough, se_key, hidden_shown, info_changes, w, h, hs, vs, fmt10 ? "NV15" : "NV12");
    mpp_destroy(ctx); mpp_dec_cfg_deinit(cfg); gst_av1_parser_free(parser);
    return 0;
}
