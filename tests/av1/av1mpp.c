/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 defcom5-rockchip */
/*
 * av1mpp.c — experiment harness: feed an AV1 IVF file to Rockchip MPP (av1d) and dump decoded frames.
 *
 *   av1mpp [--mode tu|obu] [--extra] [--out out.nv12] in.ivf
 *
 *   --mode tu   one packet per IVF frame (a whole temporal unit, as Jellyfin's ffmpeg does)
 *   --mode obu  re-packetise: sequence header once (optionally as an EXTRA_DATA packet, --extra),
 *               temporal delimiters dropped, one packet per OBU_FRAME (or FRAME_HEADER + its TILE_GROUPs)
 *               — the shape a VA-API → OBU re-serialiser would produce.
 *
 * Output: 8-bit frames as cropped NV12 (compare with: ffmpeg -f rawvideo -pix_fmt nv12 -s WxH -i out.nv12
 * -pix_fmt yuv420p -f framemd5 -), 10-bit frames as raw NV15 rows (hor_stride bytes) for a separate unpacker.
 * Build on the board: gcc -O2 -o av1mpp av1mpp.c $(pkg-config --cflags --libs rockchip_mpp)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

static FILE *out; static int frames_out, info_changes; static int w, h, hs, vs, fmt10;

static void dump_frame(MppFrame f) {
    MppBuffer buf = mpp_frame_get_buffer(f);
    if (!buf) { fprintf(stderr, "frame without buffer (err=%u discard=%u)\n", mpp_frame_get_errinfo(f), mpp_frame_get_discard(f)); return; }
    uint8_t *p = mpp_buffer_get_ptr(buf);
    w = mpp_frame_get_width(f); h = mpp_frame_get_height(f);
    hs = mpp_frame_get_hor_stride(f); vs = mpp_frame_get_ver_stride(f);
    MppFrameFormat fmt = mpp_frame_get_fmt(f) & MPP_FRAME_FMT_MASK;
    fmt10 = (fmt == MPP_FMT_YUV420SP_10BIT);
    if (!out) return;
    if (!fmt10) {
        for (int y = 0; y < h; y++) fwrite(p + (size_t)y * hs, 1, w, out);
        uint8_t *uv = p + (size_t)hs * vs;
        for (int y = 0; y < h / 2; y++) fwrite(uv + (size_t)y * hs, 1, w, out);
    } else {  /* NV15: dump full stride rows, unpack elsewhere */
        for (int y = 0; y < h; y++) fwrite(p + (size_t)y * hs, 1, hs, out);
        uint8_t *uv = p + (size_t)hs * vs;
        for (int y = 0; y < h / 2; y++) fwrite(uv + (size_t)y * hs, 1, hs, out);
    }
}

static int drain(MppCtx ctx, MppApi *mpi, int block_ms) {
    MppFrame f = NULL; int got = 0;
    for (;;) {
        MPP_RET r = mpi->decode_get_frame(ctx, &f);
        if (r != MPP_OK || !f) { if (block_ms > 0) { usleep(1000); block_ms--; continue; } break; }
        if (mpp_frame_get_info_change(f)) {
            info_changes++;
            fprintf(stderr, "info change: %ux%u stride %ux%u fmt 0x%x\n", mpp_frame_get_width(f), mpp_frame_get_height(f),
                    mpp_frame_get_hor_stride(f), mpp_frame_get_ver_stride(f), mpp_frame_get_fmt(f));
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        } else if (mpp_frame_get_eos(f)) {
            mpp_frame_deinit(&f); f = NULL; return -1;
        } else { dump_frame(f); frames_out++; got++; }
        mpp_frame_deinit(&f); f = NULL;
    }
    return got;
}

static int put(MppCtx ctx, MppApi *mpi, const uint8_t *data, size_t len, int extra) {
    MppPacket pkt; mpp_packet_init(&pkt, (void *)data, len);
    if (extra) mpp_packet_set_extra_data(pkt);
    int tries = 0; MPP_RET r;
    while ((r = mpi->decode_put_packet(ctx, pkt)) != MPP_OK) {
        if (++tries > 2000) { fprintf(stderr, "put_packet failed: %d\n", r); mpp_packet_deinit(&pkt); return -1; }
        drain(ctx, mpi, 0); usleep(500);
    }
    mpp_packet_deinit(&pkt);
    drain(ctx, mpi, 0);
    return 0;
}

