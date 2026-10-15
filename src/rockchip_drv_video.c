/*
 * rockchip_drv_video.c — VA-API driver for Rockchip RK3588 via MPP
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#ifdef HAVE_RGA
#include <rga/im2d.h>
#include <rga/rga.h>
#include <rga/drmrga.h>
#include <rga/RgaApi.h>
#include <rga/RgaUtils.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file { __u32 flags; __s32 fd; };
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#endif
/* The RGA does not take part in dma-buf implicit fencing.  Before it rewrites
 * a buffer the compositor may still be reading, export that buffer's READ
 * fences as a sync_file and hand it to the RGA job as in_fence_fd; the RGA
 * then waits for the GPU to finish with the buffer.  Kernel 6.1 supports the
 * ioctl (verified on the BSP kernel); where the GPU driver sets no fences the
 * sync_file is already signalled and this costs nothing.  -1 = no fence. */
static int rga_read_fence(int dmabuf_fd)
{
    struct dma_buf_export_sync_file ex = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
    if (dmabuf_fd < 0 || ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex) != 0)
        return -1;
    return ex.fd;
}
static void rga_fence_done(int fence)
{
    /* librga may or may not have closed it for us; only close what is still ours. */
    if (fence >= 0 && fcntl(fence, F_GETFD) != -1)
        close(fence);
}
/* Only the two RGA3 cores sit behind an IOMMU on the RK3588 (rk3588s.dtsi:
 * rga3_core0/rga3_core1 have "iommus", rga2 has none).  A job the kernel
 * schedules onto RGA2 with an MPP dma-buf therefore fails to map -- dmesg
 * shows "scheduler core[4] unsupported mm_flag[0x0]" and "map dma buffer
 * error" -- so pin every job to the RGA3 cores instead of letting the
 * scheduler pick.  RGA3 in turn only accepts aligned pixel strides, so
 * check those first and leave anything else to the CPU path. */
#define RGA3_CORES 0x3   /* RGA3_SCHEDULER_CORE0 | RGA3_SCHEDULER_CORE1 */
static inline bool rga3_stride_ok(int wstride_bytes, int hstride, bool ten_bit)
{
    /* wstride is a byte pitch here: 8-bit NV12 needs 16-byte, 10-bit 64-byte
     * alignment (the same rule ffmpeg-rockchip applies in rkrga_common.c). */
    return (hstride % 2) == 0 &&
           (ten_bit ? (wstride_bytes % 64) == 0 : (wstride_bytes % 16) == 0);
}
/* Once per process, whichever thread gets here first: decoders created at the same moment (Firefox's media
 * process) used to race the old test-then-set and could both run c_RkRgaInit(). */
static int rga_legacy_ready = 0;   /* 0 = untried, 1 = ok, -1 = failed */
static pthread_once_t rga_legacy_once = PTHREAD_ONCE_INIT;
static void rga_legacy_init_once(void)
{
    rga_legacy_ready = (c_RkRgaInit() == 0) ? 1 : -1;
}
static inline bool rga_legacy_init(void)
{
    pthread_once(&rga_legacy_once, rga_legacy_init_once);
    return rga_legacy_ready > 0;
}
#endif
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <errno.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "h264.h"
#include "hevc.h"
#include "av1.h"

/* DRM fourccs spelled out, never as hex literals: KI-3 (v2.0.0–v2.1.2) was
 * DRM_FORMAT_GR1616 mistyped as "GR16" — a fourcc that does not exist. */
#define RK_FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define RK_DRM_FORMAT_NV12   RK_FOURCC('N','V','1','2')   /* 0x3231564e */
#define RK_DRM_FORMAT_P010   RK_FOURCC('P','0','1','0')   /* 0x30313050 */
#define RK_DRM_FORMAT_R8     RK_FOURCC('R','8',' ',' ')   /* 0x20203852 */
#define RK_DRM_FORMAT_GR88   RK_FOURCC('G','R','8','8')   /* 0x38385247 */
#define RK_DRM_FORMAT_R16    RK_FOURCC('R','1','6',' ')   /* 0x20363152 */
#define RK_DRM_FORMAT_GR1616 RK_FOURCC('G','R','3','2')   /* 0x32335247 — "GR32": 16-bit G and R = 32 bpp */

/* ── logging ─────────────────────────────────────────────────── */
static FILE *g_log_fp = NULL;
static void log_init(void) {
    const char *p = getenv("RK_VAAPI_LOG");
    if (p && *p) g_log_fp = fopen(p, "a");
}
#include <time.h>
#include <sys/syscall.h>
/* monotonic microsecond clock for the per-step timing in the log */
static inline long long rk_now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000; }
#define LOG(fmt, ...) do { if (g_log_fp) { \
    long long _t = rk_now_us(); \
    fprintf(g_log_fp, "[rk-vaapi pid=%d tid=%ld t=%lld.%03lld] " fmt "\n", getpid(), (long)syscall(SYS_gettid), _t / 1000, _t % 1000, ##__VA_ARGS__); \
    fflush(g_log_fp); } \
} while(0)

/* ── limits ──────────────────────────────────────────────────── */
#define MAX_CONFIGS   16
#define MAX_CONTEXTS   8
/* 128, up from 64: Chrome's Linux VA-API path (VaapiVideoDecoder +
 * PlatformVideoFramePool) allocates one surface per output frame, sizes the
 * pool at reference-frames + 1 + a renderer estimate, and holds frames while
 * the compositor is busy; a 4K 10-bit stream exhausted 64 (forum report,
 * 2026-09-07: "vaCreateSurfaces (allocate mode) failed"). Not higher: every
 * surface carries a private MPP buffer (3 bytes/px, 19.7 MB at 4K) mapped
 * through the Rockchip DRM IOMMU, whose I/O virtual space is 4 GB —
 * measured 2026-09-06: ~200 4K surfaces -> rockchip_gem_iommu_map ENOSPC and
 * decode failure; 128 keeps 4K under ~2.5 GB with room for MPP's own DPB.
 * Slots cost nothing until a client asks for them. */
#define MAX_SURFACES  128
#define MAX_BUFFERS  256

/* VA object ID namespaces */
#define CONFIG_ID_BASE   0x01000000u
#define CONTEXT_ID_BASE  0x02000000u
#define SURFACE_ID_BASE  0x03000000u
#define BUFFER_ID_BASE   0x04000000u

/* ── data structures ─────────────────────────────────────────── */

typedef struct {
    bool          used;
    VAProfile     profile;
    VAEntrypoint  entrypoint;
} RKConfig;

typedef struct {
    bool         used;
    VAConfigID   config_id;
    VAProfile    profile;      /* copied from the config at CreateContext: a client may destroy the config first */
    int          width, height;

    MppCtx       mpp;
    MppApi      *mpi;
    MppCodingType coding;

    /* buffers collected between BeginPicture / EndPicture */
    VABufferID   pending[576];   /* Chromium: one slice-parameter buffer per AV1 tile (up to 512) + the rest */
    int          n_pending;

    VASurfaceID  render_target;

    /* FIFO queue of surfaces waiting for MPP decoded frames (in send order) */
    VASurfaceID  decode_queue[64];
    int          dq_head, dq_tail;

    /* H.264 state for SPS/PPS reconstruction */
    VAPictureParameterBufferH264 last_pp;
    bool         sps_sent;

    /* HEVC state for VPS/SPS/PPS reconstruction (Deep Ink phase 1.5) */
    VAPictureParameterBufferHEVC last_pp_hevc;
    int  hevc_sets_plus1;        /* 0 = unknown, 1 = original SPS had 0 RPS sets, 2 = had >=1 */
    int  hevc_sent_plus1;        /* what the last emitted SPS declared (same encoding) */
    int  h264_l0_minus1, h264_l1_minus1;   /* learned PPS num_ref_idx_lX_default_active_minus1 (0 until a slice teaches us) */
    /* Serialises MPP output handling for this context: decode_get_frame, assign_mpp_frame and the
       decode_queue head/tail. Taken by the decoding thread (EndPicture) and by any thread in
       vaSyncSurface; also guards the slot's lifetime (claim in CreateContext, teardown and clear in
       DestroyContext). It lives as long as the driver: initialised for every slot in __vaDriverInit,
       destroyed in rk_Terminate, and never cleared, so a thread still in vaSyncSurface on a context
       that is being destroyed waits on a valid mutex and then sees `used` false. MUST stay the last
       member: CTX_CLEAR stops short of it. Lock order: av1_lock, then out_lock, then a surface's lock. Not held
       across the retry sleeps; it is held while a frame is copied out (assign_mpp_frame). */
    /* AV1: the re-serialiser's state (see av1.h / do_av1_decode) */
    VADecPictureParameterBufferAV1 last_pp_av1;
    bool         have_pp_av1;
    GstAV1SequenceHeaderOBU av1_seq;
    bool         av1_seq_sent;
    struct {
        bool        valid;        /* a frame is held back until the next one reveals its refresh_frame_flags */
        bool        forced;       /* already decoded once with refresh_frame_flags = 0 (client synced it early) */
        bool        held;         /* that early decode asked MPP to hold the picture: its slots are committed later, no re-decode */
        uint8_t     hold_token;   /* the 'H' token; the 'C' must carry it */
        VASurfaceID surface;
        VASurfaceID ref_surface;  /* pp.current_frame: how later pictures refer to this one (== surface in every client seen) */
        VASurfaceID out_surface;  /* where the picture lands: pp.current_display_picture when the client gave a separate one (film grain), else surface */
        uint32_t    out_gen;      /* out_surface's gen when this picture was handed to us */
        uint32_t    ref_gen;      /* ref_surface's gen then */
        VADecPictureParameterBufferAV1 pp;
        uint8_t    *tiles_buf;    /* copies of the tile payloads (the client destroys its buffers after EndPicture) */
        size_t      tiles_len, tiles_cap;
        Av1Tile     tiles[512];
        int         ntiles;
    } av1_pending;
    /* Output thread (see out_thread_main): the only caller of decode_get_frame while it runs, blocking in MPP's
       output queue, so vaSyncSurface waits on the surface's condition variable instead of polling MPP. */
    pthread_t       out_thread;
    bool            out_started, out_stop;
    /* Zero-copy only for a client seen to re-export a surface after decoding into it again (Firefox, mpv export
       every frame).  Chromium and GStreamer import each surface once, at pool creation, and Chromium never calls
       vaSyncSurface per frame either: holding MPP's frame would leave them showing the surface's own, stale buffer. */
    bool            zc_ok;
    bool            av1_defer;      /* MPP accepted the deferred-refresh commands on this decoder */
    uint8_t         rkva_token;     /* last hold token sent */
    unsigned        life;           /* unique per claim (g_ctx_life), 0 once cleared: a waiter that sees it change
                                       knows its decoder was destroyed */
    int             zc_score;       /* +2 per held frame dropped unexported, -1 per exported one; > 16 turns zc_ok off */
    /* AV1: which picture MPP holds in each of its 8 reference slots (surface + its gen), as the refresh masks we sent
       say.  The synthesised show_existing of a KEY must name a slot that really holds that KEY: the old guess (slot 0
       when the KEY's mask had been cleared) showed whatever MPP had there -- with the deferred refresh, a picture MPP had
       already output, which MPP then released twice (mpp_buffer_ref_dec "non-positive ref_count", Chromium 48delayed). */
    VASurfaceID     av1_mslot[8];
    uint32_t        av1_mslot_gen[8];
    /* AV1 packets go to MPP from a per-context submit thread (see av1_put): the client's EndPicture never waits on
       MPP_ERR_BUFFER_FULL.  sub_lock/sub_cv live while sub_started. */
    struct RkPkt   *sub_head, *sub_tail;
    int             sub_count;      /* packets queued and not yet accepted by MPP */
    bool            sub_started, sub_stop;
    void           *sub_drv;
    pthread_t       sub_thread;
    pthread_mutex_t sub_lock;
    pthread_cond_t  sub_cv, sub_room;
    pthread_mutex_t out_lock;
    pthread_mutex_t av1_lock;   /* serialises the AV1 submit policy between EndPicture (decoder thread) and vaSyncSurface (any
                                   thread).  Driver lifetime like out_lock.  Lock order: av1_lock, then out_lock, then a surface. */
} RKContext;

static unsigned g_ctx_life;   /* source of RKContext.life */

/* Clear a context slot but keep its out_lock (the last member, see above). */
#define CTX_CLEAR(c) memset((c), 0, offsetof(RKContext, out_lock))
_Static_assert(offsetof(RKContext, out_lock) + 2 * sizeof(pthread_mutex_t) == sizeof(RKContext),
               "out_lock and av1_lock must be the last members of RKContext: CTX_CLEAR keeps everything after out_lock");

typedef struct {
    bool         used;
    int          width, height;

    /* Bit depth the CLIENT asked for at vaCreateSurfaces (RT format / pixel-format
     * attribute). Until a frame has been decoded, every description of this surface
     * (ExportSurfaceHandle, DeriveImage, GetImage) must follow it: Chrome exports a
     * surface BEFORE decoding into it, and deciding from the (not yet existing) frame
     * described every 10-bit surface as 8-bit NV12 — garbled, then green (KI-8). */
    bool         rt_10bit;

    /* filled after decode */
    MppFrame     frame;          /* always NULL (kept for ABI compat) */

    /* 8-bit zero-copy (RKVA_ZERO_COPY, default on): the decoded MppFrame is kept instead of being copied into
     * priv_buf, and ExportSurfaceHandle hands out MPP's own buffer with MPP's strides.  It is released when the
     * client renders into the surface again (BeginPicture -- the VA contract: a client does not reuse a surface it
     * is still displaying), on DestroySurfaces/DestroyContext/Terminate.  A client that reads the surface without
     * exporting it after this decode (Chromium imports once at pool creation; vaDeriveImage/vaGetImage users)
     * gets the old copy instead: surface_zc_materialize() copies into priv_buf at vaSyncSurface/DeriveImage/
     * GetImage time.  MPP never writes a buffer while a frame holds it (refcounted), so a reference picture that is
     * exported stays intact.  zc_prev keeps an exported frame alive if a second output lands on the same surface. */
    MppFrame     zc_frame, zc_prev;
    int          zc_fd;          /* zc_frame's buffer fd (owned by MPP, valid while the frame is held) */
    int          zc_hs, zc_vs;   /* MPP's strides for that frame */
    size_t       zc_size;
    bool         zc_exported;    /* handed to the client by ExportSurfaceHandle since it arrived */
    uint32_t     exported_gen;   /* gen at the last ExportSurfaceHandle after a decode (0 = none) */
    int          prime_fd;       /* dup'd fd to priv_buf, stable for surface lifetime */
    int          hstride;
    int          vstride;

    /* Dedicated per-surface DMA-BUF used as permanent output buffer.
     * Decoded pixels are copied here in assign_mpp_frame so MPP can
     * immediately reuse its internal 3-buffer pool, preventing the
     * buffer aliasing that causes wrong frames in Firefox's compositor.
     * Also serves as the pre-decode placeholder for ExportSurfaceHandle
     * capability probes (Firefox DMABUF probe before any decode). */
    MppBufferGroup priv_group;
    MppBuffer      priv_buf;

    MppFrameFormat fmt;     /* pixel format of last decoded frame (0 = NV12 default) */
    bool         decoded;
    VAContextID  ctx_id;   /* context currently decoding into this surface */
    Av1RefInfo   av1;      /* AV1: order hint / frame type / saved global motion of the picture decoded here */
    uint32_t     gen;      /* bumped each time the surface becomes a render target again; AV1 packets carry it in the
                              PTS high word so an output still in flight for its previous picture cannot complete the
                              new one (assign_mpp_frame drops it) */
    uint8_t      av1_mask; /* AV1: refresh_frame_flags it was submitted with (0xff = full refresh) */
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
} RKSurface;

/* The surface's bit depth as the client must see it: the decoded frame's format once
 * one exists, the created format before that (see rt_10bit). */
static inline bool surf_is_10bit(const RKSurface *s) {
    return s->decoded ? MPP_FRAME_FMT_IS_YUV_10BIT(s->fmt) : s->rt_10bit;
}

typedef struct {
    bool           used;
    VABufferType   type;
    unsigned int   size;
    unsigned int   num_elements;
    void          *data;
    bool           borrowed;   /* data points into an MPP buffer: do not free */
    unsigned short img_w, img_h; /* VAImage buffers: the size the image was created with (bounds for GetImage) */
} RKBuffer;

typedef struct {
    RKConfig   configs [MAX_CONFIGS];
    RKContext  contexts[MAX_CONTEXTS];
    RKSurface  surfaces[MAX_SURFACES];
    RKBuffer   buffers [MAX_BUFFERS];
    /* Guards claiming and releasing buffer slots. vaCreateBuffer used to find a free slot and mark it used in
       two unlocked steps; a decoding thread creating slice/parameter buffers and another thread creating image
       buffers (vaCreateImage/vaDeriveImage, e.g. ffmpeg's hwframe transfer) could claim the same slot. One
       thread's vaDestroyBuffer then wiped the other's buffer ("invalid VABufferID" on map, "invalid
       VASurfaceID" from vaGetImage, which reports a missing image buffer that way) or freed a borrowed MPP
       mapping (malloc(): invalid size / corrupted top size). Held only to claim or release a slot. */
    pthread_mutex_t buf_lock;
    /* Guards claiming and releasing config and surface slots, for the same reason. Firefox shares one VADisplay
       between all decoders in its media process (VADisplayHolder), so several decoders starting at once create
       configs and surface pools on one driver instance concurrently. Context slots are claimed under their own
       out_lock instead. */
    pthread_mutex_t obj_lock;
} RKDriver;

/* ── helpers ─────────────────────────────────────────────────── */

static RKDriver *drv_from_ctx(VADriverContextP ctx) {
    return (RKDriver *)ctx->pDriverData;
}

static RKConfig *config_by_id(RKDriver *d, VAConfigID id) {
    unsigned idx = id - CONFIG_ID_BASE;
    return (idx < MAX_CONFIGS && d->configs[idx].used) ? &d->configs[idx] : NULL;
}

static RKContext *context_by_id(RKDriver *d, VAContextID id) {
    unsigned idx = id - CONTEXT_ID_BASE;
    return (idx < MAX_CONTEXTS && d->contexts[idx].used) ? &d->contexts[idx] : NULL;
}

static RKSurface *surface_by_id(RKDriver *d, VASurfaceID id) {
    unsigned idx = id - SURFACE_ID_BASE;
    return (idx < MAX_SURFACES && d->surfaces[idx].used) ? &d->surfaces[idx] : NULL;
}

static RKBuffer *buffer_by_id(RKDriver *d, VABufferID id) {
    unsigned idx = id - BUFFER_ID_BASE;
    return (idx < MAX_BUFFERS && d->buffers[idx].used) ? &d->buffers[idx] : NULL;
}

/* Zero-copy eligibility is reversible: a client that keeps rendering into surfaces whose held frame it never exported
   (nor synced, which would have copied it) is reading them through an old import -- Chromium/GStreamer after a stray
   re-export.  Score it: +2 per such frame, -1 per exported one; above 16, zero-copy goes off for the context until the
   re-export rule turns it on again.  Firefox leaves ~10 % unexported (AV1 hidden frames never shown) and stays on. */
static void zc_note_drop(RKDriver *d, VAContextID cid, bool unexported) {
    RKContext *c = context_by_id(d, cid);
    if (!c || !__atomic_load_n(&c->zc_ok, __ATOMIC_ACQUIRE)) return;
    int sc = __atomic_load_n(&c->zc_score, __ATOMIC_RELAXED);
    sc = unexported ? sc + 2 : (sc > 0 ? sc - 1 : 0);
    if (sc > 16) {
        __atomic_store_n(&c->zc_ok, false, __ATOMIC_RELEASE);
        sc = 0;
        LOG("zero-copy: context 0x%x renders into surfaces whose frames it never exported: zero-copy off", cid);
    }
    __atomic_store_n(&c->zc_score, sc, __ATOMIC_RELAXED);
}

/* zero-copy helpers (defined with assign_mpp_frame) */
static void surface_zc_materialize(RKDriver *d, RKSurface *s, bool keep_exported);
static void surface_zc_take_locked(RKSurface *s, MppFrame *a, MppFrame *b);
static void zc_drop2(MppFrame a, MppFrame b);

static MppCodingType profile_to_coding(VAProfile p) {
    switch (p) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264High10:    return MPP_VIDEO_CodingAVC;
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:    return MPP_VIDEO_CodingHEVC;
    case VAProfileVP8Version0_3: return MPP_VIDEO_CodingVP8;
    case VAProfileVP9Profile0:
    case VAProfileVP9Profile2:   return MPP_VIDEO_CodingVP9;
    case VAProfileAV1Profile0:
    case VAProfileAV1Profile1:   return MPP_VIDEO_CodingAV1;
    default:                     return MPP_VIDEO_CodingUnused;
    }
}

static int profile_idc(VAProfile p) {
    switch (p) {
    case VAProfileH264ConstrainedBaseline: return 66;
    case VAProfileH264Main:                return 77;
    case VAProfileH264High:                return 100;
    case VAProfileH264High10:              return 110;
    default:                               return 100;
    }
}

