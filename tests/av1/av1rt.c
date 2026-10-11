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
                    GstAV1BitWriterResult w = gst_av1_bit_writer_frame_header_obu(fp, &seq, obu.header.obu_temporal_id, obu.header.obu_spatial_id, TRUE, out, &osz);
                    if (w != GST_AV1_BIT_WRITER_OK) { fprintf(stderr, "tu %d: frame header write failed %d (type %u show_existing %u)\n", tu, w, fp->frame_type, fp->show_existing_frame); errs++; }
                    else {
                        size_t plen; const uint8_t *p = payload_of(out, osz, &plen);          /* our frame-header payload */
                        const uint8_t *q = obu.data; size_t qlen = obu.obu_size;              /* original payload (frame hdr [+ tile group]) */
                        size_t nb = header_bits(p, plen), bad = 0; int ok = (nb / 8 < qlen) && bits_equal(p, q, nb, &bad);
                        if (ok) fh_ok++;
                        else { fprintf(stderr, "tu %d: FRAME HEADER differs (type %u, show_frame %u, show_existing %u, ours %zu header bits vs orig payload %zu B): first diff at bit %zu\n",
                                    tu, fp->frame_type, fp->show_frame, fp->show_existing_frame, nb, qlen, bad);
                            if (verbose) { fprintf(stderr, "  ours:"); for (size_t i = 0; i < plen && i < 24; i++) fprintf(stderr, " %02x", p[i]); fprintf(stderr, "\n  orig:"); for (size_t i = 0; i < qlen && i < 24; i++) fprintf(stderr, " %02x", q[i]); fprintf(stderr, "\n"); }
                        }
                    }
                    /* keep the parser's reference state in step */
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
