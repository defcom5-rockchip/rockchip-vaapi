/*
 * gstcompat.h — the little bit of GLib/GStreamer that gstav1bitwriter.c and gstav1parser.h need, so the AV1 OBU
 * writer from GStreamer (gst-plugins-bad 1.28.2, Intel 2022, LGPL-2.0-or-later) can be built inside rockchip-vaapi
 * unmodified apart from its #includes.
 *
 * Copyright (C) 2026 defcom5-rockchip
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* GLib basic types */
typedef int gboolean; typedef char gchar; typedef unsigned char guchar;
typedef int8_t gint8; typedef uint8_t guint8; typedef int16_t gint16; typedef uint16_t guint16;
typedef int32_t gint32; typedef uint32_t guint32; typedef int64_t gint64; typedef uint64_t guint64;
typedef int gint; typedef unsigned int guint; typedef size_t gsize; typedef void *gpointer; typedef const void *gconstpointer;
typedef double gdouble; typedef float gfloat;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define G_BEGIN_DECLS
#define G_END_DECLS
#define G_GNUC_UNUSED __attribute__((unused))
#define G_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define G_LIKELY(x)   __builtin_expect(!!(x), 1)
#define G_MAXUINT UINT_MAX
#define G_MAXUINT32 UINT32_MAX
#define G_MAXINT INT_MAX
#define G_N_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))
#define GST_USE_UNSTABLE_API 1
#define GST_CODEC_PARSERS_API
#define GST_CODEC_PARSERS_DEPRECATED
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x, lo, hi) (((x) > (hi)) ? (hi) : (((x) < (lo)) ? (lo) : (x)))
#endif
#define g_assert(expr) do { if (!(expr)) { fprintf(stderr, "rockchip-vaapi av1 writer: assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); } } while (0)
#define g_return_val_if_fail(expr, val) do { if (G_UNLIKELY(!(expr))) { fprintf(stderr, "rockchip-vaapi av1 writer: %s: check '%s' failed\n", __func__, #expr); return (val); } } while (0)
#define g_return_if_fail(expr) do { if (G_UNLIKELY(!(expr))) { fprintf(stderr, "rockchip-vaapi av1 writer: %s: check '%s' failed\n", __func__, #expr); return; } } while (0)

/* g_once: the writer uses it only to create its debug category once; single-threaded init is fine here */
static inline gboolean g_once_init_enter(void *location) { return *(volatile gsize *)location == 0; }
static inline void g_once_init_leave(void *location, gsize result) { *(volatile gsize *)location = result; }
#define g_warning(...) do { fprintf(stderr, "rockchip-vaapi av1 writer: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* GStreamer debug: off by default; RKVA_AV1_WRITER_DEBUG=1 prints the writer's own messages to stderr */
extern int rkva_av1_writer_debug;
#define GST_WARNING(...) do { if (rkva_av1_writer_debug) { fprintf(stderr, "av1-writer warning: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define GST_DEBUG(...)   do { if (rkva_av1_writer_debug > 1) { fprintf(stderr, "av1-writer: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define GST_LOG(...)     do { } while (0)
#define GST_ERROR(...)   GST_WARNING(__VA_ARGS__)
#define GST_INFO(...)    GST_DEBUG(__VA_ARGS__)
#define GST_DEBUG_CATEGORY_STATIC(c)
#define GST_DEBUG_CATEGORY_INIT(...)
typedef struct _GstDebugCategory GstDebugCategory;

/* GstBitWriter: the subset gstav1bitwriter.c uses (MSB-first bit packing into a caller-provided buffer) */
typedef struct {
    guint8 *data;
    guint   bit_size;      /* bits written so far */
    guint   bit_capacity;  /* capacity in bits */
    guint   auto_grow;     /* unused: we never grow */
    guint   owned;
} GstBitWriter;

static inline void gst_bit_writer_init_with_data(GstBitWriter *bw, guint8 *data, guint size, gboolean initialized) {
    bw->data = data; bw->bit_size = 0; bw->bit_capacity = size * 8u; bw->auto_grow = 0; bw->owned = 0;
    if (!initialized) memset(data, 0, size);
}
static inline void gst_bit_writer_reset(GstBitWriter *bw) { bw->data = NULL; bw->bit_size = 0; bw->bit_capacity = 0; }
static inline guint gst_bit_writer_get_size(const GstBitWriter *bw) { return bw->bit_size; }
static inline gboolean gst_bit_writer_put_bits_uint32(GstBitWriter *bw, guint32 value, guint nbits) {
    if (nbits > 32 || bw->bit_size + nbits > bw->bit_capacity) return FALSE;
    for (int i = (int)nbits - 1; i >= 0; i--) {
        guint pos = bw->bit_size++;
        if ((value >> i) & 1u) bw->data[pos >> 3] |= (guint8)(0x80u >> (pos & 7));
        else                   bw->data[pos >> 3] &= (guint8)~(0x80u >> (pos & 7));
    }
    return TRUE;
}
static inline gboolean gst_bit_writer_put_bits_uint16(GstBitWriter *bw, guint16 value, guint nbits) { return nbits <= 16 && gst_bit_writer_put_bits_uint32(bw, value, nbits); }
static inline gboolean gst_bit_writer_put_bits_uint8(GstBitWriter *bw, guint8 value, guint nbits)   { return nbits <= 8 && gst_bit_writer_put_bits_uint32(bw, value, nbits); }
static inline gboolean gst_bit_writer_put_bits_uint64(GstBitWriter *bw, guint64 value, guint nbits) {
    if (nbits > 64) return FALSE;
    if (nbits > 32) { if (!gst_bit_writer_put_bits_uint32(bw, (guint32)(value >> 32), nbits - 32)) return FALSE; nbits = 32; }
    return gst_bit_writer_put_bits_uint32(bw, (guint32)value, nbits);
}
static inline gboolean gst_bit_writer_align_bytes(GstBitWriter *bw, guint8 trailing_bit) {
    while (bw->bit_size & 7) if (!gst_bit_writer_put_bits_uint32(bw, trailing_bit & 1u, 1)) return FALSE;
    return TRUE;
}