/* ── VADriverVTable implementations ──────────────────────────── */

static void av1_submit_stop(RKContext *c);   /* AV1 submit thread, see av1_put */
static bool av1_defer_ok(void);              /* AV1 deferred refresh, see av1_defer_probe */
/* MPP's deferred-refresh entry points (mpp_rkva.h, protocol 2), weak: NULL against an MPP without them */
extern RK_S32  mpp_av1d_rkva_deferred_refresh(void) __attribute__((weak));
extern MPP_RET mpp_av1d_rkva_enable(MppCtx ctx) __attribute__((weak));
#define RKVA_MPP_PROTOCOL 2
static void out_thread_start(RKContext *c, RKDriver *d);   /* output thread, see out_thread_main */
static void out_thread_stop(RKContext *c);

/* Tear an MPP decoder down without stranding its buffers.
 *
 * mpp_destroy() alone was not enough when a client abandoned a context early.
 * mpv's zero-copy attempt on the Mali-blob desktop destroys its decoder about
 * 60 ms after creating it, just after the info-change ack, with packets still
 * in flight and only two frames fetched. Each such destroy left about four full
 * decoder frame buffers (16,588,800 bytes each at 4K, dma-heap "system") and one
 * or two 2 MiB stream buffers open in the process until exit: ~70 MB per
 * attempt. A looping player exhausted the rkvdec IOMMU after ~55 attempts
 * (Pi-Desktop screensaver, 2026-10-07; mpv log + RK_VAAPI_LOG receipts).
 *
 * Reset first, so the decoder stops and gives back the pictures it holds, then
 * drain and release any frame still queued for output, then destroy.
 * decode_get_frame does not block here (MPP_SET_OUTPUT_BLOCK = 0).
 * Caller holds c->out_lock for the whole teardown, mpp_destroy included, so no
 * other thread can reach the decoder between the drain and the destroy. */
static void rk_mpp_teardown_locked(RKContext *c) {
    if (!c->mpp) return;
    if (c->mpi) {
        MppPollType nb = MPP_POLL_NON_BLOCK;      /* the output thread (stopped by now) used a blocking timeout */
        c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&nb);
        c->mpi->reset(c->mpp);
        MppFrame f = NULL;
        while (c->mpi->decode_get_frame(c->mpp, &f) == MPP_OK && f) {
            mpp_frame_deinit(&f);
            f = NULL;
        }
    }
    mpp_destroy(c->mpp);
    c->mpp = NULL;
    c->mpi = NULL;
}

static VAStatus rk_Terminate(VADriverContextP ctx) {
    LOG("Terminate: cleaning up driver");
    RKDriver *d = drv_from_ctx(ctx);
    if (!d) return VA_STATUS_SUCCESS;

    /* Stop every live context's submit and output threads first: they route frames into surfaces (and copy into
       their buffers) until stopped, so tearing surfaces down under them was a use-after-free. */
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        RKContext *c = &d->contexts[i];
        pthread_mutex_lock(&c->av1_lock);
        if (c->used) { av1_submit_stop(c); out_thread_stop(c); }
        pthread_mutex_unlock(&c->av1_lock);
    }

    /* destroy any leftover objects */
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!d->surfaces[i].used) continue;
        if (d->surfaces[i].frame) mpp_frame_deinit(&d->surfaces[i].frame);
        if (d->surfaces[i].zc_frame) mpp_frame_deinit(&d->surfaces[i].zc_frame);
        if (d->surfaces[i].zc_prev)  mpp_frame_deinit(&d->surfaces[i].zc_prev);
        if (d->surfaces[i].prime_fd >= 0) close(d->surfaces[i].prime_fd);
        if (d->surfaces[i].priv_buf)   mpp_buffer_put(d->surfaces[i].priv_buf);
        if (d->surfaces[i].priv_group) mpp_buffer_group_put(d->surfaces[i].priv_group);
        pthread_cond_destroy(&d->surfaces[i].cond);
        pthread_mutex_destroy(&d->surfaces[i].lock);
    }
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        RKContext *c = &d->contexts[i];
        pthread_mutex_lock(&c->av1_lock);
        pthread_mutex_lock(&c->out_lock);       /* threads already stopped above */
        if (c->used) { free(c->av1_pending.tiles_buf); c->av1_pending.tiles_buf = NULL; rk_mpp_teardown_locked(c); }
        pthread_mutex_unlock(&c->out_lock); pthread_mutex_unlock(&c->av1_lock);
        pthread_mutex_destroy(&c->out_lock);   /* every slot has both, used or not */
        pthread_mutex_destroy(&c->av1_lock);
    }
    for (int i = 0; i < MAX_BUFFERS; i++) {
        /* a derived image still alive aliases MPP memory: not ours to free */
        if (d->buffers[i].used && !d->buffers[i].borrowed) free(d->buffers[i].data);
    }
    pthread_mutex_destroy(&d->buf_lock);
    pthread_mutex_destroy(&d->obj_lock);
    free(d);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryConfigProfiles(VADriverContextP ctx,
                                       VAProfile *list, int *n) {
    (void)ctx;
    int i = 0;
    list[i++] = VAProfileH264ConstrainedBaseline;
    list[i++] = VAProfileH264Main;
    list[i++] = VAProfileH264High;
    list[i++] = VAProfileVP8Version0_3;
    list[i++] = VAProfileVP9Profile0;
    /* HEVC Main (8-bit): bit-exact vs software decode, displays in Firefox,
     * mpv, GStreamer and the VA-API Chromium builds. */
    list[i++] = VAProfileHEVCMain;
    /* 10-bit profiles: advertised by default since v2.1.3. They decode
     * bit-exact (NV15 -> P010 repack, "Deep Ink") and, now that the export
     * descriptor names DRM_FORMAT_GR1616 correctly, display zero-copy on the
     * stock panfork stack (mpv hwdec=vaapi, Firefox: eyeballed 2026-09-06).
     * RKVA_HIDE_10BIT=1 restores the 8-bit-only menu for a client that
     * still cannot present P010 (RKVA_ADVERTISE_ALL is kept as a no-op alias
     * so older test recipes keep working). */
    if (!getenv("RKVA_HIDE_10BIT")) {
        list[i++] = VAProfileH264High10;
        list[i++] = VAProfileHEVCMain10;
        list[i++] = VAProfileVP9Profile2;
    }
    /* AV1 Main (Profile 0, 8/10-bit): the driver re-serialises the OBU headers MPP needs from the VA
       parameters (av1.c).  RKVA_HIDE_AV1=1 hides it. */
    if (!getenv("RKVA_HIDE_AV1"))
        list[i++] = VAProfileAV1Profile0;
    *n = i;
    return VA_STATUS_SUCCESS;
}

/* RT formats a profile decodes into.  RKVA_HIDE_10BIT=1 (the Firefox stopgap while the Mali blob imports R16 planes as
   integer textures) must also cover AV1, whose 8- and 10-bit streams share VAProfileAV1Profile0: with it set, every
   profile answers 8-bit only, a 10-bit request is refused at vaCreateConfig, and P010 leaves the surface attributes —
   FFmpeg/ffvpx then fails hwaccel setup for a 10-bit AV1 stream and falls back to software. */
static bool hide_10bit(void) { static int v = -1; if (v < 0) v = getenv("RKVA_HIDE_10BIT") ? 1 : 0; return v == 1; }
static unsigned rt_formats_for(VAProfile profile) {
    (void)profile;
    return hide_10bit() ? VA_RT_FORMAT_YUV420 : (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10);
}

