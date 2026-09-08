/* va-export-before-decode — what does the driver describe an UNDECODED surface as?
 * Chrome exports right after vaCreateSurfaces; the description must match the created depth.
 * Build: cc va-export-before-decode.c -o va-export-before-decode -lva -lva-drm
 * Run:   LIBVA_DRIVER_NAME=rockchip [LIBVA_DRIVERS_PATH=dir] ./va-export-before-decode */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
static const char *fcc(unsigned v){ static char bufs[8][8]; static int k; char *b = bufs[k++ & 7]; b[0]=v&255; b[1]=(v>>8)&255; b[2]=(v>>16)&255; b[3]=(v>>24)&255; b[4]=0; return b; }
static int probe(VADisplay dpy, unsigned rt, unsigned pixfmt, const char *label) {
    VASurfaceAttrib at[2]; int na = 0;
    at[na].type = VASurfaceAttribUsageHint; at[na].flags = VA_SURFACE_ATTRIB_SETTABLE; at[na].value.type = VAGenericValueTypeInteger; at[na].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER; na++;
    if (pixfmt) { at[na].type = VASurfaceAttribPixelFormat; at[na].flags = VA_SURFACE_ATTRIB_SETTABLE; at[na].value.type = VAGenericValueTypeInteger; at[na].value.value.i = (int)pixfmt; na++; }
    VASurfaceID sid; VAStatus st = vaCreateSurfaces(dpy, rt, 1920, 1080, &sid, 1, at, na);
    if (st != VA_STATUS_SUCCESS) { printf("%-34s vaCreateSurfaces failed: %s\n", label, vaErrorStr(st)); return 1; }
    VADRMPRIMESurfaceDescriptor d = {0};
    st = vaExportSurfaceHandle(dpy, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
    if (st != VA_STATUS_SUCCESS) { printf("%-34s export failed: %s\n", label, vaErrorStr(st)); vaDestroySurfaces(dpy, &sid, 1); return 1; }
    printf("%-34s fourcc=%s layers=%u  L0=%s pitch=%u  L1=%s off=%u pitch=%u  size=%u\n", label, fcc(d.fourcc), d.num_layers,
           fcc(d.layers[0].drm_format), d.layers[0].pitch[0], d.num_layers > 1 ? fcc(d.layers[1].drm_format) : "-", d.layers[1].offset[0], d.layers[1].pitch[0], d.objects[0].size);
    for (unsigned i = 0; i < d.num_objects; i++) close(d.objects[i].fd);
    vaDestroySurfaces(dpy, &sid, 1); return 0;
}
int main(void) {
    const char *nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0", NULL };
    VADisplay dpy = NULL; int fd = -1, maj, min;
    for (int i = 0; nodes[i]; i++) { fd = open(nodes[i], O_RDWR | O_CLOEXEC); if (fd < 0) continue; dpy = vaGetDisplayDRM(fd);
        if (dpy && vaInitialize(dpy, &maj, &min) == VA_STATUS_SUCCESS) { printf("VA %d.%d on %s: %s\n", maj, min, nodes[i], vaQueryVendorString(dpy)); break; }
        dpy = NULL; close(fd); fd = -1; }
    if (!dpy) { printf("no VA display\n"); return 2; }
    int rc = 0;
    rc |= probe(dpy, VA_RT_FORMAT_YUV420,    0,              "8-bit  RT=0x1   (no fourcc)");
    rc |= probe(dpy, VA_RT_FORMAT_YUV420_10, 0,              "10-bit RT=0x100 (no fourcc, Chrome)");
    rc |= probe(dpy, VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, "10-bit RT=0x100 + P010 (mpv/ffmpeg)");
    rc |= probe(dpy, VA_RT_FORMAT_YUV420,    VA_FOURCC_NV12, "8-bit  RT=0x1   + NV12");
    vaTerminate(dpy); close(fd); return rc;
}