/* --- OBU walking --- */
static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) { *v = 0; for (size_t i = 0; i < 8 && i < n; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (7 * i); if (!(p[i] & 0x80)) return i + 1; } return 0; }
/* returns total OBU length (header+size+payload), sets type; 0 on error */
static size_t obu_len(const uint8_t *p, size_t n, int *type) {
    if (n < 1) return 0; int ext = (p[0] >> 2) & 1, has = (p[0] >> 1) & 1; *type = (p[0] >> 3) & 0xf;
    size_t off = 1 + ext; if (!has) return n; uint64_t sz; size_t l = leb128(p + off, n - off, &sz); if (!l) return 0;
    return off + l + sz;
}

int main(int argc, char **argv) {
    const char *mode = "tu", *outp = NULL; int extra = 0, i;
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--mode")) mode = argv[++i]; else if (!strcmp(argv[i], "--out")) outp = argv[++i];
        else if (!strcmp(argv[i], "--extra")) extra = 1; else break;
    }
    if (i != argc - 1) { fprintf(stderr, "usage: av1mpp [--mode tu|obu] [--extra] [--out f] in.ivf\n"); return 2; }
    FILE *in = fopen(argv[i], "rb"); if (!in) { perror("open"); return 1; }
    uint8_t hdr[32]; if (fread(hdr, 1, 32, in) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not IVF\n"); return 1; }
    if (outp) out = fopen(outp, "wb");

    MppCtx ctx; MppApi *mpi; MppDecCfg cfg;
    if (mpp_create(&ctx, &mpi) || mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAV1)) { fprintf(stderr, "mpp init failed\n"); return 1; }
    mpp_dec_cfg_init(&cfg); mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1); mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    RK_U32 block = 0; mpi->control(ctx, MPP_SET_OUTPUT_BLOCK, &block);

    size_t cap = 1 << 20; uint8_t *buf = malloc(cap), *pkt = malloc(cap); size_t pktn = 0;
    int ivf_frames = 0, packets = 0, seq_sent = 0; int obu_mode = !strcmp(mode, "obu");
    for (;;) {
        uint8_t fh[12]; if (fread(fh, 1, 12, in) != 12) break;
        uint32_t sz = fh[0] | fh[1] << 8 | fh[2] << 16 | (uint32_t)fh[3] << 24;
        if (sz > cap) { cap = sz * 2; buf = realloc(buf, cap); pkt = realloc(pkt, cap); }
        if (fread(buf, 1, sz, in) != sz) break;
        ivf_frames++;
        if (!obu_mode) { put(ctx, mpi, buf, sz, 0); packets++; continue; }
        size_t pos = 0; pktn = 0;
        while (pos < sz) {
            int t; size_t l = obu_len(buf + pos, sz - pos, &t); if (!l || pos + l > sz) { fprintf(stderr, "bad OBU at %zu\n", pos); break; }
            switch (t) {
            case 1: /* sequence header: once */
                if (!seq_sent) { put(ctx, mpi, buf + pos, l, extra); packets++; seq_sent = 1; }
                break;
            case 2: case 5: case 15: break;            /* TD, metadata, padding: dropped */
            case 6:                                     /* OBU_FRAME: its own packet */
                if (pktn) { put(ctx, mpi, pkt, pktn, 0); packets++; pktn = 0; }
                put(ctx, mpi, buf + pos, l, 0); packets++; break;
            case 3:                                     /* FRAME_HEADER: starts a packet (flush a pending one) */
                if (pktn) { put(ctx, mpi, pkt, pktn, 0); packets++; pktn = 0; }
                memcpy(pkt + pktn, buf + pos, l); pktn += l; break;
            case 4:                                     /* TILE_GROUP: append */
                memcpy(pkt + pktn, buf + pos, l); pktn += l; break;
            default: memcpy(pkt + pktn, buf + pos, l); pktn += l; break;
            }
            pos += l;
        }
        if (pktn) { put(ctx, mpi, pkt, pktn, 0); packets++; pktn = 0; }
    }
    /* EOS */
    MppPacket eos; mpp_packet_init(&eos, NULL, 0); mpp_packet_set_eos(eos);
    while (mpi->decode_put_packet(ctx, eos) != MPP_OK) { drain(ctx, mpi, 0); usleep(500); }
    mpp_packet_deinit(&eos);
    for (int k = 0; k < 3000; k++) { if (drain(ctx, mpi, 0) < 0) break; usleep(1000); }
    if (out) fclose(out);
    printf("ivf_frames=%d packets=%d frames_out=%d info_changes=%d size=%dx%d stride=%dx%d fmt=%s\n",
           ivf_frames, packets, frames_out, info_changes, w, h, hs, vs, fmt10 ? "NV15" : "NV12");
    mpp_destroy(ctx); mpp_dec_cfg_deinit(cfg);
    return frames_out ? 0 : 1;
}