static VAStatus rk_QueryConfigEntrypoints(VADriverContextP ctx,
                                          VAProfile profile,
                                          VAEntrypoint *list, int *n) {
    (void)ctx;
    if (profile_to_coding(profile) == MPP_VIDEO_CodingUnused)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    list[0] = VAEntrypointVLD;
    *n = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_GetConfigAttributes(VADriverContextP ctx,
                                       VAProfile profile,
                                       VAEntrypoint entrypoint,
                                       VAConfigAttrib *list, int n) {
    (void)ctx; (void)entrypoint;
    for (int i = 0; i < n; i++) {
        LOG("GetConfigAttributes: type=%d", list[i].type);
        switch (list[i].type) {
        case VAConfigAttribRTFormat:
            list[i].value = rt_formats_for(profile);
            break;
        case VAConfigAttribDecSliceMode:
            list[i].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        case VAConfigAttribEncryption:
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        default:
            LOG("GetConfigAttributes: unsupported type=%d", list[i].type);
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateConfig(VADriverContextP ctx,
                                 VAProfile profile, VAEntrypoint entrypoint,
                                 VAConfigAttrib *attribs, int n_attribs,
                                 VAConfigID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    LOG("CreateConfig: profile=%d entrypoint=%d n_attribs=%d",
        profile, entrypoint, n_attribs);

    if (profile_to_coding(profile) == MPP_VIDEO_CodingUnused) {
        LOG("CreateConfig: unsupported profile %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != VAEntrypointVLD) {
        LOG("CreateConfig: unsupported entrypoint %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    for (int i = 0; i < n_attribs && attribs; i++) {
        if (attribs[i].type == VAConfigAttribRTFormat && attribs[i].value != VA_ATTRIB_NOT_SUPPORTED &&
            (attribs[i].value & ~rt_formats_for(profile))) {
            LOG("CreateConfig: RT format 0x%x not offered for profile %d (supported 0x%x)", attribs[i].value, profile, rt_formats_for(profile));
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
    }

    pthread_mutex_lock(&d->obj_lock);
    for (unsigned i = 0; i < MAX_CONFIGS; i++) {
        if (!d->configs[i].used) {
            d->configs[i].used = true;
            d->configs[i].profile = profile;
            d->configs[i].entrypoint = entrypoint;
            pthread_mutex_unlock(&d->obj_lock);
            *out_id = CONFIG_ID_BASE + i;
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&d->obj_lock);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus rk_DestroyConfig(VADriverContextP ctx, VAConfigID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->obj_lock);
    RKConfig *c = config_by_id(d, id);
    if (c) c->used = false;
    pthread_mutex_unlock(&d->obj_lock);
    return c ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus rk_QueryConfigAttributes(VADriverContextP ctx,
                                          VAConfigID id,
                                          VAProfile *profile,
                                          VAEntrypoint *entrypoint,
                                          VAConfigAttrib *attribs, int *n) {
    RKDriver *d = drv_from_ctx(ctx);
    RKConfig *c = config_by_id(d, id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    *profile = c->profile;
    *entrypoint = c->entrypoint;

    /* Chromium reads VAConfigAttribRTFormat here to decide which internal
       formats a profile supports; finding none, it treats the profile as
       unsupported.  NOTE: it passes an UNINITIALISED int as *n
       (`int num_config_attributes;`), so *n must not be read as an input
       capacity -- always write when attribs is non-NULL.  Otherwise Chromium
       reads a zero-filled entry whose type happens to equal
       VAConfigAttribRTFormat (enum value 0) with value 0, i.e. "no formats". */
    if (attribs) {
        attribs[0].type  = VAConfigAttribRTFormat;
        attribs[0].value = rt_formats_for(c->profile);
    }
    *n = 1;
    LOG("QueryConfigAttributes: profile=%d entrypoint=%d -> RTFormat=0x%x",
        c->profile, c->entrypoint, rt_formats_for(c->profile));
    return VA_STATUS_SUCCESS;
}

/* Give a surface slot back. Its buffers, fd, mutex and cond must already be released. */
static void surface_slot_release(RKDriver *d, RKSurface *s) {
    pthread_mutex_lock(&d->obj_lock);
    memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&d->obj_lock);
}

/* vaCreateSurfaces (old API, redirected) */
static VAStatus create_surfaces_impl(VADriverContextP ctx,
                                     int width, int height, bool rt_10bit,
                                     int n, VASurfaceID *ids) {
    RKDriver *d = drv_from_ctx(ctx);

    int allocated = 0;
    for (int s = 0; s < n; s++) {
        unsigned i;
        pthread_mutex_lock(&d->obj_lock);   /* find and mark in one step (see obj_lock) */
        for (i = 0; i < MAX_SURFACES; i++) {
            if (!d->surfaces[i].used) break;
        }
        if (i < MAX_SURFACES) {
            memset(&d->surfaces[i], 0, sizeof(RKSurface));
            d->surfaces[i].used = true;
        }
        pthread_mutex_unlock(&d->obj_lock);
        if (i == MAX_SURFACES) {
            LOG("CreateSurfaces: POOL EXHAUSTED — client asked for surface #%d "
                "while %d are live (MAX_SURFACES=%d); returning "
                "VA_STATUS_ERROR_ALLOCATION_FAILED", s + 1, (int)MAX_SURFACES, (int)MAX_SURFACES);
            /* roll back — must free placeholder buffers before zeroing */
            for (int j = 0; j < allocated; j++) {
                unsigned idx = ids[j] - SURFACE_ID_BASE;
                RKSurface *rb = &d->surfaces[idx];
                if (rb->prime_fd >= 0) close(rb->prime_fd);
                if (rb->priv_buf)   mpp_buffer_put(rb->priv_buf);
                if (rb->priv_group) mpp_buffer_group_put(rb->priv_group);
                pthread_mutex_destroy(&rb->lock);
                pthread_cond_destroy(&rb->cond);
                surface_slot_release(d, rb);
            }
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        RKSurface *surf = &d->surfaces[i];     /* claimed and zeroed above */
        surf->width    = width;
        surf->height   = height;
        surf->rt_10bit = rt_10bit;
        surf->prime_fd = -1;

        /* Pre-allocate placeholder DMA-BUF so ExportSurfaceHandle succeeds
         * before any decode (e.g. Firefox's DMABUF capability probe). */
        {
            unsigned hs = (unsigned)((width  + 63) & ~63);
            unsigned vs = (unsigned)((height + 15) & ~15);
            MppBufferGroup grp = NULL;
            MppBuffer      buf = NULL;
            if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) == MPP_OK &&
                mpp_buffer_get(grp, &buf, hs * vs * 3) == MPP_OK) {
                int raw_fd = mpp_buffer_get_fd(buf);
                int dup_fd = (raw_fd > 0) ? dup(raw_fd) : -1;
                if (dup_fd > 0) {
                    surf->priv_group = grp;
                    surf->priv_buf   = buf;
                    surf->prime_fd   = dup_fd;
                    surf->hstride    = (int)hs;
                    surf->vstride    = (int)vs;
                    LOG("CreateSurfaces: surface %ux%u placeholder prime_fd=%d rt10=%d",
                        (unsigned)width, (unsigned)height, surf->prime_fd, rt_10bit);
                } else {
                    LOG("CreateSurfaces: mpp_buffer_get_fd failed (raw_fd=%d), no placeholder", raw_fd);
                    mpp_buffer_put(buf);
                    mpp_buffer_group_put(grp);
                }
            } else {
                if (buf) mpp_buffer_put(buf);
                if (grp) mpp_buffer_group_put(grp);
                /* No buffer means no decode target and no export: fail the
                 * whole request cleanly so the client falls back (software
                 * decode) instead of driving a half-built pool into
                 * corruption. Typical cause: DRM IOMMU I/O-virtual space
                 * exhausted by too many large surfaces (dmesg:
                 * rockchip_gem_iommu_map ENOSPC). */
                LOG("CreateSurfaces: placeholder alloc FAILED for surface #%d "
                    "(%ux%u, %u bytes) — returning VA_STATUS_ERROR_ALLOCATION_FAILED",
                    s + 1, hs, vs, hs * vs * 3);
                for (int j = 0; j < allocated; j++) {
                    unsigned idx = ids[j] - SURFACE_ID_BASE;
                    RKSurface *rb = &d->surfaces[idx];
                    if (rb->prime_fd >= 0) close(rb->prime_fd);
                    if (rb->priv_buf)   mpp_buffer_put(rb->priv_buf);
                    if (rb->priv_group) mpp_buffer_group_put(rb->priv_group);
                    pthread_mutex_destroy(&rb->lock);
                    pthread_cond_destroy(&rb->cond);
                    surface_slot_release(d, rb);
                }
                surface_slot_release(d, surf);
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        }

        pthread_mutex_init(&surf->lock, NULL);
        pthread_cond_init(&surf->cond, NULL);
        ids[s] = SURFACE_ID_BASE + i;
        allocated++;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroySurfaces(VADriverContextP ctx,
                                    VASurfaceID *list, int n) {
    LOG("DestroySurfaces: n=%d", n);
    RKDriver *d = drv_from_ctx(ctx);
    for (int i = 0; i < n; i++) {
        RKSurface *s = surface_by_id(d, list[i]);
        if (!s) continue;
        /* The decoder that last rendered into this surface routes its frames asynchronously now (output thread,
           AV1 submit thread), always under its out_lock: hold that lock across the teardown so no frame is routed
           into a surface being destroyed.  Afterwards surface_by_id() no longer finds it and such a frame is dropped. */
        pthread_mutex_lock(&s->lock);
        VAContextID cid = s->ctx_id;
        MppFrame za, zb;
        surface_zc_take_locked(s, &za, &zb);
        pthread_mutex_unlock(&s->lock);
        RKContext *c = cid ? context_by_id(d, cid) : NULL;
        if (c) pthread_mutex_lock(&c->out_lock);
        zc_drop2(za, zb);
        if (s->frame)      mpp_frame_deinit(&s->frame);
        if (s->zc_frame)   mpp_frame_deinit(&s->zc_frame);   /* routed between the take and out_lock */
        if (s->zc_prev)    mpp_frame_deinit(&s->zc_prev);
        if (s->prime_fd >= 0) close(s->prime_fd);
        if (s->priv_buf)   { mpp_buffer_put(s->priv_buf);        s->priv_buf   = NULL; }
        if (s->priv_group) { mpp_buffer_group_put(s->priv_group); s->priv_group = NULL; }
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->lock);
        surface_slot_release(d, s);
        if (c) pthread_mutex_unlock(&c->out_lock);
    }
    return VA_STATUS_SUCCESS;
}

/* vaCreateSurfaces2 (new API with attributes) */
static VAStatus rk_CreateSurfaces2(VADriverContextP ctx,
                                    unsigned int format,
                                    unsigned int width, unsigned int height,
                                    VASurfaceID *ids, unsigned int n,
                                    VASurfaceAttrib *attribs,
                                    unsigned int n_attribs) {
    LOG("CreateSurfaces2: %ux%u fmt=0x%x n=%u n_attribs=%u",
        width, height, format, n, n_attribs);
    for (unsigned i = 0; i < n_attribs; i++) {
        LOG("  attrib[%u] type=%d flags=%d value=0x%x",
            i, attribs[i].type, attribs[i].flags,
            attribs[i].value.type == VAGenericValueTypeInteger
                ? (unsigned)attribs[i].value.value.i : 0u);
    }
    /* 10-bit if the RT format says so (VA_RT_FORMAT_YUV420_10 = 0x100) or the client
     * pinned the pixel format to P010; an explicit NV12 pixel format wins over the RT
     * format bit (ffmpeg creates 8-bit pools with RT 0x1, Chrome with 0x100 + no fourcc). */
    bool ten = (format & 0x100u) != 0;
    for (unsigned i = 0; i < n_attribs; i++) {
        if (attribs[i].type == VASurfaceAttribPixelFormat &&
            attribs[i].value.type == VAGenericValueTypeInteger) {
            unsigned fcc = (unsigned)attribs[i].value.value.i;
            if (fcc == VA_FOURCC_P010) ten = true;
            else if (fcc == VA_FOURCC_NV12) ten = false;
        }
    }
    return create_surfaces_impl(ctx, (int)width, (int)height, ten, (int)n, ids);
}

static VAStatus rk_CreateSurfaces(VADriverContextP ctx,
                                   int width, int height, int format,
                                   int n, VASurfaceID *ids) {
    return create_surfaces_impl(ctx, width, height, (format & 0x100) != 0, n, ids);
}

static VAStatus rk_CreateContext(VADriverContextP ctx,
                                  VAConfigID config_id,
                                  int width, int height,
                                  int flag,
                                  VASurfaceID *targets, int n_targets,
                                  VAContextID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)flag; (void)targets; (void)n_targets;

    RKConfig *cfg = config_by_id(d, config_id);
    if (!cfg) return VA_STATUS_ERROR_INVALID_CONFIG;

    MppCodingType coding = profile_to_coding(cfg->profile);
    if (coding == MPP_VIDEO_CodingUnused)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
#ifdef HAVE_RGA
    /* librga initialises itself implicitly inside the first API call a process
     * makes, and that cost has been reported as a 100 ms+ stall (librga#152).
     * Pay it here, when a decoder is being created, rather than on the decode
     * thread inside the first frame's copy. */
    rga_legacy_init();
#endif

    /* Build the decoder first, then claim a slot for it. The old order (claim a free slot, then spend
       milliseconds in mpp_create/mpp_init before marking it used) let two decoders starting at once
       (Firefox's media process, one shared VADisplay) take the same slot: same VAContextID for both, one
       MPP instance leaked, frames routed to the other decoder's surfaces. */
    LOG("CreateContext: config=0x%x %dx%d coding=%d",
        config_id, width, height, (int)coding);

    MppCtx  mpp = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = mpp_create(&mpp, &mpi);
    if (ret != MPP_OK) {
        LOG("mpp_create FAILED: %d", ret);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("CreateContext: mpp_create OK");

    ret = mpp_init(mpp, MPP_CTX_DEC, coding);
    if (ret != MPP_OK) {
        mpp_destroy(mpp);
        LOG("mpp_init FAILED: %d (coding=%d)", ret, (int)coding);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("CreateContext: mpp_init OK");

    /* Must be set after mpp_init: split_parse=0 means we send complete
     * access units; OUTPUT_BLOCK=0 makes decode_get_frame non-blocking. */
    MppDecCfg dec_cfg = NULL;
    mpp_dec_cfg_init(&dec_cfg);
    mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 0);
    mpi->control(mpp, MPP_DEC_SET_CFG, dec_cfg);
    mpp_dec_cfg_deinit(dec_cfg);

    int block = 0;
    mpi->control(mpp, MPP_SET_OUTPUT_BLOCK, (MppParam)&block);

    /* Emit every picture as soon as it is decoded, in decode order, instead
       of holding it back for output reordering.  A stateless VA-API client
       owns presentation order and blocks in vaSyncSurface on the exact
       surface it submitted; if MPP holds that picture waiting for later
       input, the client never submits the later input -- it is waiting on
       us -- and the sync deadline expires.  That deadlock is why B-frame
       streams fell back to software after a seek (the pre-roll submits too
       few pictures to satisfy a reorder window).  Telling MPP the truth
       here is better than telling the bitstream a lie about
       max_num_reorder_frames. */
    {
        RK_U32 immediate = 1;
        MPP_RET r = mpi->control(mpp, MPP_DEC_SET_IMMEDIATE_OUT, (MppParam)&immediate);
        LOG("CreateContext: MPP_DEC_SET_IMMEDIATE_OUT=1 -> %d%s",
            (int)r, r == MPP_OK ? "" : " (unsupported; reorder deadlock may persist)");
    }

    bool av1_defer = false;
    if (coding == MPP_VIDEO_CodingAV1 && av1_defer_ok()) {
        av1_defer = (mpp_av1d_rkva_enable(mpp) == MPP_OK);
        LOG("CreateContext: AV1 deferred refresh %s", av1_defer ? "enabled" : "refused by MPP (re-decode)");
    }

    for (unsigned i = 0; i < MAX_CONTEXTS; i++) {
        RKContext *c = &d->contexts[i];
        if (c->used) continue;                 /* busy: don't queue behind its frame copy or teardown */
        pthread_mutex_lock(&c->out_lock);      /* claim: test and mark in one step */
        if (c->used) { pthread_mutex_unlock(&c->out_lock); continue; }
        CTX_CLEAR(c);
        c->mpp       = mpp;
        c->mpi       = mpi;
        c->config_id = config_id;
        c->profile   = cfg->profile;
        c->width     = width;
        c->height    = height;
        c->coding    = coding;
        c->sps_sent  = false;
        c->av1_defer = av1_defer;
        do c->life = __atomic_add_fetch(&g_ctx_life, 1, __ATOMIC_RELAXED); while (c->life == 0);
        c->used      = true;
        pthread_mutex_unlock(&c->out_lock);
        out_thread_start(c, d);

        *out_id = CONTEXT_ID_BASE + i;
        return VA_STATUS_SUCCESS;
    }
    LOG("CreateContext: all %d context slots in use", MAX_CONTEXTS);
    mpp_destroy(mpp);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus rk_DestroyContext(VADriverContextP ctx, VAContextID id) {
    LOG("DestroyContext: ctx=0x%x", id);
    RKDriver *d = drv_from_ctx(ctx);
    RKContext *c = context_by_id(d, id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    /* Teardown and clear under the slot's lock, so a vaSyncSurface on another thread (mpv's VO exporting a
       surface while its decoder thread abandons zero-copy) either finishes its drain first or finds the slot
       unused afterwards; it can never reach a half-destroyed decoder or a destroyed mutex. */
    /* av1_lock before out_lock, the order EndPicture and a forcing vaSyncSurface use (av1_submit takes out_lock
       repeatedly under av1_lock); the reverse order deadlocked a sync on a held AV1 picture against this teardown. */
    pthread_mutex_lock(&c->av1_lock);
    if (c->used) { av1_submit_stop(c); out_thread_stop(c); }   /* before out_lock: both threads take it */
    pthread_mutex_lock(&c->out_lock);
    if (!c->used) {                            /* lost a race with another DestroyContext */
        pthread_mutex_unlock(&c->out_lock); pthread_mutex_unlock(&c->av1_lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    LOG("DestroyContext: reset + drain + destroy");
    /* zero-copy frames of this decoder: copy each into its surface's own buffer (a later export or image of the
       surface stays right) and give MPP its buffers back before the decoder goes.  An export the client still holds
       keeps its dma-buf alive by itself. */
    for (int i = 0; i < MAX_SURFACES; i++) {
        RKSurface *zs = &d->surfaces[i];
        if (!zs->used || zs->ctx_id != id || (!zs->zc_frame && !zs->zc_prev)) continue;
        surface_zc_materialize(d, zs, false);
        MppFrame za, zb;
        pthread_mutex_lock(&zs->lock); surface_zc_take_locked(zs, &za, &zb); pthread_mutex_unlock(&zs->lock);
        zc_drop2(za, zb);
    }
    free(c->av1_pending.tiles_buf); c->av1_pending.tiles_buf = NULL; c->av1_pending.valid = false;
    rk_mpp_teardown_locked(c);
    CTX_CLEAR(c);                              /* used = false, life = 0; out_lock and av1_lock untouched */
    /* wake whoever waits in vaSyncSurface on this decoder's surfaces: they see life change and return at once */
    for (int i = 0; i < MAX_SURFACES; i++) {
        RKSurface *ws = &d->surfaces[i];
        if (!ws->used || ws->ctx_id != id) continue;
        pthread_mutex_lock(&ws->lock); pthread_cond_broadcast(&ws->cond); pthread_mutex_unlock(&ws->lock);
    }
    pthread_mutex_unlock(&c->out_lock); pthread_mutex_unlock(&c->av1_lock);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateBuffer(VADriverContextP ctx,
                                 VAContextID context,
                                 VABufferType type,
                                 unsigned int size,
                                 unsigned int num_elements,
                                 void *data,
                                 VABufferID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)context;

    /* Claim the slot under the lock, allocate and fill outside it: an image buffer is a 25 MB malloc + memset
       at 4K 10-bit (ffmpeg's hwframe transfer creates one per frame), which would otherwise stall the decoding
       thread's per-slice vaCreateBuffer for milliseconds. The ID is not handed out until the buffer is ready. */
    RKBuffer *b = NULL;
    unsigned i;
    pthread_mutex_lock(&d->buf_lock);
    for (i = 0; i < MAX_BUFFERS; i++) {
        if (d->buffers[i].used) continue;
        b = &d->buffers[i];
        memset(b, 0, sizeof(*b));
        b->used = true;
        break;
    }
    pthread_mutex_unlock(&d->buf_lock);
    if (!b) return VA_STATUS_ERROR_ALLOCATION_FAILED;

    void *mem = malloc((size_t)size * num_elements);
    if (!mem) {
        pthread_mutex_lock(&d->buf_lock);
        memset(b, 0, sizeof(*b));
        pthread_mutex_unlock(&d->buf_lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (data) memcpy(mem, data, (size_t)size * num_elements);
    else      memset(mem, 0,    (size_t)size * num_elements);
    b->type         = type;
    b->size         = size;
    b->num_elements = num_elements;
    b->data         = mem;
    *out_id = BUFFER_ID_BASE + i;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_BufferSetNumElements(VADriverContextP ctx,
                                         VABufferID id, unsigned int n) {
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_by_id(d, id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    b->num_elements = n;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_MapBuffer(VADriverContextP ctx,
                              VABufferID id, void **ptr) {
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_by_id(d, id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    *ptr = b->data;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_UnmapBuffer(VADriverContextP ctx, VABufferID id) {
    (void)ctx; (void)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroyBuffer(VADriverContextP ctx, VABufferID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->buf_lock);
    RKBuffer *b = buffer_by_id(d, id);
    if (!b) { pthread_mutex_unlock(&d->buf_lock); return VA_STATUS_ERROR_INVALID_BUFFER; }
    if (!b->borrowed) free(b->data);   /* derived images alias MPP memory */
    memset(b, 0, sizeof(*b));
    pthread_mutex_unlock(&d->buf_lock);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_BeginPicture(VADriverContextP ctx,
                                 VAContextID ctx_id,
                                 VASurfaceID render_target) {
    LOG("BeginPicture: ctx=0x%x surface=0x%x", ctx_id, render_target);
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_by_id(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    c->render_target = render_target;
    c->n_pending     = 0;

    /* Reset surface state for this decode cycle.  priv_buf/prime_fd are kept
     * intact so ExportSurfaceHandle always returns a valid fd (the previous
     * decoded frame or the initial placeholder). assign_mpp_frame copies the
     * new decoded pixels into priv_buf without touching prime_fd. */
    RKSurface *s = surface_by_id(d, render_target);
    if (s) {
        MppFrame za, zb;
        pthread_mutex_lock(&s->lock);
        VAContextID zc_ctx = s->ctx_id;
        bool zc_held = s->zc_frame != NULL, zc_unexported = zc_held && !s->zc_exported;
        s->decoded = false;
        s->ctx_id  = ctx_id;
        if (++s->gen == 0) s->gen = 1;
        surface_zc_take_locked(s, &za, &zb);     /* the client renders into it again: done with the old picture */
        pthread_mutex_unlock(&s->lock);
        zc_drop2(za, zb);
        if (zc_held) zc_note_drop(d, zc_ctx, zc_unexported);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_RenderPicture(VADriverContextP ctx,
                                  VAContextID ctx_id,
                                  VABufferID *buffers, int n) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_by_id(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    const int max_pending = (int)(sizeof c->pending / sizeof c->pending[0]);
    if (c->n_pending + n > max_pending) LOG("RenderPicture: %d buffers exceed the pending list (%d); the picture will be refused", c->n_pending + n, max_pending);
    for (int i = 0; i < n && c->n_pending < max_pending; i++) {
        c->pending[c->n_pending++] = buffers[i];
        /* Snapshot VAPictureParameterBufferH264 immediately */
        RKBuffer *b = buffer_by_id(d, buffers[i]);
        if (b && b->type == VAPictureParameterBufferType &&
            c->coding == MPP_VIDEO_CodingAVC) {
            memcpy(&c->last_pp, b->data,
                   sizeof(VAPictureParameterBufferH264));
        } else if (b && b->type == VAPictureParameterBufferType &&
                   c->coding == MPP_VIDEO_CodingHEVC) {
            memcpy(&c->last_pp_hevc, b->data,
                   sizeof(VAPictureParameterBufferHEVC));
        
        } else if (b && b->type == VAPictureParameterBufferType &&
                   c->coding == MPP_VIDEO_CodingAV1 &&
                   b->size >= sizeof(VADecPictureParameterBufferAV1)) {
            memcpy(&c->last_pp_av1, b->data, sizeof(VADecPictureParameterBufferAV1));
            c->have_pp_av1 = true;
        }
    }
    return VA_STATUS_SUCCESS;
}

/* Route one MPP output frame to the right surface and mark it decoded.
 * Shared by EndPicture poll loops and the SyncSurface drain loop. */
/* NV15 -> P010 row repack ("Deep Ink").
 * MPP emits 10-bit YUV as NV15: fully packed, 5 bytes per 4 samples, no padding
 * bits.  Every consumer of this surface (the export descriptor above, Firefox,
 * Chrome, mpv) was promised P010: one uint16 per sample, 10 significant bits,
 * MSB-aligned.  Until now the copy below moved the packed bytes verbatim under
 * the P010 label -- solid green (HEVC) / corruption (VP9 Profile 2).
 * Unpack math per the canonical CPU reference (nyanmisaka ffmpeg-rockchip,
 * nv15_20ToYUV_c): sample x lives at bit offset x*10 of the row; read two
 * little-endian bytes, shift, mask to 10 bits; P010 wants it << 6. */
/* NV15 is periodic: every 5 bytes hold exactly 4 samples at bit offsets
 * 0/10/20/30.  At 4096x1714 this runs ~10.5M times per frame, 24 times a
 * second, on the decode thread, so it is worth vectorising.
 *
 * NEON path (8 samples / 10 bytes per iteration): one byte-table shuffle
 * gathers the little-endian 16-bit word containing each sample, one variable
 * right shift aligns all eight lanes, mask to 10 bits, shift left 6 for P010.
 * The x + 16 <= n guard keeps the 16-byte load inside the row's own packed
 * bytes: at the last iteration the read ends 4 bytes before the row does.
 * The scalar paths (4-at-a-time, then per sample) cover the tail and non-NEON
 * builds.  All three compute identical bits -- same math as nyanmisaka's
 * nv15_20ToYUV_c -- so output is bit-exact whichever runs. */
static inline void nv15_row_to_p010(const uint8_t *srow, uint16_t *drow, int n)
{
    int x = 0;
#if defined(__aarch64__)
    {
        static const uint8_t gather[16] = { 0,1, 1,2, 2,3, 3,4, 5,6, 6,7, 7,8, 8,9 };
        const uint8x16_t tbl = vld1q_u8(gather);
        const int16x8_t  rsh = { 0, -2, -4, -6, 0, -2, -4, -6 };
        const uint16x8_t msk = vdupq_n_u16(0x03FF);
        for (; x + 16 <= n; x += 8, srow += 10, drow += 8) {
            uint16x8_t v = vreinterpretq_u16_u8(vqtbl1q_u8(vld1q_u8(srow), tbl));
            v = vandq_u16(vshlq_u16(v, rsh), msk);
            vst1q_u16(drow, vshlq_n_u16(v, 6));
        }
    }
#endif
    for (; x + 4 <= n; x += 4, srow += 5, drow += 4) {
        uint32_t lo = (uint32_t)srow[0]        | ((uint32_t)srow[1] << 8) |
                     ((uint32_t)srow[2] << 16) | ((uint32_t)srow[3] << 24);
        uint32_t hi = (uint32_t)srow[4];
        drow[0] = (uint16_t)(( lo         & 0x3FFu) << 6);
        drow[1] = (uint16_t)(((lo >> 10)  & 0x3FFu) << 6);
        drow[2] = (uint16_t)(((lo >> 20)  & 0x3FFu) << 6);
        drow[3] = (uint16_t)((((lo >> 30) | (hi << 2)) & 0x3FFu) << 6);
    }
    for (int i = 0; x < n; x++, i++) {          /* fewer than 4 samples left */
        int pos = (i * 10) >> 3, sh = (i * 10) & 7;
        drow[i] = (uint16_t)(((((uint16_t)srow[pos] |
                                ((uint16_t)srow[pos + 1] << 8)) >> sh) & 0x3FF) << 6);
    }
}

/* Copy (or repack) one decoded MPP frame into the surface's own buffer, priv_buf, at the layout ExportSurfaceHandle
 * reported.  Returns the assign_mpp_frame `copied=` code (0 = nothing copied); out_w and out_h get the picture size
 * actually written.  Does not consume the frame. */
static int rk_copy_into_surface(MppFrame frame, RKSurface *s, VASurfaceID sid, int *out_w, int *out_h)
{
    MppBuffer      buf    = mpp_frame_get_buffer(frame);
    int            fwidth = (int)mpp_frame_get_width(frame);
    int            fheight= (int)mpp_frame_get_height(frame);
    int            fhs    = (int)mpp_frame_get_hor_stride(frame);
    int            fvs    = (int)mpp_frame_get_ver_stride(frame);
    MppFrameFormat ffmt   = mpp_frame_get_fmt(frame);

    int  copy_w = fwidth  > 0 ? fwidth  : s->width;
    int  copy_h = fheight > 0 ? fheight : s->height;
    int  src_hs = fhs > 0 ? fhs : copy_w;
    int  src_vs = fvs > 0 ? fvs : copy_h;
    bool i10    = MPP_FRAME_FMT_IS_YUV_10BIT(ffmt);
    int  bpp    = i10 ? 2 : 1;
    int  copied = 0;
    void *src = buf ? mpp_buffer_get_ptr(buf) : NULL;
    void *dst = s->priv_buf ? mpp_buffer_get_ptr(s->priv_buf) : NULL;
    /* The destination layout must match what ExportSurfaceHandle already
       reported, not MPP's frame stride.  Clients import the dma-buf once --
       Chromium does so at frame-pool creation, before any decode -- and never
       re-read the layout, so writing at MPP's stride puts the chroma plane at
       the wrong offset.  MPP does not use the alignment the surface was
       created with; observed on RK3588:
           3840x2160 -> MPP frame stride 3840x2176
           1920x1080 -> MPP frame stride 2304x1088
       Re-stride here instead: read with src_hs, write with dst_hs. */
    int dst_hs = s->hstride ? s->hstride : copy_w;
    int dst_vs = s->vstride ? s->vstride : copy_h;
    /* A picture larger than the surface was created for (AV1 sequence change inside one VA context, libaom
       av1-1-b8-03-sizeup) must never be copied past the placeholder: clamp, and tell the log.  The client sees
       a cropped picture rather than a corrupted heap. */
    if (copy_w > dst_hs || copy_h > dst_vs) {
        LOG("assign_mpp_frame: WARNING surface=0x%x holds %dx%d but MPP decoded %dx%d — cropping",
            (unsigned)sid, dst_hs, dst_vs, copy_w, copy_h);
        if (copy_w > dst_hs) copy_w = dst_hs;
        if (copy_h > dst_vs) copy_h = dst_vs;
    }
#ifdef HAVE_RGA
    /* Offload the plane copy to RGA, the RK3588 2D blitter, which handles
       strided NV12->NV12 in hardware, dma-buf to dma-buf.  At 4K the CPU
       memcpy moves 12.4MB per frame (~6ms, about a third of the 16.7ms budget
       at 60fps) on the decode path, which shows up as dropped frames.
       memcpy stays the fallback. */
    /* RKVA_RGA_COPY: "0" turns every RGA copy off (8-bit imcopy and the
       10-bit NV15->P010 lane); "1" also enables the 8-bit imcopy.  Default:
       8-bit copies stay on the CPU (as in 2.1.5), 10-bit uses the RGA lane.
       Rationale: the RGA does not take part in the GPU's implicit fencing on
       these kernels, so a fast RGA rewrite of a buffer the compositor may
       still be reading can tear; the 6 ms memcpy rarely collided, the 1 ms
       blit can.  Reported as intermittent artifacts in Chrome after long
       8-bit VP9 sessions with 2.2.0-rc1.  Until the copy is fenced, 8-bit
       stays opt-in. */
    const char *rga_sw = getenv("RKVA_RGA_COPY");
    bool rga_off    = (rga_sw && rga_sw[0] == '0');
    bool rga_8bit   = (rga_sw && rga_sw[0] == '1');
    if (rga_8bit && !rga_off && !i10 && buf && s->priv_buf && rga_legacy_init()) {
        int sfd = mpp_buffer_get_fd(buf);
        int dfd = mpp_buffer_get_fd(s->priv_buf);
        size_t need_src = (size_t)src_hs * src_vs * 3 / 2;
        size_t need_dst = (size_t)dst_hs * dst_vs * 3 / 2;
        /* Only describe what the buffers really hold: an over-described rect
           makes the RGA fail to map the buffer ("src channel map job buffer
           failed") and we would rather take the CPU path than that. */
        if (sfd > 0 && dfd > 0 && mpp_buffer_get_size(buf) >= need_src &&
            mpp_buffer_get_size(s->priv_buf) >= need_dst &&
            rga3_stride_ok(src_hs, src_vs, false) && rga3_stride_ok(dst_hs, dst_vs, false)) {
            rga_info_t rs = {0}, rd = {0};
            rs.fd = sfd; rs.mmuFlag = 1;
            rga_set_rect(&rs.rect, 0, 0, copy_w, copy_h, src_hs, src_vs, RK_FORMAT_YCbCr_420_SP);
            rd.fd = dfd; rd.mmuFlag = 1; rd.core = RGA3_CORES;
            rga_set_rect(&rd.rect, 0, 0, copy_w, copy_h, dst_hs, dst_vs, RK_FORMAT_YCbCr_420_SP);
            int fence = rga_read_fence(dfd);
            rd.in_fence_fd = fence;
            if (c_RkRgaBlit(&rs, &rd, NULL) == 0)
                copied = 2;   /* 2 = RGA hardware blit (fenced) */
            rga_fence_done(fence);
        }
    }
#endif
#ifdef HAVE_RGA
    /* 10-bit: NV15 -> P010 on RGA3 instead of the CPU repack below.  RGA3
       writes true P010 when the destination is described as the SAME format
       as the packed source (RK_FORMAT_YCbCr_420_SP_10B) with the legacy
       rga_info_t 10-bit flags set (is_10b_compact = is_10b_endian = 1) --
       librga's RK_FORMAT_P010 is rejected by the hardware, which is how this
       was once misread as "closed by silicon".  Strides are BYTE pitches on
       both sides (packed 10-bit plane, and 2 bytes per P010 sample).  Verified
       bit-exact against nv15_row_to_p010() on RK3588 (3840x2160 and 1920x1080:
       0 mismatches; 3.8 ms/frame at 4K vs the CPU on the decode thread).
       RKVA_RGA_P010=0 forces the CPU repack. */
    if (!copied && !rga_off && i10 && buf && s->priv_buf) {
        const char *sw = getenv("RKVA_RGA_P010");
        if (!(sw && sw[0] == '0')) {
            int sfd = mpp_buffer_get_fd(buf);
            int dfd = mpp_buffer_get_fd(s->priv_buf);
            int src_bs = src_hs;
            int min_bs = (copy_w * 10 + 7) / 8;
            if (src_bs < min_bs) src_bs = ((src_hs * 10 + 7) / 8 + 63) & ~63;
            size_t need_src = (size_t)src_bs * src_vs * 3 / 2;
            size_t need_dst = (size_t)dst_hs * 2 * dst_vs * 3 / 2;
            if (rga_legacy_init() && sfd > 0 && dfd > 0 &&
                mpp_buffer_get_size(buf) >= need_src && mpp_buffer_get_size(s->priv_buf) >= need_dst &&
                rga3_stride_ok(src_bs, src_vs, true) && rga3_stride_ok(dst_hs * 2, dst_vs, true)) {
                rga_info_t rs = {0}, rd = {0};
                rs.fd = sfd; rs.mmuFlag = 1;
                rga_set_rect(&rs.rect, 0, 0, copy_w, copy_h, src_bs, src_vs,
                             RK_FORMAT_YCbCr_420_SP_10B);
                rd.fd = dfd; rd.mmuFlag = 1; rd.core = RGA3_CORES;
                rd.is_10b_compact = 1; rd.is_10b_endian = 1;
                rga_set_rect(&rd.rect, 0, 0, copy_w, copy_h, dst_hs * 2, dst_vs,
                             RK_FORMAT_YCbCr_420_SP_10B);
                int fence = rga_read_fence(dfd);
                rd.in_fence_fd = fence;
                int ret = c_RkRgaBlit(&rs, &rd, NULL);
                rga_fence_done(fence);
                if (ret == 0) {
                    copied = 4;   /* 4 = RGA NV15->P010 hardware blit */
                    /* Same debug valve as the CPU repack: dump ONE P010 frame
                       (RKVA_DUMP=<file>) so the two lanes can be cmp'd. */
                    static int dumped_rga = 0;
                    const char *dump = getenv("RKVA_DUMP");
                    if (dump && !dumped_rga && dst) {
                        FILE *df = fopen(dump, "wb");
                        if (df) {
                            const uint8_t *dp = (const uint8_t *)dst;
                            fwrite(dp, 1, (size_t)dst_hs * 2 * dst_vs, df);
                            fwrite(dp + (size_t)dst_hs * 2 * dst_vs, 1, (size_t)dst_hs * 2 * (dst_vs / 2), df);
                            fclose(df);
                            dumped_rga = 1;
                            LOG("rga: dumped P010 frame %dx%d stride=%d to %s", copy_w, copy_h, dst_hs, dump);
                        }
                    }
                } else {
                    static int warned = 0;
                    if (!warned) { LOG("RGA NV15->P010 blit failed ret=%d, falling back to CPU repack", ret); warned = 1; }
                }
            }
        }
    }
#endif
    if (!copied && src && dst && i10) {
        /* Deep Ink: repack NV15 -> true P010 (see nv15_row_to_p010 above).
         * For 10-bit frames MPP's hor_stride is a BYTE stride of the packed
         * plane.  Defensive: if it looks like a sample count instead (older
         * kernels), derive the packed byte stride from it. */
        int src_bs = src_hs;
        int min_bs = (copy_w * 10 + 7) / 8;
        if (src_bs < min_bs) src_bs = ((src_hs * 10 + 7) / 8 + 63) & ~63;
        const uint8_t *sy = (const uint8_t *)src;
        uint8_t       *dy = (uint8_t       *)dst;
        for (int r = 0; r < copy_h; r++)
            nv15_row_to_p010(sy + (size_t)r * src_bs,
                             (uint16_t *)(dy + (size_t)r * dst_hs * 2), copy_w);
        const uint8_t *su = sy + (size_t)src_bs * src_vs;
        uint8_t       *du = dy + (size_t)dst_hs * 2 * dst_vs;
        for (int r = 0; r < copy_h / 2; r++)
            nv15_row_to_p010(su + (size_t)r * src_bs,
                             (uint16_t *)(du + (size_t)r * dst_hs * 2), copy_w);
        copied = 3;   /* 3 = NV15->P010 CPU repack */
        /* Deep Ink debug valve: dump ONE repacked frame as raw P010 for
         * GL-free verification (ffmpeg -f rawvideo -pix_fmt p010le). */
        static int dumped = 0;
        const char *dump = getenv("RKVA_DUMP");
        if (dump && !dumped) {
            FILE *df = fopen(dump, "wb");
            if (df) {
                fwrite(dy, 1, (size_t)dst_hs * 2 * dst_vs, df);          /* Y  */
                fwrite(du, 1, (size_t)dst_hs * 2 * (dst_vs / 2), df);    /* UV */
                fclose(df);
                dumped = 1;
                LOG("deepink: dumped P010 frame %dx%d stride=%d to %s",
                    copy_w, copy_h, dst_hs, dump);
            }
        }
    }
    if (!copied && src && dst) {
        const uint8_t *sy = (const uint8_t *)src;
        uint8_t       *dy = (uint8_t       *)dst;
        int row_bytes = (copy_w < dst_hs ? copy_w : dst_hs) * bpp;
        if (row_bytes > src_hs * bpp) row_bytes = src_hs * bpp;
        for (int r = 0; r < copy_h; r++)
            memcpy(dy + (size_t)r * dst_hs * bpp,
                   sy + (size_t)r * src_hs * bpp,
                   (size_t)row_bytes);
        const uint8_t *su = sy + (size_t)src_hs * src_vs * bpp;
        uint8_t       *du = dy + (size_t)dst_hs * dst_vs * bpp;
        for (int r = 0; r < copy_h / 2; r++)
            memcpy(du + (size_t)r * dst_hs * bpp,
                   su + (size_t)r * src_hs * bpp,
                   (size_t)row_bytes);
        copied = 1;
    }
    *out_w = copy_w; *out_h = copy_h;
    return copied;
}

static bool zc_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RKVA_ZERO_COPY"); v = (e && e[0] == '0') ? 0 : 1; }
    return v == 1;
}

/* Copy a held zero-copy frame into priv_buf for a reader of the surface's own buffer; release the frame unless the
 * client was handed MPP's buffer (keep_exported).  Caller must not hold s->lock. */
static void surface_zc_materialize(RKDriver *d, RKSurface *s, bool keep_exported) {
    MppFrame drop = NULL;
    pthread_mutex_lock(&s->lock);
    if (s->zc_frame) {
        int w = 0, h = 0;
        long long t0 = rk_now_us();
        int copied = rk_copy_into_surface(s->zc_frame, s, (VASurfaceID)(SURFACE_ID_BASE + (unsigned)(s - d->surfaces)), &w, &h);
        if (w > 0) s->width = w;
        if (h > 0) s->height = h;
        LOG("zero-copy: surface=0x%x read without an export: copied=%d copy_us=%lld%s",
            (unsigned)(SURFACE_ID_BASE + (unsigned)(s - d->surfaces)), copied, rk_now_us() - t0,
            (keep_exported && s->zc_exported) ? " (frame kept: exported)" : "");
        if (!(keep_exported && s->zc_exported)) { drop = s->zc_frame; s->zc_frame = NULL; s->zc_exported = false; }
    }
    pthread_mutex_unlock(&s->lock);
    if (drop) mpp_frame_deinit(&drop);
}

/* The client renders into the surface again / destroys it: drop the frames it held.  Caller holds s->lock;
 * the frames are returned for deinit after the unlock. */
static void surface_zc_take_locked(RKSurface *s, MppFrame *a, MppFrame *b) {
    *a = s->zc_frame; *b = s->zc_prev;
    s->zc_frame = NULL; s->zc_prev = NULL; s->zc_exported = false;
}
static void zc_drop2(MppFrame a, MppFrame b) { if (a) mpp_frame_deinit(&a); if (b) mpp_frame_deinit(&b); }

/* Caller must hold c->out_lock: this pops c->decode_queue. */
static void assign_mpp_frame(MppFrame frame, RKContext *c, RKDriver *d)
{
    if (mpp_frame_get_info_change(frame)) {
        LOG("assign_mpp_frame: info_change → acknowledged, render_target=0x%x",
            (unsigned)c->render_target);
        c->mpi->control(c->mpp, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        mpp_frame_deinit(&frame);
        return;
    }

    RK_S64      raw_pts = mpp_frame_get_pts(frame);
    VASurfaceID sid     = (VASurfaceID)raw_pts;
    RKSurface  *s       = sid ? surface_by_id(d, sid) : NULL;

    if (!s) {
        if (c->dq_head != c->dq_tail) {
            sid = c->decode_queue[c->dq_head];
            c->dq_head = (c->dq_head + 1) & 63;
            LOG("assign_mpp_frame: PTS=0x%llx unmapped, FIFO → surface=0x%x",
                (unsigned long long)raw_pts, (unsigned)sid);
        }
        s = sid ? surface_by_id(d, sid) : NULL;
    } else {
        /* PTS valid — advance FIFO head only if this surface is at the front */
        if (c->dq_head != c->dq_tail && c->decode_queue[c->dq_head] == sid)
            c->dq_head = (c->dq_head + 1) & 63;
    }

    if (!s) {
        LOG("assign_mpp_frame: PTS=0x%llx surface not found, dropped",
            (unsigned long long)raw_pts);
        mpp_frame_deinit(&frame);
        return;
    }
    {   /* AV1 packets carry the surface generation in the PTS high word (0 = untagged, other codecs) */
        uint32_t pgen = (uint32_t)((uint64_t)raw_pts >> 32);
        if (pgen && (VASurfaceID)raw_pts == sid && pgen != s->gen) {
            LOG("assign_mpp_frame: surface=0x%x output for an earlier picture (gen %u, surface now %u) dropped",
                (unsigned)sid, pgen, s->gen);
            mpp_frame_deinit(&frame);
            return;
        }
    }

    MppBuffer      buf    = mpp_frame_get_buffer(frame);
    int            fwidth = (int)mpp_frame_get_width(frame);
    int            fheight= (int)mpp_frame_get_height(frame);
    int            fhs    = (int)mpp_frame_get_hor_stride(frame);
    int            fvs    = (int)mpp_frame_get_ver_stride(frame);
    MppFrameFormat ffmt   = mpp_frame_get_fmt(frame);

    /* 8-bit zero-copy: keep MPP's frame (see RKSurface.zc_frame) */
    int zfd = buf ? mpp_buffer_get_fd(buf) : -1;
    if (zc_enabled() && __atomic_load_n(&c->zc_ok, __ATOMIC_ACQUIRE) &&
        !MPP_FRAME_FMT_IS_YUV_10BIT(ffmt) && (ffmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP &&
        zfd > 0 && fwidth > 0 && fheight > 0 && fhs >= fwidth && fvs >= fheight &&
        /* invariant 2: an exported pitch is 64-aligned (a 16-aligned one green-screens the RK3588 EGL importer,
           REPRO.md).  MPP aligns AV1/H.264/VP8 frames up to 1920 wide to 16 only (854 -> 864, 426 -> 432), so those
           take the copy into the 64-aligned surface buffer.  With fhs % 64 == 0 the chroma plane's offset
           (fhs * fvs) and pitch are 64-aligned too. */
        (fhs % 64) == 0 &&
        fwidth <= (s->hstride ? s->hstride : s->width) && fheight <= (s->vstride ? s->vstride : s->height) &&
        mpp_buffer_get_size(buf) >= (size_t)fhs * fvs * 3 / 2) {
        MppFrame drop = NULL;
        pthread_mutex_lock(&s->lock);
        if (s->zc_frame) {                       /* a second output for this picture (should not happen) */
            if (s->zc_exported) { drop = s->zc_prev; s->zc_prev = s->zc_frame; }   /* the client may be showing it */
            else drop = s->zc_frame;
            LOG("assign_mpp_frame: surface=0x%x already held a frame (exported=%d)", (unsigned)sid, s->zc_exported);
        }
        s->zc_frame = frame; s->zc_fd = zfd; s->zc_hs = fhs; s->zc_vs = fvs; s->zc_size = mpp_buffer_get_size(buf);
        s->zc_exported = false;
        s->frame  = NULL;
        s->fmt    = ffmt;
        s->width  = fwidth;
        s->height = fheight;
        s->decoded = true;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (drop) mpp_frame_deinit(&drop);
        LOG("assign_mpp_frame: surface=0x%x MPP %dx%d stride=%dx%d fmt=0x%x zero-copy fd=%d",
            (unsigned)sid, fwidth, fheight, fhs, fvs, (unsigned)ffmt, zfd);
        return;
    }

    int copy_w = 0, copy_h = 0;
    long long t_copy0 = rk_now_us();
    int copied = rk_copy_into_surface(frame, s, sid, &copy_w, &copy_h);
    long long t_copy1 = rk_now_us();
    mpp_frame_deinit(&frame);

    pthread_mutex_lock(&s->lock);
    s->frame  = NULL;
    s->fmt    = ffmt;
    if (MPP_FRAME_FMT_IS_YUV_10BIT(ffmt) != s->rt_10bit)
        LOG("assign_mpp_frame: WARNING surface=0x%x created as %d-bit but decoded frame is %d-bit — "
            "a client that exported before decode holds a stale descriptor",
            sid, s->rt_10bit ? 10 : 8, MPP_FRAME_FMT_IS_YUV_10BIT(ffmt) ? 10 : 8);
    if (fwidth  > 0) s->width   = copy_w;   /* clamped above: never beyond the exported layout */
    if (fheight > 0) s->height  = copy_h;
    /* hstride/vstride are deliberately NOT updated from the MPP frame: they
       describe the layout already handed to the client by ExportSurfaceHandle.
       Changing them would desynchronise the importer's view of the buffer.
       The copy above re-strides into this fixed layout. */
    s->decoded  = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    LOG("assign_mpp_frame: surface=0x%x prime_fd=%d MPP %dx%d stride=%dx%d fmt=0x%x copied=%d copy_us=%lld",
        (unsigned)sid, s->prime_fd, fwidth, fheight, fhs, fvs, (unsigned)ffmt, copied, t_copy1 - t_copy0);
}

/* Output thread.  It blocks in MPP's output queue (MPP_SET_OUTPUT_TIMEOUT) and routes every frame as soon as MPP
 * emits it, under out_lock; vaSyncSurface then only waits on the surface's condition variable (event-driven, no 1 ms
 * poll), and no thread ever blocks inside MPP while holding out_lock -- a blocking wait under out_lock starved the AV1
 * submit thread and EndPicture of that lock (3 s sync timeout with ffmpeg's DRM-map path).  While it runs it is the
 * only caller of decode_get_frame.  RKVA_SYNC_POLL=1 keeps the old way (callers drain, sync polls every 1 ms). */
static int sync_poll_mode(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RKVA_SYNC_POLL"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}
typedef struct { RKContext *c; RKDriver *d; } RkOutArg;
static void *out_thread_main(void *arg) {
    RKContext *c = ((RkOutArg *)arg)->c;
    RKDriver  *d = ((RkOutArg *)arg)->d;
    free(arg);
    while (!__atomic_load_n(&c->out_stop, __ATOMIC_ACQUIRE)) {
        MppFrame f = NULL;
        MPP_RET r = c->mpi->decode_get_frame(c->mpp, &f);   /* blocks up to the output timeout */
        if (r == MPP_OK && f) {
            pthread_mutex_lock(&c->out_lock);
            assign_mpp_frame(f, c, d);
            pthread_mutex_unlock(&c->out_lock);
        } else if (r != MPP_OK && r != MPP_ERR_TIMEOUT) {
            usleep(1000);                                     /* unexpected error: do not spin */
        }
    }
    return NULL;
}
static void out_thread_start(RKContext *c, RKDriver *d) {
    if (sync_poll_mode() || c->out_started) return;
    MppPollType to = 20;                                      /* ms; bounds how long a stop request waits */
    if (c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&to) != MPP_OK) { LOG("output thread: MPP refused the output timeout, polling"); return; }
    RkOutArg *a = malloc(sizeof *a);
    if (!a) goto fail;
    a->c = c; a->d = d;
    c->out_stop = false;
    if (pthread_create(&c->out_thread, NULL, out_thread_main, a) != 0) { free(a); goto fail; }
    c->out_started = true;
    return;
fail:
    to = MPP_POLL_NON_BLOCK;
    c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&to);
    LOG("output thread: not started, polling");
}
/* Caller must NOT hold out_lock (the thread takes it). */
static void out_thread_stop(RKContext *c) {
    if (!c->out_started) return;
    __atomic_store_n(&c->out_stop, true, __ATOMIC_RELEASE);
    pthread_join(c->out_thread, NULL);
    c->out_started = false; c->out_stop = false;
    /* back to non-blocking at once: a vaSyncSurface that now takes the polling branch calls decode_get_frame under
       out_lock and must not wait up to the thread's 20 ms in MPP there */
    MppPollType nb = MPP_POLL_NON_BLOCK;
    c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&nb);
}

/* The PTS a packet carries: the target surface in the low word, its generation (bumped at every BeginPicture) in the
   high word, so assign_mpp_frame can drop an output that arrives after the client re-targeted the surface. */
static RK_S64 rk_surface_pts(RKDriver *d, VASurfaceID sid) {
    RKSurface *t = surface_by_id(d, sid);
    uint32_t gen = t ? __atomic_load_n(&t->gen, __ATOMIC_RELAXED) : 0;
    return (RK_S64)(((uint64_t)gen << 32) | (uint64_t)sid);
}

/* Build Annex B bitstream from VA-API buffers and send to MPP */
static VAStatus do_h264_decode(RKContext *c, RKDriver *d)
{
    /* gather slice data */
    uint8_t *pkt_data = NULL;
    size_t   pkt_cap  = 0;
    size_t   pkt_sz   = 0;

#define PKT_APPEND(ptr, len) do {                           \
    size_t _l = (len);                                      \
    if (pkt_sz + _l > pkt_cap) {                           \
        pkt_cap = (pkt_sz + _l) * 2 + 4096;               \
        pkt_data = realloc(pkt_data, pkt_cap);             \
        if (!pkt_data) return VA_STATUS_ERROR_ALLOCATION_FAILED; \
    }                                                       \
    memcpy(pkt_data + pkt_sz, (ptr), _l);                 \
    pkt_sz += _l;                                          \
} while (0)

    /* Classify the picture: is it an IDR (SPS+PPS go ahead of it)?  And what do
       the slices say the PPS reference-count defaults are?  An override-free P/B
       slice inherits the PPS defaults, and ffmpeg's per-slice active counts are
       then exactly those defaults -- the only place VA-API lets us see them. */
    bool is_idr = false, seen_first = false;
    const VAPictureParameterBufferH264 *hpp = &c->last_pp;
    const VASliceParameterBufferH264 *last_sp = NULL;
    int learned_l0 = c->h264_l0_minus1, learned_l1 = c->h264_l1_minus1;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (!b) continue;
        if (b->type == VASliceParameterBufferType) { last_sp = (const VASliceParameterBufferH264 *)b->data; continue; }
        if (b->type != VASliceDataBufferType) continue;
        const uint8_t *nal = (const uint8_t *)b->data;
        size_t len = (size_t)b->size * b->num_elements;
        if (!seen_first) { is_idr = ((nal[0] & 0x1F) == 5); seen_first = true; }
        int st5 = -1;
        if (h264_peek_ref_override(nal, len, hpp, &st5) == 0 && last_sp) {
            learned_l0 = last_sp->num_ref_idx_l0_active_minus1;
            if (st5 == 1) learned_l1 = last_sp->num_ref_idx_l1_active_minus1;
        }
    }
    bool pps_dirty = false;
    if (learned_l0 != c->h264_l0_minus1 || learned_l1 != c->h264_l1_minus1) {
        LOG("do_h264_decode: PPS num_ref_idx defaults learned from an override-free slice: "
            "L0 %d -> %d, L1 %d -> %d (re-emitting PPS)",
            c->h264_l0_minus1 + 1, learned_l0 + 1, c->h264_l1_minus1 + 1, learned_l1 + 1);
        c->h264_l0_minus1 = learned_l0; c->h264_l1_minus1 = learned_l1; pps_dirty = true;
    }
    if (getenv("RKVA_TRACE_H264") && (is_idr || !c->sps_sent))
        LOG("h264 sps/pps: poc_type=%u log2_poc_lsb=%u frame_mbs_only=%u direct8x8=%u gaps_allowed=%u log2_mfn=%u "
            "num_ref_frames=%u | entropy=%u weighted_pred=%u bipred_idc=%u t8x8=%u deblock_ctrl=%u pps_l0=%d pps_l1=%d",
            hpp->seq_fields.bits.pic_order_cnt_type, hpp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4,
            hpp->seq_fields.bits.frame_mbs_only_flag, hpp->seq_fields.bits.direct_8x8_inference_flag,
            hpp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, hpp->seq_fields.bits.log2_max_frame_num_minus4 + 4,
            (unsigned)hpp->num_ref_frames, hpp->pic_fields.bits.entropy_coding_mode_flag,
            hpp->pic_fields.bits.weighted_pred_flag, hpp->pic_fields.bits.weighted_bipred_idc,
            hpp->pic_fields.bits.transform_8x8_mode_flag, hpp->pic_fields.bits.deblocking_filter_control_present_flag,
            c->h264_l0_minus1 + 1, c->h264_l1_minus1 + 1);

    /* SPS + PPS before every IDR (or first frame); PPS alone whenever the learned
       reference-count defaults change -- a PPS update between pictures does not
       disturb the DPB (unlike an SPS re-activation, which flushes it). */
    if (is_idr || !c->sps_sent || pps_dirty) {
        uint8_t hdr[512];
        if (is_idr || !c->sps_sent) {
            int n = h264_write_sps(hdr, sizeof(hdr), &c->last_pp,
                                   profile_idc(c->profile));
            if (n > 0) { PKT_APPEND(hdr, (size_t)n); }
        }
        int n = h264_write_pps(hdr, sizeof(hdr), &c->last_pp, c->h264_l0_minus1, c->h264_l1_minus1);
        if (n > 0) { PKT_APPEND(hdr, (size_t)n); }
        c->sps_sent = true;
    }

    /* append each slice with Annex B start code */
    static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (!b || b->type != VASliceDataBufferType) continue;
        PKT_APPEND(sc, 4);
        PKT_APPEND(b->data, (size_t)b->size * b->num_elements);
    }
#undef PKT_APPEND

    if (!pkt_sz) {
        LOG("do_h264_decode: no slice data, marking surface 0x%x decoded",
            (unsigned)c->render_target);
        free(pkt_data);
        RKSurface *tgt = surface_by_id(d, c->render_target);
        if (tgt) {
            pthread_mutex_lock(&tgt->lock);
            tgt->decoded = true;
            pthread_cond_broadcast(&tgt->cond);
            pthread_mutex_unlock(&tgt->lock);
        }
        return VA_STATUS_SUCCESS;
    }

    /* Pre-drain: consume frames MPP already has ready from previous packets */
    pthread_mutex_lock(&c->out_lock);
    {
        MppFrame f = NULL;
        while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &f) == MPP_OK && f) {
            assign_mpp_frame(f, c, d);
            f = NULL;
        }
    }

    /* Enqueue this surface for PTS-routing fallback, exactly as the generic
     * path does.  H.264 used to skip the queue and fall back to render_target,
     * which is only correct when output order matches submit order: with
     * B-frames a decoded picture would land in whichever surface was submitted
     * most recently, producing ghosted frames. */
    c->decode_queue[c->dq_tail] = c->render_target;
    c->dq_tail = (c->dq_tail + 1) & 63;
    pthread_mutex_unlock(&c->out_lock);

    /* Debug valve: RKVA_DUMP_H264=<path> appends every assembled H.264 packet so
       `ffmpeg -f h264 -i <path> -f null -` (or a hybrid with the original's
       SPS/PPS) can judge our parameter sets before MPP has to. */
    { const char *dump = getenv("RKVA_DUMP_H264");
      if (dump) { FILE *df = fopen(dump, "ab"); if (df) { fwrite(pkt_data, 1, pkt_sz, df); fclose(df); } } }
    LOG("do_h264_decode: sending %zu bytes target=0x%x", pkt_sz, (unsigned)c->render_target);
    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, pkt_data, pkt_sz);
    mpp_packet_set_length(pkt, pkt_sz);
    mpp_packet_set_pts(pkt, rk_surface_pts(d, c->render_target));

    /* MPP_ERR_BUFFER_FULL is backpressure, not a stream error: the decoder's
       input queue is full because output frames have not been drained yet.
       Drain the output queue and retry rather than discarding the packet. */
    MPP_RET ret = MPP_OK;
    for (int attempt = 0; attempt < 200; attempt++) {
        ret = c->mpi->decode_put_packet(c->mpp, pkt);
        if (ret == MPP_OK || ret != MPP_ERR_BUFFER_FULL)
            break;
        MppFrame df = NULL;
        pthread_mutex_lock(&c->out_lock);
        while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &df) == MPP_OK && df) {
            assign_mpp_frame(df, c, d);
            df = NULL;
        }
        pthread_mutex_unlock(&c->out_lock);
        usleep(500);
    }
    mpp_packet_deinit(&pkt);
    free(pkt_data);

    if (ret != MPP_OK) {
        LOG("decode_put_packet failed after retries: %d", ret);
        /* undo enqueue, as the generic path does, and only if our entry is still the newest */
        pthread_mutex_lock(&c->out_lock);
        int last = (c->dq_tail - 1) & 63;
        if (c->dq_tail != c->dq_head && c->decode_queue[last] == c->render_target)
            c->dq_tail = last;
        pthread_mutex_unlock(&c->out_lock);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    /* Do NOT block waiting for THIS surface to come out.  With B-frames MPP
       cannot emit it until later frames have been submitted, so the wait always
       runs to its limit: 100 tries x 1ms = ~100ms per frame, i.e. ~10fps, and
       1080p ends up decoding SLOWER than 4K.  Measured on a 1080p29.97 stream
       with has_b_frames=2: 0.56x realtime, versus 4.5x in software.
       Waiting is vaSyncSurface's job, and rk_SyncSurface() already drains MPP
       with a 3s deadline.  Here, just collect whatever is already available. */
    MppFrame frame = NULL;
    pthread_mutex_lock(&c->out_lock);
    while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &frame) == MPP_OK && frame) {
        assign_mpp_frame(frame, c, d);
        frame = NULL;
    }
    pthread_mutex_unlock(&c->out_lock);

    return VA_STATUS_SUCCESS;
}

/* Return false for VP9 altref / non-displayed frames (show_frame=0).
 * These are decoded by MPP internally as references but never output via
 * decode_get_frame — polling for them stalls the thread for the full timeout.
 * VP9 uncompressed header layout (MSB-first):
 *   [7:6] frame_marker=0b10  [5] profile_low  [4] profile_high
 *   profile≠3: [3] show_existing  [2] frame_type  [1] show_frame
 *   profile=3:  [3] reserved       [2] show_existing [1] frame_type [0] show_frame */
static bool vp9_show_frame(const uint8_t *data, size_t len)
{
    if (!data || len < 1) return true;
    uint8_t b = data[0];
    if ((b >> 6) != 2) return true;                   /* bad frame_marker, assume shown */
    int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    if (profile != 3) {
        if ((b >> 3) & 1) return true;                /* show_existing_frame */
        return (b >> 1) & 1;                          /* show_frame */
    } else {
        if ((b >> 2) & 1) return true;                /* show_existing_frame (profile 3) */
        return b & 1;                                 /* show_frame (profile 3) */
    }
}

/* For VP9 / HEVC / AV1: slice data is already a complete coded picture */
static VAStatus do_generic_decode(RKContext *c, RKDriver *d)
{
    uint8_t *pkt_data = NULL;
    size_t   pkt_sz   = 0;
    size_t   pkt_cap  = 0;

#define PKT_APPEND(ptr, len) do {                           \
    size_t _l = (len);                                      \
    if (pkt_sz + _l > pkt_cap) {                           \
        pkt_cap = (pkt_sz + _l) * 2 + 4096;               \
        pkt_data = realloc(pkt_data, pkt_cap);             \
        if (!pkt_data) return VA_STATUS_ERROR_ALLOCATION_FAILED; \
    }                                                       \
    memcpy(pkt_data + pkt_sz, (ptr), _l);                 \
    pkt_sz += _l;                                          \
} while (0)

    /* HEVC (Deep Ink phase 1.5): MPP is a bitstream decoder and VA-API never
       hands us the VPS/SPS/PPS bytes -- only the parsed fields.  Re-synthesise
       them (hevc.c) ahead of the slices on every IRAP (and the first frame),
       and give each slice NALU its Annex B start code.  Without this MPP
       parsed nothing and returned zero-filled buffers: the solid green. */
    bool hevc = (c->coding == MPP_VIDEO_CodingHEVC);
    int  n_slices = 0;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (b && b->type == VASliceDataBufferType) n_slices++;
    }
    if (hevc && n_slices) {
        const VAPictureParameterBufferHEVC *hp = &c->last_pp_hevc;
        bool irap = hp->slice_parsing_fields.bits.RapPicFlag;
        /* How many short-term RPS sets did the encoder's SPS declare?  Only the
           count CLASS matters to us (0 vs >=1): a slice-inline RPS parses as
           st_ref_pic_set(num_sets), and that carries an
           inter_ref_pic_set_prediction_flag iff the index is non-zero -- get the
           class wrong and every slice header shifts by one bit.
           VA-API hands us the real number in pp->num_short_term_ref_pic_sets, so
           it is known at the very first picture.  Phase 1.5 guessed and then
           corrected itself by re-emitting a DIFFERENT SPS mid-stream; re-activating
           an SPS flushes the DPB, which destroys the earlier references a B-frame
           needs ("Could not find ref with POC 0") and dropped every reordered
           stream to software.  Decide once, here, and never change it. */
        int want_plus1 = (hp->num_short_term_ref_pic_sets > 0) ? 2 : 1;
        if (c->hevc_sets_plus1 == 0) {
            c->hevc_sets_plus1 = want_plus1;
            /* Cross-check against the slice-header probe on the first picture that
               actually carries an inline RPS; disagreement is worth knowing about. */
            if (hp->st_rps_bits > 0) {
                for (int i = 0; i < c->n_pending; i++) {
                    RKBuffer *b = buffer_by_id(d, c->pending[i]);
                    if (!b || b->type != VASliceDataBufferType) continue;
                    int det = hevc_detect_num_st_rps(b->data, (size_t)b->size * b->num_elements, hp);
                    if (det >= 0 && (det + 1) != want_plus1)
                        LOG("do_hevc: NOTE pp->num_short_term_ref_pic_sets=%u implies %d sets, "
                            "slice probe says %d (trusting pp)",
                            (unsigned)hp->num_short_term_ref_pic_sets, want_plus1 - 1, det);
                    else if (det == -2)
                        LOG("do_hevc: WARNING slice uses SPS-level RPS sets or inter-RPS "
                            "prediction -> not reconstructible from VA-API");
                    break;
                }
            }
        }
        if (irap || !c->sps_sent) {
            int main10 = hp->bit_depth_luma_minus8 > 0;
            uint8_t hdr[1024];
            int n;
            n = hevc_write_vps(hdr, sizeof(hdr), hp, main10);                 if (n > 0) PKT_APPEND(hdr, (size_t)n);
            n = hevc_write_sps(hdr, sizeof(hdr), hp, main10, want_plus1 - 1); if (n > 0) PKT_APPEND(hdr, (size_t)n);
            n = hevc_write_pps(hdr, sizeof(hdr), hp);                         if (n > 0) PKT_APPEND(hdr, (size_t)n);
            c->sps_sent = true; c->hevc_sent_plus1 = want_plus1;
            LOG("do_hevc: VPS/SPS/PPS synthesised (%zu bytes) %dx%d main10=%d irap=%d st_rps=%d st_rps_bits=%u",
                pkt_sz, hp->pic_width_in_luma_samples, hp->pic_height_in_luma_samples,
                main10, (int)irap, want_plus1 - 1, (unsigned)hp->st_rps_bits);
        }
    }
    static const uint8_t hevc_sc[4] = {0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (!b || b->type != VASliceDataBufferType) continue;
        if (hevc) PKT_APPEND(hevc_sc, 4);
        PKT_APPEND(b->data, (size_t)b->size * b->num_elements);
    }
#undef PKT_APPEND

    if (!pkt_sz) {
        LOG("do_generic_decode: no slice data, marking surface 0x%x decoded",
            (unsigned)c->render_target);
        free(pkt_data);
        RKSurface *tgt = surface_by_id(d, c->render_target);
        if (tgt) {
            pthread_mutex_lock(&tgt->lock);
            tgt->decoded = true;
            pthread_cond_broadcast(&tgt->cond);
            pthread_mutex_unlock(&tgt->lock);
        }
        return VA_STATUS_SUCCESS;
    }

    /* Detect VP9 altref (show_frame=0): MPP decodes them as references but does
     * NOT output them via decode_get_frame in this environment, so we never
     * poll/block for them — see the is_hidden handling after decode_put_packet. */
    bool is_hidden = (c->coding == MPP_VIDEO_CodingVP9) &&
                     !vp9_show_frame(pkt_data, pkt_sz);

    /* Pre-drain: consume any frames MPP already has ready from previous
     * packets.  Keyframes that timed out in the previous EndPicture poll
     * window land here at the start of the next call. */
    pthread_mutex_lock(&c->out_lock);
    {
        MppFrame f = NULL;
        while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &f) == MPP_OK && f) {
            assign_mpp_frame(f, c, d);
            f = NULL;
        }
    }

    /* Enqueue this surface into the FIFO decode queue (PTS-routing fallback).
     * Altref frames are skipped: MPP never outputs them, so enqueuing would
     * leave a permanent entry the head can never advance past. */
    if (!is_hidden) {
        c->decode_queue[c->dq_tail] = c->render_target;
        c->dq_tail = (c->dq_tail + 1) & 63;
    }
    pthread_mutex_unlock(&c->out_lock);

    /* Debug valve: RKVA_DUMP_HEVC=<path> appends every assembled HEVC packet
       so `ffmpeg -f hevc -i <path> -f null -` can name a broken syntax element. */
    if (hevc) {
        const char *dump = getenv("RKVA_DUMP_HEVC");
        if (dump) { FILE *df = fopen(dump, "ab"); if (df) { fwrite(pkt_data, 1, pkt_sz, df); fclose(df); } }
    }
    LOG("do_generic_decode: sending %zu bytes to MPP (coding=%d) target=0x%x%s",
        pkt_sz, (int)c->coding, (unsigned)c->render_target,
        is_hidden ? " [altref]" : "");
    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, pkt_data, pkt_sz);
    mpp_packet_set_length(pkt, pkt_sz);
    mpp_packet_set_pts(pkt, rk_surface_pts(d, c->render_target));

    /* MPP_ERR_BUFFER_FULL is backpressure, not a stream error: the decoder's
       input queue is full because output frames have not been drained yet.
       Drain the output queue and retry rather than discarding the packet. */
    MPP_RET ret = MPP_OK;
    for (int attempt = 0; attempt < 200; attempt++) {
        ret = c->mpi->decode_put_packet(c->mpp, pkt);
        if (ret == MPP_OK || ret != MPP_ERR_BUFFER_FULL)
            break;
        MppFrame df = NULL;
        pthread_mutex_lock(&c->out_lock);
        while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &df) == MPP_OK && df) {
            assign_mpp_frame(df, c, d);
            df = NULL;
        }
        pthread_mutex_unlock(&c->out_lock);
        usleep(500);
    }
    mpp_packet_deinit(&pkt);
    free(pkt_data);

    if (ret != MPP_OK) {
        LOG("decode_put_packet failed after retries: %d", ret);
        if (!is_hidden) {                                  /* undo enqueue */
            /* Only if our entry is still the newest: a retry-loop drain here or in vaSyncSurface may already
               have popped it, and stepping the tail back past the head would make the queue look 63 deep. */
            pthread_mutex_lock(&c->out_lock);
            int last = (c->dq_tail - 1) & 63;
            if (c->dq_tail != c->dq_head && c->decode_queue[last] == c->render_target)
                c->dq_tail = last;
            pthread_mutex_unlock(&c->out_lock);
        }
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    /* Altref frames (show_frame=0): MPP decodes them internally as references
     * but never outputs them via decode_get_frame here, and ffmpeg never syncs
     * or displays a hidden frame directly.  So do NOT poll — that would block
     * the decode thread for the whole poll window per altref, accumulating
     * latency until Firefox's pipeline underruns (NS_ERROR_DOM_MEDIA_FATAL_ERR).
     * Just mark the surface decoded immediately; its permanent prime_fd stays
     * valid (placeholder content) so ExportSurfaceHandle always succeeds. */
    if (is_hidden) {
        RKSurface *tgt = surface_by_id(d, c->render_target);
        if (tgt) {
            pthread_mutex_lock(&tgt->lock);
            tgt->decoded = true;
            pthread_cond_broadcast(&tgt->cond);
            pthread_mutex_unlock(&tgt->lock);
        }
        return VA_STATUS_SUCCESS;
    }

    /* Do NOT poll here — return immediately so Firefox's decode thread is never
     * stalled. 4K keyframes (835KB) can take >1.6s in MPP; blocking EndPicture
     * for that long freezes Firefox's media pipeline and triggers NS_ERROR at
     * DASH segment boundaries. SyncSurface already has a drain loop and is the
     * correct place to wait for the decoded frame. */
    return VA_STATUS_SUCCESS;
}


/* ============================================================================================================
 * AV1 (av1.c re-serialises the headers; this is the submit policy)
 *
 * MPP needs refresh_frame_flags in the frame header; VA-API never carries it.  The next picture's ref_frame_map
 * reveals which slots the previous picture took, so a picture is held back until its successor arrives.  If the
 * client syncs the held picture first (FFmpeg does, every frame), it is decoded at once with refresh_frame_flags =
 * 0 — a correct picture that leaves MPP's references untouched — and decoded again with the real mask when the
 * successor shows it was a reference (measured bit-exact either way, tests/av1).  Hidden frames (show_frame = 0)
 * are followed by a synthesised show_existing_frame of a slot they refreshed, so MPP outputs the picture for the
 * client's surface; the client never sends show_existing to a VA driver.
 * ============================================================================================================ */

/* Deferred refresh (needs an MPP speaking protocol 2 of mpp_rkva.h, RK3588 only): the early decode of a picture the
   client synced before its successor arrived carries an 'H' padding OBU with a token, and MPP keeps the picture with the
   slot state it would have saved; when the successor reveals refresh_frame_flags, a 'C' padding OBU (mask, token) stores
   it into those slots.  Without it the picture is decoded a second time with the real mask (one extra av1d job per
   shown reference frame, the 4K60 bottleneck in Firefox).  The MPP entry points are weak references: against an MPP
   without them (any stock build) they are NULL and the driver keeps its re-decode.  MPP ignores the commands until
   mpp_av1d_rkva_enable() accepted them on that decoder (CreateContext).  RKVA_AV1_NO_DEFER=1 forces the re-decode. */
static int av1_defer_state = -1;
static pthread_once_t av1_defer_once = PTHREAD_ONCE_INIT;
static void av1_defer_probe(void) {
    int v = mpp_av1d_rkva_deferred_refresh ? (int)mpp_av1d_rkva_deferred_refresh() : -1;
    bool off = getenv("RKVA_AV1_NO_DEFER") != NULL;
    av1_defer_state = (v == RKVA_MPP_PROTOCOL && mpp_av1d_rkva_enable && !off) ? 1 : 0;
    if (v < 0)       LOG("av1: deferred refresh off (this MPP lacks it: early-synced pictures are decoded twice)");
    else if (v == 0) LOG("av1: deferred refresh off (MPP has it, not for this SoC)");
    else             LOG("av1: deferred refresh %s (MPP protocol %d, driver needs %d%s)", av1_defer_state ? "available" : "off",
                         v, RKVA_MPP_PROTOCOL, off ? ", RKVA_AV1_NO_DEFER set" : "");
}
static bool av1_defer_ok(void) { pthread_once(&av1_defer_once, av1_defer_probe); return av1_defer_state == 1; }
/* The private padding OBU MPP's patched av1d understands: payload "RKVA", command, then one or two argument bytes
   ('H' token, 'C' mask token). */
static size_t av1_write_rkva(uint8_t *dst, char cmd, uint8_t a, int nb, uint8_t b) {
    static const uint8_t magic[4] = { 'R', 'K', 'V', 'A' };
    size_t pl = 4 + 1 + 1 + (nb > 1 ? 1 : 0);
    dst[0] = (uint8_t)((15 << 3) | 0x02); dst[1] = (uint8_t)pl;   /* OBU_PADDING, has_size_field */
    memcpy(dst + 2, magic, 4); dst[6] = (uint8_t)cmd; dst[7] = a;
    if (nb > 1) dst[8] = b;
    return 2 + pl;
}

/* A synthesised show_existing_frame header, closed with a temporal delimiter.  MPP's av1d splitter (av1d_split_frame)
   ends a FRAME_HEADER OBU only when the next frame header or temporal delimiter arrives (tile groups may follow it), so a
   bare show_existing packet sat in its stream buffer until the client's NEXT picture was put, and then went out under
   that packet's PTS.  A client that syncs the hidden picture before submitting another one (Firefox, FFmpeg's download
   path) waited out the 3 s deadline: "Failed to sync surface" at the first 5-picture temporal unit of a 4K60 YouTube
   stream.  The trailing delimiter makes the splitter emit the header at once, with its own PTS; the delimiter itself
   is a no-op at the start of the next packet. */
static size_t av1_show_existing_closed(const GstAV1SequenceHeaderOBU *seq, int slot, uint8_t frame_type, uint8_t *dst, size_t cap) {
    size_t n = av1_write_show_existing(seq, slot, frame_type, dst, cap);
    if (!n || n + 2 > cap) return n;
    dst[n++] = (uint8_t)((2 << 3) | 0x02);   /* OBU_TEMPORAL_DELIMITER, has_size_field */
    dst[n++] = 0;
    return n;
}

static void av1_slots_store(RKContext *c, uint8_t mask, VASurfaceID sid, uint32_t gen) {
    for (int i = 0; i < 8; i++) if (mask & (1u << i)) { c->av1_mslot[i] = sid; c->av1_mslot_gen[i] = gen; }
}

static void av1_mark_decoded(RKDriver *d, VASurfaceID sid) {
    RKSurface *t = surface_by_id(d, sid);
    if (!t) return;
    pthread_mutex_lock(&t->lock); t->decoded = true; pthread_cond_broadcast(&t->cond); pthread_mutex_unlock(&t->lock);
}

/* Send one packet (pts = the surface its output belongs to); enqueue that surface in the FIFO when `enqueue`.
 *
 * The packet is queued for the context's submit thread, which feeds MPP and absorbs MPP_ERR_BUFFER_FULL.  MPP's AV1
 * input queue holds 3 packets; a temporal unit with several hidden pictures (each followed by a show_existing header)
 * used to block the client's EndPicture until the hardware had decoded earlier pictures -- 17 + 12 ms inside one
 * Firefox decode call on the YouTube BBB 4K60 stream, which Firefox counts as a late decode, and after 16 of those it
 * abandons hardware decoding.  MPP still sees its packets one at a time, in order, exactly as before. */
typedef struct RkPkt { struct RkPkt *next; size_t len; VASurfaceID sid; RK_S64 pts; uint8_t *data; uint8_t inl[]; } RkPkt;
/* data is inl[] (small packets copied in) or a heap buffer the queue owns (av1_put_take) */
static void rk_pkt_free(RkPkt *k) { if (k->data != k->inl) free(k->data); free(k); }
/* Bound on the queue: deep enough that a temporal unit of hidden pictures never waits, shallow enough that a client
   which never syncs (ffmpeg -hwaccel_output_format vaapi -f null) is held to the hardware's pace instead of queueing
   the whole stream in memory and having it dropped at DestroyContext. */
#define RK_SUB_MAX 16

static void *av1_submit_thread(void *arg) {
    RKContext *c = arg;
    RKDriver  *d = NULL;
    for (;;) {
        pthread_mutex_lock(&c->sub_lock);
        while (!c->sub_head && !c->sub_stop) pthread_cond_wait(&c->sub_cv, &c->sub_lock);
        RkPkt *k = c->sub_head;
        if (!k) { pthread_mutex_unlock(&c->sub_lock); break; }           /* stop requested and nothing left */
        if (c->sub_stop) {                                               /* teardown: drop what is left */
            while (k) { RkPkt *n = k->next; rk_pkt_free(k); k = n; }
            c->sub_head = c->sub_tail = NULL; c->sub_count = 0;
            pthread_cond_broadcast(&c->sub_room);
            pthread_mutex_unlock(&c->sub_lock);
            break;
        }
        pthread_mutex_unlock(&c->sub_lock);
        d = (RKDriver *)c->sub_drv;
        MppPacket pkt = NULL;
        mpp_packet_init(&pkt, k->data, k->len);
        mpp_packet_set_length(pkt, k->len);
        mpp_packet_set_pts(pkt, k->pts);
        MPP_RET ret = MPP_OK;
        long long t_put0 = rk_now_us(); int attempts = 0;
        for (int attempt = 0; attempt < 4000 && !__atomic_load_n(&c->sub_stop, __ATOMIC_ACQUIRE); attempt++) {
            attempts = attempt + 1;
            ret = c->mpi->decode_put_packet(c->mpp, pkt);
            if (ret == MPP_OK || ret != MPP_ERR_BUFFER_FULL) break;
            MppFrame df = NULL;
            pthread_mutex_lock(&c->out_lock);
            while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &df) == MPP_OK && df) { assign_mpp_frame(df, c, d); df = NULL; }
            pthread_mutex_unlock(&c->out_lock);
            usleep(500);
        }
        mpp_packet_deinit(&pkt);
        LOG("av1_put: surface=0x%x %zu bytes put in %lld us (%d attempts)", (unsigned)k->sid, k->len, rk_now_us() - t_put0, attempts);
        if (ret != MPP_OK && !__atomic_load_n(&c->sub_stop, __ATOMIC_ACQUIRE)) {
            /* nothing will come out for this packet: release a waiter, but only if the surface still holds the picture
               the packet was for (not after the client re-targeted it), and under out_lock like every routing */
            LOG("av1_put: decode_put_packet failed: %d", ret);
            pthread_mutex_lock(&c->out_lock);
            RKSurface *t = surface_by_id(d, k->sid);
            if (t && t->gen == (uint32_t)((uint64_t)k->pts >> 32)) av1_mark_decoded(d, k->sid);
            pthread_mutex_unlock(&c->out_lock);
        }
        pthread_mutex_lock(&c->sub_lock);
        c->sub_head = k->next; if (!c->sub_head) c->sub_tail = NULL;
        c->sub_count--;
        pthread_cond_signal(&c->sub_room);
        pthread_mutex_unlock(&c->sub_lock);
        rk_pkt_free(k);
    }
    return NULL;
}

/* Stop the submit thread (pending packets are dropped).  Caller must NOT hold out_lock: the thread takes it. */
static void av1_submit_stop(RKContext *c) {
    if (!c->sub_started) return;
    pthread_mutex_lock(&c->sub_lock); __atomic_store_n(&c->sub_stop, true, __ATOMIC_RELEASE); pthread_cond_signal(&c->sub_cv); pthread_mutex_unlock(&c->sub_lock);
    pthread_join(c->sub_thread, NULL);
    pthread_cond_destroy(&c->sub_room); pthread_cond_destroy(&c->sub_cv); pthread_mutex_destroy(&c->sub_lock);
    c->sub_started = false; c->sub_stop = false; c->sub_head = c->sub_tail = NULL; c->sub_count = 0;
}

static VAStatus av1_queue_pkt(RKContext *c, RKDriver *d, RkPkt *k, VASurfaceID sid, uint32_t gen, bool enqueue) {
    k->next = NULL; k->sid = sid; k->pts = (RK_S64)(((uint64_t)gen << 32) | (uint64_t)sid);
    if (!c->sub_started) {
        c->sub_drv = d;
        pthread_mutex_init(&c->sub_lock, NULL); pthread_cond_init(&c->sub_cv, NULL); pthread_cond_init(&c->sub_room, NULL);
        c->sub_stop = false; c->sub_head = c->sub_tail = NULL; c->sub_count = 0;
        if (pthread_create(&c->sub_thread, NULL, av1_submit_thread, c) != 0) {
            pthread_cond_destroy(&c->sub_room); pthread_cond_destroy(&c->sub_cv); pthread_mutex_destroy(&c->sub_lock); rk_pkt_free(k);
            LOG("av1_put: cannot start the submit thread");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        c->sub_started = true;
    }
    pthread_mutex_lock(&c->out_lock);
    { MppFrame f = NULL; while (!c->out_started && c->mpi->decode_get_frame(c->mpp, &f) == MPP_OK && f) { assign_mpp_frame(f, c, d); f = NULL; } }
    if (enqueue) { c->decode_queue[c->dq_tail] = sid; c->dq_tail = (c->dq_tail + 1) & 63; }
    pthread_mutex_unlock(&c->out_lock);
    pthread_mutex_lock(&c->sub_lock);
    while (c->sub_count >= RK_SUB_MAX && !c->sub_stop) pthread_cond_wait(&c->sub_room, &c->sub_lock);
    if (c->sub_tail) c->sub_tail->next = k; else c->sub_head = k;
    c->sub_tail = k; c->sub_count++;
    pthread_cond_signal(&c->sub_cv);
    pthread_mutex_unlock(&c->sub_lock);
    return VA_STATUS_SUCCESS;
}
/* Small packets (show_existing, commit): copied into the queue entry. */
static VAStatus av1_put(RKContext *c, RKDriver *d, uint8_t *data, size_t len, VASurfaceID sid, uint32_t gen, bool enqueue) {
    RkPkt *k = malloc(sizeof *k + len);
    if (!k) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    k->len = len; k->data = k->inl; memcpy(k->inl, data, len);
    return av1_queue_pkt(c, d, k, sid, gen, enqueue);
}
/* A heap buffer (a frame OBU, tens of KB at 4K): the queue takes ownership instead of copying it again; freed after
   the put, or here on failure. */
static VAStatus av1_put_take(RKContext *c, RKDriver *d, uint8_t *data, size_t len, VASurfaceID sid, uint32_t gen, bool enqueue) {
    RkPkt *k = malloc(sizeof *k);
    if (!k) { free(data); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    k->len = len; k->data = data;
    return av1_queue_pkt(c, d, k, sid, gen, enqueue);
}

/* Decode the held picture with refresh_frame_flags = mask.  forced_only: this is the early (mask 0) decode. */
static VAStatus av1_submit(RKContext *c, RKDriver *d, uint8_t mask, bool force_show) {
    typeof(c->av1_pending) *p = &c->av1_pending;
    const VADecPictureParameterBufferAV1 *pp = &p->pp;
    Av1RefInfo refs[8];
    for (int i = 0; i < 8; i++) {
        RKSurface *rs = (pp->ref_frame_map[i] != VA_INVALID_SURFACE) ? surface_by_id(d, pp->ref_frame_map[i]) : NULL;
        if (rs && rs->av1.valid) refs[i] = rs->av1; else { memset(&refs[i], 0, sizeof refs[i]); av1_gm_identity(refs[i].gm); }
    }
    bool hidden = !pp->pic_info_fields.bits.show_frame;
    size_t cap = p->tiles_len + 65536 + 4 * (size_t)p->ntiles + 64;
    uint8_t *buf = malloc(cap);
    if (!buf) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t n = 0;
    if (!c->av1_seq_sent || !av1_seq_matches(pp, &c->av1_seq)) {
        if (av1_seq_from_va(pp, &c->av1_seq) < 0) { LOG("av1: unsupported sequence (profile/depth/chroma)"); free(buf); return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }
        size_t sn = av1_write_seq_obu(&c->av1_seq, buf, cap);
        if (!sn) { free(buf); return VA_STATUS_ERROR_DECODING_ERROR; }
        n += sn; c->av1_seq_sent = true;
        LOG("av1: sequence header %zu bytes (%ux%u %d-bit)", sn, c->av1_seq.max_frame_width_minus_1 + 1, c->av1_seq.max_frame_height_minus_1 + 1, c->av1_seq.bit_depth);
    }
    GstAV1FrameHeaderOBU fh;
    /* an early-synced HIDDEN picture: the client wants its pixels now (it is showing it via show_existing), so decode it
       as a shown frame with refresh_frame_flags = 0 — same picture, nothing refreshed; the real (hidden) header follows
       later if the successor shows it was a reference */
    if (force_show) hidden = false;
    bool resub = p->forced;                       /* second submission: the slot refresh only, the picture was output already */
    if (av1_frame_from_va(pp, &c->av1_seq, refs, mask, &fh) < 0) { LOG("av1: frame uses an unsupported feature (superres/large-scale tile/intra-only refresh/switch frame)"); free(buf); return VA_STATUS_ERROR_DECODING_ERROR; }
    if (force_show && !fh.show_frame) {
        fh.show_frame = 1; fh.showable_frame = 0;
        /* a hidden KEY shown early would refresh every slot (shown KEY semantics): decode it as INTRA_ONLY instead —
           identical intra coding, refresh 0 */
        if (fh.frame_type == GST_AV1_KEY_FRAME) fh.frame_type = GST_AV1_INTRA_ONLY_FRAME;
    }
    /* MPP decodes the re-submission again; its output must never reach the client's surface (it can land after the
       picture the client already read, and the second decode of a frame is not always identical: Chromium 48delayed
       frames 302/366).  A hidden frame refreshes its slots and outputs nothing. */
    if (resub && fh.show_frame) { fh.show_frame = 0; fh.showable_frame = 1; hidden = true; }
    if (getenv("RKVA_AV1_NO_GRAIN")) fh.film_grain_params.apply_grain = 0;   /* experiment switch: send frames without film grain */
    /* early decode of a shown, non-key picture: ask MPP to hold it, so the slot refresh can be committed without a
       second decode (hidden pictures shown early and converted KEYs keep the re-decode: their headers differ) */
    bool hold = force_show && !resub && mask == 0 && pp->pic_info_fields.bits.show_frame && fh.frame_type != GST_AV1_KEY_FRAME && c->av1_defer;
    uint8_t token = (uint8_t)(c->rkva_token + 1);
    if (hold) n += av1_write_rkva(buf + n, 'H', token, 1, 0);
    size_t fn = av1_write_frame_obu(&fh, &c->av1_seq, p->tiles, p->ntiles, buf + n, cap - n);
    if (!fn) { LOG("av1: frame header write failed"); free(buf); return VA_STATUS_ERROR_DECODING_ERROR; }
    n += fn;
    bool will_output = !resub && (!hidden || mask != 0);   /* hidden + a slot = shown via show_existing below */
    LOG("av1: submit surface=0x%x type=%u show=%u mask=0x%02x tiles=%d bytes=%zu%s%s%s", (unsigned)p->surface, fh.frame_type, fh.show_frame, mask, p->ntiles, n, p->forced ? " (re-submit)" : "", force_show ? " (forced early)" : "", hold ? " (held)" : "");
    const char *dump = getenv("RKVA_DUMP_AV1");
    if (dump) { FILE *df = fopen(dump, "ab"); if (df) { fwrite(buf, 1, n, df); fclose(df); } }
    VAStatus st = av1_put_take(c, d, buf, n, p->out_surface, p->out_gen, !hidden);
    buf = NULL;                                   /* owned by the submit queue now */
    if (st == VA_STATUS_SUCCESS && !resub && hidden && mask && fh.frame_type == GST_AV1_KEY_FRAME) {
        /* A hidden KEY frame (libaom forward keyframes, Chromium 48delayed): show_existing of a KEY makes the decoder
           refresh every slot and reset — the stream only does that when it really shows it later.  For the client's
           copy decode the same tiles again as a shown INTRA_ONLY frame with refresh_frame_flags = 0: identical intra
           coding, no reference side effects. */
        GstAV1FrameHeaderOBU fi = fh; fi.frame_type = GST_AV1_INTRA_ONLY_FRAME; fi.show_frame = 1; fi.showable_frame = 0; fi.refresh_frame_flags = 0;
        size_t cap2 = p->tiles_len + 65536 + 4 * (size_t)p->ntiles + 64; uint8_t *b2 = malloc(cap2);
        size_t n2 = b2 ? av1_write_frame_obu(&fi, &c->av1_seq, p->tiles, p->ntiles, b2, cap2) : 0;
        if (!n2) { LOG("av1: hidden KEY: intra-only copy header write failed"); free(b2); free(buf); return VA_STATUS_ERROR_DECODING_ERROR; }
        LOG("av1: hidden KEY surface=0x%x: output copy as shown INTRA_ONLY (%zu bytes)", (unsigned)p->surface, n2);
        if (dump) { FILE *df = fopen(dump, "ab"); if (df) { fwrite(b2, 1, n2, df); fclose(df); } }
        st = av1_put_take(c, d, b2, n2, p->out_surface, p->out_gen, true);
    } else if (st == VA_STATUS_SUCCESS && !resub && hidden && mask) {
        uint8_t se[64]; size_t sn = av1_show_existing_closed(&c->av1_seq, __builtin_ctz(mask), fh.frame_type, se, sizeof se);
        if (sn) { if (dump) { FILE *df = fopen(dump, "ab"); if (df) { fwrite(se, 1, sn, df); fclose(df); } } st = av1_put(c, d, se, sn, p->out_surface, p->out_gen, true); }
    }
    free(buf);
    if (st != VA_STATUS_SUCCESS) return st;
    if (hold) { p->held = true; p->hold_token = token; c->rkva_token = token; }
    if (!will_output && !resub) av1_mark_decoded(d, p->out_surface);     /* nothing will ever come out for it */
    if (p->out_surface != p->surface) av1_mark_decoded(d, p->surface);   /* film grain: the reconstruct surface is never read */
    RKSurface *me = surface_by_id(d, p->ref_surface);
    if (me) { av1_ref_from_frame(pp, &fh, &me->av1); me->av1_mask = mask; }
    av1_slots_store(c, fh.refresh_frame_flags, p->ref_surface, p->ref_gen);   /* what MPP's slots now hold */
    return VA_STATUS_SUCCESS;
}

/* The successor's ref_frame_map tells which slots the held picture took. */
static uint8_t av1_mask_from_successor(const VADecPictureParameterBufferAV1 *next, VASurfaceID held) {
    uint8_t m = 0;
    for (int i = 0; i < 8; i++) if (next->ref_frame_map[i] == held) m |= (uint8_t)(1u << i);
    return m;
}

static VAStatus av1_resolve_pending(RKContext *c, RKDriver *d, const VADecPictureParameterBufferAV1 *next) {
    typeof(c->av1_pending) *p = &c->av1_pending;
    if (!p->valid) return VA_STATUS_SUCCESS;
    uint8_t mask = next ? av1_mask_from_successor(next, p->ref_surface) : 0;
    VAStatus st = VA_STATUS_SUCCESS;
    if (p->forced && p->held) {                                        /* MPP holds the early decode: commit its slots */
        uint8_t cb[16]; size_t cn = av1_write_rkva(cb, 'C', mask, 2, p->hold_token);
        LOG("av1: commit surface=0x%x mask=0x%02x (held, no re-decode)", (unsigned)p->surface, mask);
        st = av1_put(c, d, cb, cn, p->out_surface, p->out_gen, false);
        RKSurface *me = surface_by_id(d, p->ref_surface);
        if (st == VA_STATUS_SUCCESS && me) me->av1_mask = mask;
        if (st == VA_STATUS_SUCCESS) av1_slots_store(c, mask, p->ref_surface, p->ref_gen);
    } else if (!p->forced || mask) st = av1_submit(c, d, mask, false);  /* forced + mask 0: the early decode was final */
    if (st != VA_STATUS_SUCCESS) { av1_mark_decoded(d, p->surface); if (p->out_surface != p->surface) av1_mark_decoded(d, p->out_surface); }
    else { RKSurface *me = surface_by_id(d, p->ref_surface); if (me) me->av1_mask = 0; }
    p->valid = false; p->forced = false; p->held = false;
    return st;
}

/* vaSyncSurface on the held picture: decode it now with refresh_frame_flags = 0. */
static void av1_force_pending(RKContext *c, RKDriver *d, VASurfaceID sid) {
    typeof(c->av1_pending) *p = &c->av1_pending;
    if (!p->valid || (p->surface != sid && p->ref_surface != sid && p->out_surface != sid)) return;   /* cheap pre-check; re-checked under the lock */
    pthread_mutex_lock(&c->av1_lock);
    if (p->valid && !p->forced && (p->surface == sid || p->ref_surface == sid || p->out_surface == sid)) {
        LOG("av1: early sync of surface 0x%x — decoding with refresh_frame_flags=0 first", (unsigned)sid);
        long long t_f0 = rk_now_us();
        if (av1_submit(c, d, 0, true) == VA_STATUS_SUCCESS) p->forced = true;
        LOG("av1: early submit took %lld us", rk_now_us() - t_f0);
    }
    pthread_mutex_unlock(&c->av1_lock);
}

/* A refused picture: nothing will come out of MPP for it, so release whoever syncs its surfaces (BeginPicture armed
   the render target; film grain arms the display surface too) instead of making them wait out the 3 s deadline. */
static VAStatus av1_refuse(RKContext *c, RKDriver *d, const VADecPictureParameterBufferAV1 *pp, VAStatus st) {
    av1_mark_decoded(d, c->render_target);
    if (pp && pp->current_display_picture != VA_INVALID_SURFACE && pp->current_display_picture != c->render_target) av1_mark_decoded(d, pp->current_display_picture);
    return st;
}

static VAStatus do_av1_decode_locked(RKContext *c, RKDriver *d) {
    if (!c->have_pp_av1) { LOG("av1: EndPicture without picture parameters"); return av1_refuse(c, d, NULL, VA_STATUS_ERROR_INVALID_PARAMETER); }
    const VADecPictureParameterBufferAV1 *pp = &c->last_pp_av1;
    c->have_pp_av1 = false;
    /* RKVA_HIDE_10BIT: 8- and 10-bit AV1 share one profile, so a client that ignores the RT-format/P010 answers can
       still hand us a 10-bit stream with 8-bit surfaces (ffmpeg's CLI does).  Refuse before MPP sees it. */
    if (hide_10bit() && pp->bit_depth_idx) { LOG("av1: 10-bit stream refused (RKVA_HIDE_10BIT)"); return av1_refuse(c, d, pp, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT); }
    /* 1. the previous picture can now be decoded: this one's ref_frame_map shows where it went */
    VAStatus st = av1_resolve_pending(c, d, pp);
    if (st != VA_STATUS_SUCCESS) return av1_refuse(c, d, pp, st);
    /* a shown-existing KEY frame: the client refreshed every slot with the same key surface → MPP must do the same */
    {
        VASurfaceID k = pp->ref_frame_map[0]; bool all = (k != VA_INVALID_SURFACE);
        for (int i = 1; i < 8 && all; i++) all = (pp->ref_frame_map[i] == k);
        RKSurface *ks = all ? surface_by_id(d, k) : NULL;
        if (ks && ks->av1.valid && ks->av1.frame_type == 0 && ks->av1_mask != 0xff && c->av1_seq_sent) {
            int slot = -1;
            for (int i = 0; i < 8 && slot < 0; i++) if (c->av1_mslot[i] == k && c->av1_mslot_gen[i] == ks->gen) slot = i;
            if (slot < 0) {
                LOG("av1: shown-existing KEY surface 0x%x: no MPP slot holds it, show_existing not sent", (unsigned)k);
            } else {
                uint8_t se[64]; size_t sn = av1_show_existing_closed(&c->av1_seq, slot, 0, se, sizeof se);
                LOG("av1: shown-existing KEY surface 0x%x (slot %d) → full refresh", (unsigned)k, slot);
                if (sn && av1_put(c, d, se, sn, k, ks->gen, true) == VA_STATUS_SUCCESS) { ks->av1_mask = 0xff; av1_slots_store(c, 0xff, k, ks->gen); }
            }
        }
    }
    /* 2. collect this picture's tiles: slice parameter elements paired with the slice data buffer that follows them
          (FFmpeg/GStreamer: one pair per tile group; Chromium: one parameter buffer per tile, one data buffer) */
    typeof(c->av1_pending) *p = &c->av1_pending;
    p->valid = false; p->forced = false; p->held = false; p->ntiles = 0; p->tiles_len = 0;
    int data_pos[64], n_data = 0;
    for (int i = 0; i < c->n_pending; i++) { RKBuffer *b = buffer_by_id(d, c->pending[i]); if (b && b->type == VASliceDataBufferType) data_pos[n_data++] = i; }
    size_t need = 0;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (!b || b->type != VASliceParameterBufferType) continue;
        if (b->size < sizeof(VASliceParameterBufferAV1)) { LOG("av1: slice parameter buffer too small (%u bytes)", b->size); return av1_refuse(c, d, pp, VA_STATUS_ERROR_INVALID_BUFFER); }
        const VASliceParameterBufferAV1 *sp = (const VASliceParameterBufferAV1 *)b->data;
        for (unsigned e = 0; e < b->num_elements; e++) need += sp[e].slice_data_size;
    }
    if (need + 16 > p->tiles_cap) { uint8_t *nb = realloc(p->tiles_buf, need + 16); if (!nb) return VA_STATUS_ERROR_ALLOCATION_FAILED; p->tiles_buf = nb; p->tiles_cap = need + 16; }
    for (int i = 0; i < c->n_pending && p->ntiles < 512; i++) {
        RKBuffer *b = buffer_by_id(d, c->pending[i]);
        if (!b || b->type != VASliceParameterBufferType) continue;
        /* the data buffer that follows these parameters (FFmpeg, GStreamer); otherwise the last one before them
           (Chromium submits the tile data first, then one parameter buffer per tile) */
        int dp = -1; for (int k = 0; k < n_data; k++) if (data_pos[k] > i) { dp = data_pos[k]; break; }
        if (dp < 0) for (int k = n_data - 1; k >= 0; k--) if (data_pos[k] < i) { dp = data_pos[k]; break; }
        RKBuffer *db = dp >= 0 ? buffer_by_id(d, c->pending[dp]) : NULL;
        if (!db) {
            LOG("av1: slice parameters without slice data (%d buffers this picture):", c->n_pending);
            for (int k = 0; k < c->n_pending; k++) { RKBuffer *bk = buffer_by_id(d, c->pending[k]); LOG("av1:   buffer %d type %d size %u x%u", k, bk ? (int)bk->type : -1, bk ? bk->size : 0u, bk ? bk->num_elements : 0u); }
            return av1_refuse(c, d, pp, VA_STATUS_ERROR_INVALID_BUFFER);
        }
        size_t dlen = (size_t)db->size * db->num_elements;
        const VASliceParameterBufferAV1 *sp = (const VASliceParameterBufferAV1 *)b->data;
        for (unsigned e = 0; e < b->num_elements && p->ntiles < 512; e++) {
            if ((size_t)sp[e].slice_data_offset + sp[e].slice_data_size > dlen || sp[e].slice_data_size == 0) { LOG("av1: tile span out of range"); return av1_refuse(c, d, pp, VA_STATUS_ERROR_INVALID_BUFFER); }
            memcpy(p->tiles_buf + p->tiles_len, (const uint8_t *)db->data + sp[e].slice_data_offset, sp[e].slice_data_size);
            p->tiles[p->ntiles].data = NULL; p->tiles[p->ntiles].len = sp[e].slice_data_size;   /* pointer fixed below (realloc-safe) */
            p->tiles_len += sp[e].slice_data_size; p->ntiles++;
        }
    }
    { size_t off = 0; for (int t = 0; t < p->ntiles; t++) { p->tiles[t].data = p->tiles_buf + off; off += p->tiles[t].len; } }
    if (p->ntiles == 0) { LOG("av1: no tiles, marking surface 0x%x decoded", (unsigned)c->render_target); av1_mark_decoded(d, c->render_target); return VA_STATUS_SUCCESS; }
    if (p->ntiles != (int)pp->tile_cols * (int)pp->tile_rows) {   /* a short tile group would send MPP's parser past the packet */
        LOG("av1: %d tiles delivered, header says %u x %u — picture refused", p->ntiles, pp->tile_cols, pp->tile_rows);
        return av1_refuse(c, d, pp, VA_STATUS_ERROR_INVALID_BUFFER);
    }
    p->pp = *pp; p->surface = c->render_target; p->valid = true;
    /* Film grain: VA-API renders the (grained) output into the render target and keeps the un-grained picture in
       pp->current_frame, which is what ref_frame_map points at later.  Slot bookkeeping follows current_frame. */
    p->ref_surface = (pp->current_frame != VA_INVALID_SURFACE && surface_by_id(d, pp->current_frame)) ? pp->current_frame : c->render_target;
    if (p->ref_surface != p->surface) LOG("av1: NOTE render target 0x%x is not current_frame 0x%x", (unsigned)p->surface, (unsigned)p->ref_surface);
    /* Film grain: the client renders into current_frame (its reference copy, reused freely between pictures) and
       reads current_display_picture, where VA-API wants the grained picture.  MPP applies no grain on RK3588 and
       keeps its own references, so the decoded picture goes to the display surface (un-grained, see the docs) and
       the reconstruct surface is only marked decoded.  Arm the display surface so a sync on it waits. */
    p->out_surface = c->render_target;
    if (pp->current_display_picture != VA_INVALID_SURFACE && pp->current_display_picture != c->render_target) {
        RKSurface *ds = surface_by_id(d, pp->current_display_picture);
        if (ds) {
            p->out_surface = pp->current_display_picture;
            MppFrame za, zb;
            pthread_mutex_lock(&ds->lock); ds->decoded = false; ds->ctx_id = (VAContextID)(CONTEXT_ID_BASE + (unsigned)(c - d->contexts)); if (++ds->gen == 0) ds->gen = 1; surface_zc_take_locked(ds, &za, &zb); pthread_mutex_unlock(&ds->lock);
            zc_drop2(za, zb);
            LOG("av1: film grain: picture for 0x%x lands on display surface 0x%x", (unsigned)p->surface, (unsigned)p->out_surface);
        }
    }
    { RKSurface *os = surface_by_id(d, p->out_surface); p->out_gen = os ? os->gen : 0; }
    { RKSurface *rs = surface_by_id(d, p->ref_surface); p->ref_gen = rs ? rs->gen : 0; }
    /* 3. a shown KEY frame refreshes every slot by definition: no need to wait */
    if (pp->pic_info_fields.bits.frame_type == 0 && pp->pic_info_fields.bits.show_frame) {
        st = av1_submit(c, d, 0xff, false); p->valid = false; return st == VA_STATUS_SUCCESS ? st : av1_refuse(c, d, pp, st);
    }
    return VA_STATUS_SUCCESS;                                          /* held until the next picture (or a sync) */
}

static VAStatus do_av1_decode(RKContext *c, RKDriver *d) {
    pthread_mutex_lock(&c->av1_lock);
    VAStatus st = do_av1_decode_locked(c, d);
    pthread_mutex_unlock(&c->av1_lock);
    return st;
}

static VAStatus rk_EndPicture(VADriverContextP ctx, VAContextID ctx_id) {
    LOG("EndPicture: ctx=0x%x", ctx_id);
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_by_id(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    VAStatus st;
    long long t_ep0 = rk_now_us();
    if (c->coding == MPP_VIDEO_CodingAVC)
        st = do_h264_decode(c, d);
    else if (c->coding == MPP_VIDEO_CodingAV1)
        st = do_av1_decode(c, d);
    else
        st = do_generic_decode(c, d);
    LOG("EndPicture: done in %lld us", rk_now_us() - t_ep0);

    return st;
}

/* Wait until the surface's picture is decoded (vaSyncSurface without the zero-copy settle: ExportSurfaceHandle
   uses this, since its own export makes the copy unnecessary). */
static VAStatus rk_sync_wait(VADriverContextP ctx, VASurfaceID id) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_by_id(d, id);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

    pthread_mutex_lock(&s->lock);
    bool ready  = s->decoded || (s->ctx_id == 0);  /* not started = placeholder valid */
    VAContextID cid = s->ctx_id;
    pthread_mutex_unlock(&s->lock);

    if (ready) {
        LOG("SyncSurface: surface=0x%x prime_fd=%d ready", id, s->prime_fd);
        return VA_STATUS_SUCCESS;
    }

    /* EndPicture already polled 500ms; if the surface still isn't decoded
     * (B-frame pipeline priming, slow keyframe), actively drain MPP here
     * instead of sleeping on a cond that will never be signalled. */
    RKContext *c = context_by_id(d, cid);
    if (c && c->coding == MPP_VIDEO_CodingAV1)
        av1_force_pending(c, d, id);
    if (!c) {                                  /* destroyed before we got here: nothing will decode it */
        pthread_mutex_lock(&s->lock);
        bool ok = s->decoded;
        pthread_mutex_unlock(&s->lock);
        LOG("SyncSurface: surface=0x%x context 0x%x no longer exists", id, cid);
        return ok ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_DECODING_ERROR;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;

    LOG("SyncSurface: surface=0x%x draining MPP ctx=0x%x", id, cid);
    long long t_sync0 = rk_now_us(); int n_frames = 0, n_sleeps = 0, n_waits = 0;
    unsigned life = __atomic_load_n(&c->life, __ATOMIC_ACQUIRE);
    if (c->out_started) {
        /* the output thread routes frames and broadcasts s->cond; DestroyContext broadcasts too after clearing the
           slot (life changes).  The 50 ms slices are only a safety net. */
        bool gone = false;
        pthread_mutex_lock(&s->lock);
        while (!s->decoded) {
            if (life == 0 || __atomic_load_n(&c->life, __ATOMIC_ACQUIRE) != life) { gone = true; break; }
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
            struct timespec slice = now;
            slice.tv_nsec += 50 * 1000000L; if (slice.tv_nsec >= 1000000000L) { slice.tv_sec++; slice.tv_nsec -= 1000000000L; }
            if (slice.tv_sec > deadline.tv_sec || (slice.tv_sec == deadline.tv_sec && slice.tv_nsec > deadline.tv_nsec)) slice = deadline;
            pthread_cond_timedwait(&s->cond, &s->lock, &slice);
            n_waits++;
        }
        bool done = s->decoded;
        pthread_mutex_unlock(&s->lock);
        if (done) LOG("SyncSurface: surface=0x%x OK prime_fd=%d waited_us=%lld frames=%d sleeps=%d waits=%d", id, s->prime_fd, rk_now_us() - t_sync0, n_frames, n_sleeps, n_waits);
        else if (gone) LOG("SyncSurface: surface=0x%x context 0x%x destroyed while waiting", id, cid);
        else LOG("SyncSurface: TIMEOUT surface=0x%x prime_fd=%d", id, s->prime_fd);
    } else for (;;) {
        pthread_mutex_lock(&s->lock);
        bool done = s->decoded;
        pthread_mutex_unlock(&s->lock);
        if (done) { LOG("SyncSurface: surface=0x%x OK prime_fd=%d waited_us=%lld frames=%d sleeps=%d waits=%d", id, s->prime_fd, rk_now_us() - t_sync0, n_frames, n_sleeps, n_waits); break; }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            LOG("SyncSurface: TIMEOUT surface=0x%x prime_fd=%d", id, s->prime_fd);
            break;
        }

        if (c) {
            MppFrame frame = NULL;
            pthread_mutex_lock(&c->out_lock);
            if (!c->used || !c->mpi) {               /* destroyed meanwhile: nothing left to drain */
                pthread_mutex_unlock(&c->out_lock);
                LOG("SyncSurface: surface=0x%x context 0x%x destroyed while waiting", id, cid);
                break;
            }
            bool got = (c->mpi->decode_get_frame(c->mpp, &frame) == MPP_OK && frame);
            if (got) assign_mpp_frame(frame, c, d);
            pthread_mutex_unlock(&c->out_lock);
            if (got) { n_frames++; continue; } /* recheck immediately without sleeping */
        }
        n_sleeps++;
        usleep(1000);
    }
    pthread_mutex_lock(&s->lock);
    bool final_ok = s->decoded;
    pthread_mutex_unlock(&s->lock);
    return final_ok ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_DECODING_ERROR;
}

static VAStatus rk_SyncSurface(VADriverContextP ctx, VASurfaceID id) {
    VAStatus st = rk_sync_wait(ctx, id);
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_by_id(d, id);
    if (st == VA_STATUS_SUCCESS && s) {
        /* zero-copy: synced but not exported since this decode -- the client reads the surface through an older
           import of priv_buf (Chromium) or will map it: give priv_buf the picture */
        pthread_mutex_lock(&s->lock);
        bool need = s->zc_frame && !s->zc_exported;
        pthread_mutex_unlock(&s->lock);
        /* keep_exported: an ExportSurfaceHandle on another thread may hand MPP's buffer out between the check
           above and the copy; materialize re-checks under the lock and then keeps the frame */
        if (need) surface_zc_materialize(d, s, true);
    }
    return st;
}

static VAStatus rk_SyncSurface2(VADriverContextP ctx,
                                  VASurfaceID id, uint64_t timeout_ns) {
    (void)timeout_ns;
    return rk_SyncSurface(ctx, id);
}

static VAStatus rk_QuerySurfaceStatus(VADriverContextP ctx,
                                       VASurfaceID id,
                                       VASurfaceStatus *status) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_by_id(d, id);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    pthread_mutex_lock(&s->lock);
    *status = s->decoded ? VASurfaceReady : VASurfaceRendering;
    pthread_mutex_unlock(&s->lock);
    LOG("QuerySurfaceStatus: surface=0x%x status=%s", id,
        (*status == VASurfaceReady) ? "Ready" : "Rendering");
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_ExportSurfaceHandle(VADriverContextP ctx,
                                        VASurfaceID id,
                                        uint32_t mem_type,
                                        uint32_t flags,
                                        void *descriptor) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_by_id(d, id);
    LOG("ExportSurfaceHandle: surface=0x%x mem_type=0x%x flags=0x%x",
        id, mem_type, flags);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        LOG("ExportSurfaceHandle: unsupported mem_type 0x%x", mem_type);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    /* Zero-copy eligibility (RKContext.zc_ok): this surface exported again after a new decode into it. */
    {
        VAContextID ecid = 0; bool reexport = false;
        pthread_mutex_lock(&s->lock);
        if (s->ctx_id != 0 && s->gen != 0) {
            if (s->exported_gen != 0 && s->exported_gen != s->gen) reexport = true;
            s->exported_gen = s->gen;
            ecid = s->ctx_id;
        }
        pthread_mutex_unlock(&s->lock);
        RKContext *ec = reexport ? context_by_id(d, ecid) : NULL;
        if (ec && !__atomic_load_n(&ec->zc_ok, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&ec->zc_ok, true, __ATOMIC_RELEASE);
            LOG("zero-copy: context 0x%x re-exports surfaces after decoding: zero-copy on", ecid);
        }
    }

    /* If decode is in progress, sync now so the exported DMA-BUF contains the
     * correct frame. Firefox calls ExportSurfaceHandle before SyncSurface when
     * EndPicture is async; without this the EGLImage gets stale data. */
    pthread_mutex_lock(&s->lock);
    bool needs_sync = !s->decoded && (s->ctx_id != 0);
    pthread_mutex_unlock(&s->lock);
    if (needs_sync)
        rk_sync_wait(ctx, id);
    /* A writable export never gets MPP's buffer: that frame may be one of MPP's reference pictures.  Copy it into the
       surface's own buffer and export that (a read-only export handed out earlier keeps the frame alive). */
    if (flags & VA_EXPORT_SURFACE_WRITE_ONLY)
        surface_zc_materialize(d, s, true);

    pthread_mutex_lock(&s->lock);
    bool zc      = (s->zc_frame != NULL) && !(flags & VA_EXPORT_SURFACE_WRITE_ONLY);
    int fd       = zc ? s->zc_fd : s->prime_fd;
    int hs       = zc ? s->zc_hs : (s->hstride ? s->hstride : s->width);
    int vs       = zc ? s->zc_vs : (s->vstride ? s->vstride : s->height);
    size_t zsize = s->zc_size;
    bool decoded = s->decoded;
    bool is_placeholder = !zc && (s->priv_buf != NULL);
    bool is_10bit = surf_is_10bit(s);
    int export_fd = fd >= 0 ? dup(fd) : -1;   /* under the lock: the held frame cannot be released meanwhile */
    if (zc && export_fd >= 0) s->zc_exported = true;
    pthread_mutex_unlock(&s->lock);

    if (fd < 0) {
        LOG("ExportSurfaceHandle: prime_fd not ready (fd<0 decoded=%d), ERROR_INVALID_SURFACE", decoded);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (export_fd < 0) {
        LOG("ExportSurfaceHandle: dup(%d) failed errno=%d, ERROR_ALLOCATION_FAILED", fd, errno);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    LOG("ExportSurfaceHandle: surface=0x%x %dx%d stride=%dx%d export_fd=%d decoded=%d placeholder=%d 10bit=%d rt10=%d%s",
        id, s->width, s->height, hs, vs, export_fd, decoded, is_placeholder, is_10bit, s->rt_10bit, zc ? " zero-copy" : "");

    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    memset(desc, 0, sizeof(*desc));
    desc->width       = (uint32_t)s->width;
    desc->height      = (uint32_t)s->height;
    desc->num_objects = 1;
    desc->objects[0].fd                  = export_fd;
    desc->objects[0].drm_format_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */

    bool composed = (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;

    /* COMPOSED_LAYERS: single NV12/P010 layer with 2 planes (mpv, GStreamer).
     * SEPARATE_LAYERS (default): R8/GR88 split planes (Firefox DMABufSurfaceYUV). */
    if (!is_10bit && composed) {
        desc->fourcc     = VA_FOURCC_NV12;
        desc->num_layers = 1;
        desc->objects[0].size           = zc ? (uint32_t)zsize : (uint32_t)(hs * vs * 3 / 2);
        desc->layers[0].drm_format      = RK_DRM_FORMAT_NV12;
        desc->layers[0].num_planes      = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0]       = 0;
        desc->layers[0].pitch[0]        = (uint32_t)hs;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1]       = (uint32_t)(hs * vs);
        desc->layers[0].pitch[1]        = (uint32_t)hs;
        return VA_STATUS_SUCCESS;
    }
    if (is_10bit && composed) {
        desc->fourcc     = VA_FOURCC_P010;
        desc->num_layers = 1;
        desc->objects[0].size           = (uint32_t)(hs * vs * 3);
        desc->layers[0].drm_format      = RK_DRM_FORMAT_P010;
        desc->layers[0].num_planes      = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0]       = 0;
        desc->layers[0].pitch[0]        = (uint32_t)(hs * 2);
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1]       = (uint32_t)(hs * vs * 2);
        desc->layers[0].pitch[1]        = (uint32_t)(hs * 2);
        return VA_STATUS_SUCCESS;
    }

    desc->num_layers = 2;

    if (is_10bit) {
        /*
         * P010: 10-bit NV12 semi-planar.  Each luma/chroma sample is uint16.
         * Two-layer layout matching Firefox DMABufSurfaceYUV P010 import:
         *   layer 0 → DRM_FORMAT_R16    (Y,  16-bit luma, 10 significant bits)
         *   layer 1 → DRM_FORMAT_GR1616 (UV, 2×16-bit interleaved chroma)
         * Both layers share pitch = hs*2 (bytes per luma row).
         * UV offset = hs*vs*2 (Y plane size in bytes).
         */
        desc->fourcc                         = VA_FOURCC_P010;
        desc->objects[0].size                = (uint32_t)(hs * vs * 3);
        /* Y plane */
        desc->layers[0].drm_format           = RK_DRM_FORMAT_R16;
        desc->layers[0].num_planes           = 1;
        desc->layers[0].object_index[0]      = 0;
        desc->layers[0].offset[0]            = 0;
        desc->layers[0].pitch[0]             = (uint32_t)(hs * 2);
        /* UV plane */
        desc->layers[1].drm_format           = RK_DRM_FORMAT_GR1616; /* was 0x36315247 ("GR16", not a fourcc) until v2.1.3 — the whole of KI-3 */
        desc->layers[1].num_planes           = 1;
        desc->layers[1].object_index[0]      = 0;
        desc->layers[1].offset[0]            = (uint32_t)(hs * vs * 2);
        desc->layers[1].pitch[0]             = (uint32_t)(hs * 2);
    } else {
        /*
         * NV12: 8-bit YUV 4:2:0 semi-planar.
         * Firefox (DMABufSurfaceYUV) expects num_layers == plane count, each
         * layer being a single-plane view of the buffer:
         *   layer 0 → DRM_FORMAT_R8   (Y,  8-bit luma)
         *   layer 1 → DRM_FORMAT_GR88 (UV, interleaved chroma, 2 bytes/px)
         */
        desc->fourcc                         = VA_FOURCC_NV12;
        desc->objects[0].size                = zc ? (uint32_t)zsize : (uint32_t)(hs * vs * 3 / 2);
        /* Y plane */
        desc->layers[0].drm_format           = RK_DRM_FORMAT_R8;
        desc->layers[0].num_planes           = 1;
        desc->layers[0].object_index[0]      = 0;
        desc->layers[0].offset[0]            = 0;
        desc->layers[0].pitch[0]             = (uint32_t)hs;
        /* UV plane */
        desc->layers[1].drm_format           = RK_DRM_FORMAT_GR88;
        desc->layers[1].num_planes           = 1;
        desc->layers[1].object_index[0]      = 0;
        desc->layers[1].offset[0]            = (uint32_t)(hs * vs);
        desc->layers[1].pitch[0]             = (uint32_t)hs;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryImageFormats(VADriverContextP ctx,
                                      VAImageFormat *list, int *n) {
    (void)ctx;
    list[0].fourcc         = VA_FOURCC_NV12;
    list[0].byte_order     = VA_LSB_FIRST;
    list[0].bits_per_pixel = 12;
    list[1].fourcc         = VA_FOURCC_P010;
    list[1].byte_order     = VA_LSB_FIRST;
    list[1].bits_per_pixel = 24;
    *n = 2;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateImage(VADriverContextP ctx,
                                VAImageFormat *format,
                                int width, int height,
                                VAImage *image) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)format;

    VABufferID buf_id;
    /* P010 images carry uint16 samples; size and pitches are in BYTES. */
    unsigned int bpp    = (format && format->fourcc == VA_FOURCC_P010) ? 2u : 1u;
    unsigned int stride = (unsigned int)((width + 15) & ~15) * bpp;
    unsigned int size   = stride * (unsigned int)height * 3 / 2;
    VAStatus st = rk_CreateBuffer(ctx, 0, VAImageBufferType, size, 1,
                                  NULL, &buf_id);
    if (st != VA_STATUS_SUCCESS) return st;
    { RKBuffer *ib = buffer_by_id(drv_from_ctx(ctx), buf_id); if (ib) { ib->img_w = (unsigned short)width; ib->img_h = (unsigned short)height; } }

    memset(image, 0, sizeof(*image));
    image->image_id    = buf_id; /* reuse buf_id as image_id for simplicity */
    image->buf         = buf_id;
    image->format      = *format;
    image->width       = (unsigned short)width;
    image->height      = (unsigned short)height;
    image->num_planes  = 2;
    image->pitches[0]  = stride;
    image->pitches[1]  = stride;
    image->offsets[0]  = 0;
    image->offsets[1]  = stride * (unsigned int)height;
    image->data_size   = size;
    (void)d;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DeriveImage(VADriverContextP ctx,
                                VASurfaceID sid, VAImage *image) {
    /* Map the surface's own memory as a VAImage.  This is what VLC's GL
     * interop and ffmpeg's hwframe transfer call; both fail without it.
     * The decode path has already written this buffer in exactly the layout
     * reported here: NV12 for 8-bit, true P010 for 10-bit (the NV15 repack
     * runs before the frame is published), both at the surface's own stride.
     * The image buffer BORROWS the MPP mapping - it must not be freed. */
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_by_id(d, sid);
    if (!s || !s->priv_buf) return VA_STATUS_ERROR_INVALID_SURFACE;
    surface_zc_materialize(d, s, true);      /* the image aliases priv_buf */

    void *ptr = mpp_buffer_get_ptr(s->priv_buf);
    if (!ptr) return VA_STATUS_ERROR_INVALID_SURFACE;

    bool i10 = surf_is_10bit(s);
    int  bpp = i10 ? 2 : 1;
    int  hs  = (s->hstride ? s->hstride : s->width) * bpp;   /* bytes per row */
    int  vs  = s->vstride ? s->vstride : s->height;

    VABufferID buf_id;
    VAStatus st = rk_CreateBuffer(ctx, 0, VAImageBufferType, 1, 1, NULL, &buf_id);
    if (st != VA_STATUS_SUCCESS) return st;
    RKBuffer *b = buffer_by_id(d, buf_id);
    if (!b) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    free(b->data);
    b->data     = ptr;          /* alias the decoded surface */
    b->borrowed = true;
    b->size     = (unsigned int)(hs * vs * 3 / 2);

    memset(image, 0, sizeof(*image));
    image->image_id           = buf_id;
    image->buf                = buf_id;
    image->format.fourcc      = i10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    image->format.byte_order  = VA_LSB_FIRST;
    image->format.bits_per_pixel = i10 ? 24 : 12;
    image->width              = (unsigned short)s->width;
    image->height             = (unsigned short)s->height;
    image->num_planes         = 2;
    image->pitches[0]         = (unsigned int)hs;
    image->pitches[1]         = (unsigned int)hs;
    image->offsets[0]         = 0;
    image->offsets[1]         = (unsigned int)(hs * vs);
    image->data_size          = (unsigned int)(hs * vs * 3 / 2);
    LOG("DeriveImage: surface=0x%x %dx%d %s pitch=%d -> image=0x%x",
        sid, s->width, s->height, i10 ? "P010" : "NV12", hs, (unsigned)buf_id);
    return VA_STATUS_SUCCESS;
}


static VAStatus rk_DestroyImage(VADriverContextP ctx, VAImageID id) {
    return rk_DestroyBuffer(ctx, (VABufferID)id);
}

/* ── stub implementations ────────────────────────────────────── */

/* Suppress -Wunused-parameter for pure stub functions */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static VAStatus rk_SetImagePalette(VADriverContextP ctx, VAImageID image,
                                    unsigned char *palette)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_GetImage(VADriverContextP ctx, VASurfaceID surface_id,
                             int x, int y, unsigned int w,
                             unsigned int h, VAImageID image_id)
{
    (void)x; (void)y; (void)w; (void)h;
    RKDriver  *d  = drv_from_ctx(ctx);
    RKSurface *s  = surface_by_id(d, surface_id);
    RKBuffer  *ib = buffer_by_id(d, (VABufferID)image_id);
    if (!s || !ib || !s->priv_buf || !ib->data) return VA_STATUS_ERROR_INVALID_SURFACE;
    surface_zc_materialize(d, s, true);      /* read from priv_buf below */

    int hs  = s->hstride ? s->hstride : s->width;
    int vs  = s->vstride ? s->vstride : s->height;
    bool i10 = surf_is_10bit(s);
    int bpp  = i10 ? 2 : 1;
    /* The image has its own size (what the client asked vaCreateImage for) and stride ((width+15)&~15, as in
       rk_CreateImage); the surface may hold a picture of another size after an AV1 sequence change.  Copy the
       overlap only — never past either buffer (libaom av1-1-b8-03-sizeup corrupted the heap here). */
    int img_w  = ib->img_w ? ib->img_w : s->width, img_h = ib->img_h ? ib->img_h : s->height;
    int img_hs = (int)(((unsigned int)img_w + 15u) & ~15u) * bpp;
    int rows   = img_h < s->height ? img_h : s->height; if (rows > vs) rows = vs;
    size_t row = (size_t)(img_hs < hs * bpp ? img_hs : hs * bpp);
    if ((size_t)img_hs * (size_t)img_h * 3 / 2 > ib->size) return VA_STATUS_ERROR_INVALID_IMAGE;

    const uint8_t *sp = (const uint8_t *)mpp_buffer_get_ptr(s->priv_buf);
    uint8_t       *dp = (uint8_t *)ib->data;
    /* Y plane */
    for (int r = 0; r < rows; r++)
        memcpy(dp + (size_t)r * img_hs, sp + (size_t)r * hs * bpp, row);
    /* UV plane: src at hs*vs, dst at img_hs*img_h */
    const uint8_t *su = sp + (size_t)hs * vs * bpp;
    uint8_t       *du = dp + (size_t)img_hs * img_h;
    for (int r = 0; r < rows / 2; r++)
        memcpy(du + (size_t)r * img_hs, su + (size_t)r * hs * bpp, row);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_PutImage(VADriverContextP ctx, VASurfaceID surface,
                             VAImageID image, int src_x, int src_y,
                             unsigned int src_width, unsigned int src_height,
                             int dest_x, int dest_y,
                             unsigned int dest_width, unsigned int dest_height)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QuerySubpicFmts(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats)
{ *num_formats = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateSubpicture(VADriverContextP ctx,
                                     VAImageID image,
                                     VASubpictureID *subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DestroySubpicture(VADriverContextP ctx,
                                      VASubpictureID subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicImage(VADriverContextP ctx,
                                   VASubpictureID subpicture, VAImageID image)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicChromakey(VADriverContextP ctx,
                                       VASubpictureID subpicture,
                                       unsigned int chromakey_min,
                                       unsigned int chromakey_max,
                                       unsigned int chromakey_mask)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicAlpha(VADriverContextP ctx,
                                   VASubpictureID subpicture, float global_alpha)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_AssociateSubpic(VADriverContextP ctx,
                                    VASubpictureID subpicture,
                                    VASurfaceID *target_surfaces, int num_surfaces,
                                    short src_x, short src_y,
                                    unsigned short src_width, unsigned short src_height,
                                    short dest_x, short dest_y,
                                    unsigned short dest_width, unsigned short dest_height,
                                    unsigned int flags)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DeassociateSubpic(VADriverContextP ctx,
                                      VASubpictureID subpicture,
                                      VASurfaceID *target_surfaces, int num_surfaces)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QueryDisplayAttrs(VADriverContextP ctx,
                                      VADisplayAttribute *attr_list,
                                      int *num_attributes)
{ *num_attributes = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_GetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                               VABufferType *type, unsigned int *size,
                               unsigned int *num_elements)
{
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_by_id(d, buf_id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (type)         *type         = b->type;
    if (size)         *size         = b->size;
    if (num_elements) *num_elements = b->num_elements;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_LockSurface(VADriverContextP ctx, VASurfaceID surface,
                                unsigned int *fourcc,
                                unsigned int *luma_stride,
                                unsigned int *chroma_u_stride,
                                unsigned int *chroma_v_stride,
                                unsigned int *luma_offset,
                                unsigned int *chroma_u_offset,
                                unsigned int *chroma_v_offset,
                                unsigned int *buffer_name, void **buffer)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QuerySurfaceAttrs(VADriverContextP ctx, VAConfigID config,
                                      VASurfaceAttrib *attrib_list,
                                      unsigned int *num_attribs)
{
    LOG("QuerySurfaceAttributes: config=0x%x list=%s",
        config, attrib_list ? "provided" : "NULL (query count)");

    /* Firefox calls this twice: first with NULL to get count, then with buffer */
    const unsigned int n = hide_10bit() ? 6 : 7;     /* RKVA_HIDE_10BIT: no P010 entry */
    if (!attrib_list) {
        *num_attribs = n;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < n) {
        *num_attribs = n;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    unsigned k = 0;
    /* Pixel format: NV12 (8-bit) */
    attrib_list[k].type              = VASurfaceAttribPixelFormat;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = VA_FOURCC_NV12;
    k++;

    /* Pixel format: P010 (10-bit) — not under RKVA_HIDE_10BIT */
    if (!hide_10bit()) {
        attrib_list[k].type              = VASurfaceAttribPixelFormat;
        attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
        attrib_list[k].value.type        = VAGenericValueTypeInteger;
        attrib_list[k].value.value.i     = VA_FOURCC_P010;
        k++;
    }

    /* Memory type: VA-managed + DRM PRIME 2 */
    attrib_list[k].type              = VASurfaceAttribMemoryType;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE |
                                       VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = (int)(VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
    k++;

    /* Max resolution */
    attrib_list[k].type              = VASurfaceAttribMaxWidth;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = 7680;
    k++;

    attrib_list[k].type              = VASurfaceAttribMaxHeight;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = 4320;
    k++;

    attrib_list[k].type              = VASurfaceAttribMinWidth;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = 16;
    k++;

    attrib_list[k].type              = VASurfaceAttribMinHeight;
    attrib_list[k].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[k].value.type        = VAGenericValueTypeInteger;
    attrib_list[k].value.value.i     = 16;
    k++;

    *num_attribs = k;
    LOG("QuerySurfaceAttributes: returned %u attribs (NV12%s, DRM_PRIME_2)", n, hide_10bit() ? "" : ", P010");
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_AcquireBufferHandle(VADriverContextP ctx,
                                        VABufferID buf_id, VABufferInfo *buf_info)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateMFContext(VADriverContextP ctx,
                                    VAMFContextID *mfe_context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFAddContext(VADriverContextP ctx,
                                 VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFReleaseContext(VADriverContextP ctx,
                                     VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context,
                             VAContextID *contexts, int num_contexts)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_QueryProcessingRate(VADriverContextP ctx,
                                        VAConfigID config_id,
                                        VAProcessingRateParameter *proc_buf,
                                        unsigned int *processing_rate)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_SyncBuffer(VADriverContextP ctx, VABufferID buf_id,
                               uint64_t timeout_ns)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_Copy(VADriverContextP ctx, VACopyObject *dst,
                         VACopyObject *src, VACopyOption option)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

#pragma GCC diagnostic pop

static VAStatus rk_PutSurface(VADriverContextP ctx, VASurfaceID s,
                               void *draw, short sx, short sy,
                               unsigned short sw, unsigned short sh,
                               short dx, short dy,
                               unsigned short dw, unsigned short dh,
                               VARectangle *clips, unsigned int nc,
                               unsigned int flags) {
    (void)ctx;(void)s;(void)draw;(void)sx;(void)sy;(void)sw;(void)sh;
    (void)dx;(void)dy;(void)dw;(void)dh;(void)clips;(void)nc;(void)flags;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QuerySurfaceError(VADriverContextP ctx, VASurfaceID s,
                                      VAStatus err, void **info) {
    (void)ctx;(void)s;(void)err; *info = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateBuffer2(VADriverContextP ctx, VAContextID context,
                                  VABufferType type,
                                  unsigned int w, unsigned int h,
                                  unsigned int *unit_size,
                                  unsigned int *pitch,
                                  VABufferID *id) {
    unsigned int stride = (w + 15) & ~15;
    unsigned int size   = stride * h;
    if (unit_size) *unit_size = size;
    if (pitch)     *pitch     = stride;
    return rk_CreateBuffer(ctx, context, type, size, 1, NULL, id);
}

static VAStatus rk_GetSurfaceAttributes(VADriverContextP ctx,
                                         VAConfigID config,
                                         VASurfaceAttrib *list,
                                         unsigned int n) {
    (void)ctx;(void)config;(void)list;(void)n;
    return VA_STATUS_SUCCESS;
}

/* ── driver init ─────────────────────────────────────────────── */

VAStatus __vaDriverInit_1_20(VADriverContextP ctx)  /* NOLINT */
{
    log_init();
    LOG("__vaDriverInit_1_20: entry");
    RKDriver *d = calloc(1, sizeof(*d));
    if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ctx->pDriverData = d;
    pthread_mutex_init(&d->buf_lock, NULL);
    pthread_mutex_init(&d->obj_lock, NULL);
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        pthread_mutex_init(&d->contexts[i].out_lock, NULL);   /* live as long as the driver (see RKContext) */
        pthread_mutex_init(&d->contexts[i].av1_lock, NULL);
    }

    ctx->version_major        = VA_MAJOR_VERSION;
    ctx->version_minor        = VA_MINOR_VERSION;
    ctx->max_profiles         = 16;
    ctx->max_entrypoints      = 4;
    ctx->max_attributes       = 8;
    ctx->max_image_formats    = 4;
    ctx->max_subpic_formats   = 4;
    ctx->max_display_attributes = 4;
    ctx->str_vendor           = "Rockchip MPP VA-API Driver 3.5.0~rc1 (defcom5)";

    struct VADriverVTable *v = ctx->vtable;
    v->vaTerminate                = rk_Terminate;
    v->vaQueryConfigProfiles      = rk_QueryConfigProfiles;
    v->vaQueryConfigEntrypoints   = rk_QueryConfigEntrypoints;
    v->vaGetConfigAttributes      = rk_GetConfigAttributes;
    v->vaCreateConfig             = rk_CreateConfig;
    v->vaDestroyConfig            = rk_DestroyConfig;
    v->vaQueryConfigAttributes    = rk_QueryConfigAttributes;
    v->vaCreateSurfaces           = rk_CreateSurfaces;
    v->vaDestroySurfaces          = rk_DestroySurfaces;
    v->vaCreateContext            = rk_CreateContext;
    v->vaDestroyContext           = rk_DestroyContext;
    v->vaCreateBuffer             = rk_CreateBuffer;
    v->vaBufferSetNumElements     = rk_BufferSetNumElements;
    v->vaMapBuffer                = rk_MapBuffer;
    v->vaUnmapBuffer              = rk_UnmapBuffer;
    v->vaDestroyBuffer            = rk_DestroyBuffer;
    v->vaBeginPicture             = rk_BeginPicture;
    v->vaRenderPicture            = rk_RenderPicture;
    v->vaEndPicture               = rk_EndPicture;
    v->vaSyncSurface              = rk_SyncSurface;
    v->vaQuerySurfaceStatus       = rk_QuerySurfaceStatus;
    v->vaQuerySurfaceError        = rk_QuerySurfaceError;
    v->vaPutSurface               = rk_PutSurface;
    v->vaQueryImageFormats        = rk_QueryImageFormats;
    v->vaCreateImage              = rk_CreateImage;
    v->vaDeriveImage              = rk_DeriveImage;
    v->vaDestroyImage             = rk_DestroyImage;
    v->vaSetImagePalette          = rk_SetImagePalette;
    v->vaGetImage                 = rk_GetImage;
    v->vaPutImage                 = rk_PutImage;
    v->vaQuerySubpictureFormats   = rk_QuerySubpicFmts;
    v->vaCreateSubpicture         = rk_CreateSubpicture;
    v->vaDestroySubpicture        = rk_DestroySubpicture;
    v->vaSetSubpictureImage       = rk_SetSubpicImage;
    v->vaSetSubpictureChromakey   = rk_SetSubpicChromakey;
    v->vaSetSubpictureGlobalAlpha = rk_SetSubpicAlpha;
    v->vaAssociateSubpicture      = rk_AssociateSubpic;
    v->vaDeassociateSubpicture    = rk_DeassociateSubpic;
    v->vaQueryDisplayAttributes   = rk_QueryDisplayAttrs;
    v->vaGetDisplayAttributes     = rk_GetDisplayAttrs;
    v->vaSetDisplayAttributes     = rk_SetDisplayAttrs;
    v->vaBufferInfo               = rk_BufferInfo;
    v->vaLockSurface              = rk_LockSurface;
    v->vaUnlockSurface            = rk_UnlockSurface;
    v->vaGetSurfaceAttributes     = rk_GetSurfaceAttributes;
    v->vaCreateSurfaces2          = rk_CreateSurfaces2;
    v->vaQuerySurfaceAttributes   = rk_QuerySurfaceAttrs;
    v->vaAcquireBufferHandle      = rk_AcquireBufferHandle;
    v->vaReleaseBufferHandle      = rk_ReleaseBufferHandle;
    v->vaCreateMFContext          = rk_CreateMFContext;
    v->vaMFAddContext             = rk_MFAddContext;
    v->vaMFReleaseContext         = rk_MFReleaseContext;
    v->vaMFSubmit                 = rk_MFSubmit;
    v->vaCreateBuffer2            = rk_CreateBuffer2;
    v->vaQueryProcessingRate      = rk_QueryProcessingRate;
    v->vaExportSurfaceHandle      = rk_ExportSurfaceHandle;
    v->vaSyncSurface2             = rk_SyncSurface2;
    v->vaSyncBuffer               = rk_SyncBuffer;
    v->vaCopy                     = rk_Copy;

    LOG("driver init OK — Rockchip RK3588 MPP");
    return VA_STATUS_SUCCESS;
}
