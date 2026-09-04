/* nv15-unpack-test.c — prove the NV15->P010 unpack (NEON + scalar tails) is
 * bit-exact against the naive per-sample reference for EVERY row width.
 * Build:  gcc -O2 -o /tmp/nv15test tests/nv15-unpack-test.c && /tmp/nv15test
 * The implementation is pasted in by the harness below so the test always
 * exercises the code that actually ships in rockchip_drv_video.c. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
static void ref_row(const uint8_t *s, uint16_t *d, int n){
    for (int x = 0; x < n; x++) { int p = (x*10)>>3, sh = (x*10)&7;
        d[x] = (uint16_t)(((((uint16_t)s[p] | ((uint16_t)s[p+1]<<8)) >> sh) & 0x3FF) << 6); } }
#define nv15_row_to_p010 impl_row
/*IMPL*/
int main(void){
    srandom(20260904); int fails = 0;
    for (int n = 1; n <= 4224; n++) {
        int rb = (n*10+7)/8 + 32; uint8_t *s = malloc(rb);
        for (int i = 0; i < rb; i++) s[i] = random() & 0xFF;
        uint16_t *a = calloc(n,2), *b = calloc(n,2);
        ref_row(s,a,n); impl_row(s,b,n);
        if (memcmp(a,b,(size_t)n*2)) { for (int i=0;i<n;i++) if (a[i]!=b[i]) {
            printf("MISMATCH width=%d idx=%d ref=%04x got=%04x\n", n,i,a[i],b[i]); fails++; break; } }
        free(s); free(a); free(b);
    }
    if (fails) { printf("FAILED: %d widths\n", fails); return 1; }
    printf("nv15 unpack: bit-exact vs reference for every width 1..4224 (incl. 1920/3840/4096)  OK\n");
    return 0; }
