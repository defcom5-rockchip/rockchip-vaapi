/* nv15unpack.c — unpack MPP's NV15 (10-bit, 4 samples per 5 bytes, little-endian bit-packed rows) into yuv420p10le.
 * usage: nv15unpack in.nv15 out.yuv width height hor_stride   (input = frames of height + height/2 rows of hor_stride bytes)
 * Build: gcc -O2 -o nv15unpack nv15unpack.c */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static void unpack_row(const uint8_t *src, uint16_t *dst, int n) {
    /* sample k occupies bits [10k, 10k+10) of the row, LSB first */
    for (int k = 0; k < n; k++) {
        size_t bit = (size_t)k * 10, byte = bit >> 3; int sh = bit & 7;
        uint32_t v = src[byte] | (src[byte + 1] << 8) | ((sh > 6) ? (src[byte + 2] << 16) : 0);
        dst[k] = (v >> sh) & 0x3ff;
    }
}
int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: nv15unpack in.nv15 out.yuv w h hor_stride\n"); return 2; }
    int w = atoi(argv[3]), h = atoi(argv[4]), hs = atoi(argv[5]);
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb"); if (!in || !out) { perror("open"); return 1; }
    uint8_t *row = malloc(hs + 4); uint16_t *y = malloc(w * 2), *u = malloc(w), *v = malloc(w); size_t frames = 0;
    for (;;) {
        int r;
        for (r = 0; r < h; r++) { if (fread(row, 1, hs, in) != (size_t)hs) goto done; unpack_row(row, y, w); fwrite(y, 2, w, out); }
        /* chroma: h/2 rows of interleaved U,V (w samples per row) -> planar U then V per frame */
        uint16_t *ubuf = malloc((size_t)w / 2 * (h / 2) * 2), *vbuf = malloc((size_t)w / 2 * (h / 2) * 2);
        for (r = 0; r < h / 2; r++) {
            if (fread(row, 1, hs, in) != (size_t)hs) { free(ubuf); free(vbuf); goto done; }
            unpack_row(row, y, w);
            for (int k = 0; k < w / 2; k++) { ubuf[(size_t)r * (w / 2) + k] = y[2 * k]; vbuf[(size_t)r * (w / 2) + k] = y[2 * k + 1]; }
        }
        fwrite(ubuf, 2, (size_t)w / 2 * (h / 2), out); fwrite(vbuf, 2, (size_t)w / 2 * (h / 2), out); free(ubuf); free(vbuf);
        frames++;
    }
done:
    fprintf(stderr, "unpacked %zu frames\n", frames); fclose(out); return 0;
}
