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
static FILE *out; static int frames_out, dropped, info_changes, drop_next; static int w, h, hs, vs, fmt10;
static void dump_frame(MppFrame f) {
    MppBuffer buf = mpp_frame_get_buffer(f); if (!buf) return;
    uint8_t *p = mpp_buffer_get_ptr(buf);
    w = mpp_frame_get_width(f); h = mpp_frame_get_height(f); hs = mpp_frame_get_hor_stride(f); vs = mpp_frame_get_ver_stride(f);
    fmt10 = ((mpp_frame_get_fmt(f) & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP_10BIT);
    if (drop_next) { drop_next = 0; dropped++; return; }
    frames_out++;
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
    MppPacket pkt; mpp_packet_init(&pkt, (void *)data, len); int tries = 0; MPP_RET r;
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

/* Build an OBU_FRAME packet: header bits from our writer (given the header struct), byte-aligned, + tile bytes. */
static size_t build_frame_obu(const GstAV1FrameHeaderOBU *fh, const GstAV1SequenceHeaderOBU *seq, uint8_t tid, uint8_t sid,
                              const uint8_t *tiles, size_t tiles_len, uint8_t *dst, size_t dst_cap, size_t *hdr_bits_out) {
    static uint8_t tmp[1 << 16]; guint tsz = sizeof(tmp);
    if (gst_av1_bit_writer_frame_header_obu(fh, seq, tid, sid, TRUE, tmp, &tsz) != GST_AV1_BIT_WRITER_OK) return 0;
    int ext = (tmp[0] >> 2) & 1; size_t off = 1 + ext; uint64_t psz; size_t l = leb128(tmp + off, tsz - off, &psz);
    const uint8_t *p = tmp + off + l; size_t nb = header_bits(p, psz), hb = (nb + 7) / 8;
    if (hdr_bits_out) *hdr_bits_out = nb;
    uint8_t hdr[sizeof(tmp)]; memcpy(hdr, p, hb); if (nb % 8) hdr[hb - 1] &= (uint8_t)(0xff << (8 - nb % 8));   /* byte_alignment(): zeros */
    size_t payload = hb + tiles_len, n = 0;
    if (payload + 16 > dst_cap) return 0;
    dst[n++] = (uint8_t)((6 << 3) | (ext << 2) | 0x02);                /* OBU_FRAME, has_size_field */
    if (ext) dst[n++] = tmp[1];
    n += leb128_write(dst + n, payload);
    memcpy(dst + n, hdr, hb); n += hb; memcpy(dst + n, tiles, tiles_len); n += tiles_len;
    return n;
}

int main(int argc, char **argv) {
    const char *outp = NULL, *mask0 = NULL; int report = 0, i;
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--out")) outp = argv[++i]; else if (!strcmp(argv[i], "--mask0")) mask0 = argv[++i];
        else if (!strcmp(argv[i], "--report")) report = 1; else break;
    }
    if (i != argc - 1) { fprintf(stderr, "usage: av1exp3 [--mask0 all|N,..] [--out f] [--report] in.ivf\n"); return 2; }
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
    int tu = 0, fidx = 0, twice = 0, passthrough = 0, se_key = 0;
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
            } else if (obu.obu_type == GST_AV1_OBU_FRAME && have_seq) {
                GstAV1FrameOBU frame;
                if (gst_av1_parser_parse_frame_obu(parser, &obu, &frame) != GST_AV1_PARSER_OK) { fprintf(stderr, "tu %d: frame parse failed\n", tu); pos += consumed; continue; }
                GstAV1FrameHeaderOBU *fh = &frame.frame_header;
                /* tile bytes = original payload after the header's byte alignment; header length from our own writer */
                size_t nb = 0; size_t n = build_frame_obu(fh, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, obu.data, 0, pkt, 8 << 20, &nb);
                size_t hb = (nb + 7) / 8; const uint8_t *tiles = obu.data + hb; size_t tiles_len = obu.obu_size > hb ? obu.obu_size - hb : 0;
                if (!n || tiles_len == 0) { put(ctx, mpi, buf + pos, consumed); passthrough++; }
                else {
                    int do_mask0 = 0;
                    if (mask0 && fh->frame_type != GST_AV1_KEY_FRAME && fh->refresh_frame_flags) {
                        if (mask_all) do_mask0 = 1; else { char s[16]; snprintf(s, sizeof s, "%d", fidx); const char *q = mask0; size_t ls = strlen(s);
                            while ((q = strstr(q, s))) { if ((q == mask0 || q[-1] == ',') && (q[ls] == 0 || q[ls] == ',')) { do_mask0 = 1; break; } q += ls; } }
                    }
                    if (report || do_mask0) fprintf(stderr, "frame %d (tu %d): type %u show %u se %u refresh 0x%02x order_hint %u%s\n", fidx, tu, fh->frame_type, fh->show_frame,
                                               fh->show_existing_frame, fh->refresh_frame_flags, fh->order_hint, do_mask0 ? "  <- mask0 then real" : "");
                    if (do_mask0) {
                        GstAV1FrameHeaderOBU fh0 = *fh; fh0.refresh_frame_flags = 0;
                        size_t n0 = build_frame_obu(&fh0, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, tiles, tiles_len, pkt2, 8 << 20, NULL);
                        if (!n0) { fprintf(stderr, "mask0 rewrite failed at frame %d\n", fidx); }
                        else { put(ctx, mpi, pkt2, n0); settle(ctx, mpi, fh->show_frame); twice++; drop_next = fh->show_frame; }
                    }
                    n = build_frame_obu(fh, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, tiles, tiles_len, pkt, 8 << 20, NULL);
                    put(ctx, mpi, pkt, n); if (do_mask0) { settle(ctx, mpi, fh->show_frame); drop_next = 0; }
                }
                gst_av1_parser_reference_frame_update(parser, fh); fidx++;
            } else if (obu.obu_type == GST_AV1_OBU_FRAME_HEADER && have_seq) {
                GstAV1FrameHeaderOBU fh;
                if (gst_av1_parser_parse_frame_header_obu(parser, &obu, &fh) == GST_AV1_PARSER_OK) {
                    if (fh.show_existing_frame && fh.frame_type == GST_AV1_KEY_FRAME) se_key++;
                    if (report) fprintf(stderr, "frame %d (tu %d): FRAME_HEADER type %u show %u se %u (slot %u) refresh 0x%02x\n", fidx, tu, fh.frame_type, fh.show_frame, fh.show_existing_frame, fh.frame_to_show_map_idx, fh.refresh_frame_flags);
                    gst_av1_parser_reference_frame_update(parser, &fh); fidx++;
                }
                put(ctx, mpi, buf + pos, consumed); passthrough++;   /* show_existing / standalone headers: pass through unchanged (experiment 4 observes them) */
            } /* TD, metadata, padding, tile lists: dropped */
            pos += consumed;
        }
    }
    MppPacket eos; mpp_packet_init(&eos, NULL, 0); mpp_packet_set_eos(eos);
    while (mpi->decode_put_packet(ctx, eos) != MPP_OK) { drain(ctx, mpi, 0); usleep(500); }
    mpp_packet_deinit(&eos);
    for (int k = 0; k < 3000; k++) { if (drain(ctx, mpi, 0) < 0) break; usleep(1000); }
    if (out) fclose(out);
    printf("tu=%d frame_headers=%d frames_out=%d dropped_duplicates=%d submitted_twice=%d passthrough=%d show_existing_key=%d info_changes=%d %dx%d stride=%dx%d %s\n",
           tu, fidx, frames_out, dropped, twice, passthrough, se_key, info_changes, w, h, hs, vs, fmt10 ? "NV15" : "NV12");
    mpp_destroy(ctx); mpp_dec_cfg_deinit(cfg); gst_av1_parser_free(parser);
    return 0;
}
