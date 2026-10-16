# Developer documentation — rockchip-vaapi

> **defcom5 fork status (2026-10-11, v3.0.0).** This file is upstream's design document,
> kept verbatim below because the architecture it describes still holds. The fork has
> moved on in these places — read [KNOWN-ISSUES.md](../KNOWN-ISSUES.md) and
> [AGENTS.md](../AGENTS.md) for the current, hardware-verified state:
>
> | upstream text below says | the fork today |
> |---|---|
> | `num_ref_idx_l0/l1_default` hardcoded to 0 ("may require this to match the stream") | **Was KI-1.** Learned from override-free slices and re-emitted; fixed in v2.1.2 (`h264_peek_ref_override`). |
> | HEVC/VP9/AV1 paths are stubs | HEVC has a real VPS/SPS/PPS assembler (`src/hevc.c`), Main and Main10 advertised, bit-exact; VP9 P0/P2 decode; AV1 since v3.0.0 through an OBU re-serialiser (`src/av1.c` + `src/av1/`), see "AV1 decode pipeline". |
> | export = NV12 as R8 + GR88 | 8-bit unchanged; 10-bit exports true P010 (NV15 repacked) as R16 + GR1616 or one composed P010 layer. Fourccs are built with `RK_FOURCC()` after the "GR16" typo that was KI-3. |
> | 64 surfaces / 8 contexts | 128 surfaces (Chrome's per-frame pool at 4K); the DRM IOMMU's 4 GB I/O space bounds it (KI-7). |
> | `vaDeriveImage` not mentioned | Implemented (aliases the surface buffer, `borrowed` flag); copy-back clients work. |
> | `priv_buf` in "system dma-heap" | Allocated with `MPP_BUFFER_TYPE_DRM` (Rockchip GEM through the DRM IOMMU); still not CMA. |
> | Firefox-only story | Firefox, mpv, GStreamer and VA-API Chromium builds. The `+rkmpp` Chromium builds do not use this driver at all (KI-6). |
>
> Lesson recorded in AGENTS.md: upstream's *Known limitations* list below named the
> B-frame reference-count bug in April 2026; it cost this fork days to rediscover in
> September. Read the limitations section of an inherited driver first.


**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com

---

## Project overview

`rockchip-vaapi` is a VA-API 1.20 driver (`rockchip_drv_video.so`) that
bridges the `libva` API to the Rockchip MPP (Media Process Platform) library.
It was written from scratch because no open VA-API driver exists for the
Rockchip RK3588 SoC.

### Why a VA-API driver and not GStreamer or V4L2?

- **GStreamer**: Firefox 128+ has no GStreamer media backend. The code path
  (`dom/media/platforms/gstreamer/`) was removed; only the VA-API PDM remains.
- **V4L2**: Rockchip exposes a V4L2 interface, but it uses virtual M2M devices
  that are incompatible with Firefox's V4L2 backend (which expects real
  `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` M2M).
- **VA-API**: Firefox uses `libva` for hardware decode on Linux via its
  VA-API Platform Decode Module (PDM), running inside the RDD (Remote Data
  Decoder) sandboxed process.

---

## Source tree

```
rockchip-vaapi/
├── src/
│   ├── rockchip_drv_video.c   # Main driver: full VA-API vtable
│   ├── h264.c                 # H.264 SPS/PPS Annex B reconstruction
│   ├── h264.h
│   ├── hevc.c / hevc.h        # HEVC VPS/SPS/PPS reconstruction
│   ├── av1.c / av1.h          # AV1: VA parameters -> sequence/frame header OBUs
│   ├── av1/                   # vendored GStreamer AV1 bit writer + GLib-free shim
│   └── bs.h                   # Header-only Exp-Golomb bitstream writer
├── tests/av1/                 # raw-MPP harness, header dumper, Fluster + attribution scripts
├── evidence/                  # measured runs quoted by the docs
├── debian/                    # Debian packaging metadata
├── docs/
│   └── DEVELOPMENT.md         # This file
├── Makefile
├── README.md
└── INSTALL.md
```

---

## Architecture

```
Firefox (RDD process)
    │
    │  VA-API calls (libva 1.20)
    ▼
rockchip_drv_video.so          ← this driver
    │
    │  MppApi calls (librockchip-mpp1)
    ▼
Rockchip MPP
    │
    │  kernel ioctl
    ▼
RK3588 VPU (hardware)
    │
    │  DMA-BUF fd (PRIME 2)
    ▼
Firefox compositor (zero-copy NV12)
```

### Object model

The driver maintains four ID-namespaced flat arrays in a single `RKDriver`
heap allocation (`ctx->pDriverData`):

| Array | Max | ID base | Struct |
|-------|-----|---------|--------|
| `configs` | 16 | `0x01000000` | `RKConfig` |
| `contexts` | 8 | `0x02000000` | `RKContext` |
| `surfaces` | 64 | `0x03000000` | `RKSurface` |
| `buffers` | 256 | `0x04000000` | `RKBuffer` |

---

## VA-API vtable (`VADriverVTable`)

The entry point `__vaDriverInit_1_20` fills all 50+ function pointers. The
functions that actually do meaningful work are:

| Function | Role |
|----------|------|
| `rk_CreateConfig` | Validate profile/entrypoint; allocate `RKConfig` |
| `rk_CreateContext` | `mpp_create` + `mpp_init`; allocate `RKContext` |
| `rk_CreateSurfaces2` | Allocate `RKSurface` slots; surface dimensions stored |
| `rk_CreateBuffer` | `malloc` + copy caller data into `RKBuffer` |
| `rk_BeginPicture` | Store render target in `RKContext` |
| `rk_RenderPicture` | Collect buffer IDs into `pending[]` |
| `rk_EndPicture` | Dispatch to `do_h264_decode()` (or future codec dispatch) |
| `rk_QuerySurfaceAttrs` | Report NV12 + `DRM_PRIME_2` support (critical for Firefox) |
| `rk_ExportSurfaceHandle` | Return `VADRMPRIMESurfaceDescriptor` with `dup()`'d DMABUF fd |
| `rk_SyncSurface` | Wait on `surface->cond` until `decoded == true` |
| `rk_Terminate` | Destroy MPP contexts; close fds; `free(RKDriver)` |

All other vtable slots are filled with stubs that return `VA_STATUS_SUCCESS`
(or `VA_STATUS_ERROR_UNIMPLEMENTED` where appropriate) to satisfy libva's
initialization checks.

---

## H.264 decode pipeline

### The SPS/PPS problem

VA-API decouples parameter parsing from decoding. Firefox (via FFmpeg) parses
the H.264 bitstream and extracts the SPS and PPS as C structs
(`VAPictureParameterBufferH264`, `VAIQMatrixBufferH264`), then passes them via
`vaRenderPicture`. It does **not** pass the original binary NALUs.

MPP's `decode_put_packet` requires a complete Annex B bitstream including SPS
and PPS NALUs prepended before each IDR slice. The driver must therefore
**reconstruct** the binary SPS/PPS from the parsed structs before each IDR
frame.

### SPS/PPS reconstruction (`h264.c`)

`h264_write_sps()` and `h264_write_pps()` encode the structs back to binary
using Exp-Golomb coding:

1. A `BSWriter` (from `bs.h`) writes individual fields as `u(n)`, `ue(v)` or
   `se(v)` elements into a temporary `raw[]` buffer.
2. The NAL header byte is written first (`0x67` for SPS, `0x68` for PPS).
3. High-profile SPS includes `chroma_format_idc` and bit-depth fields.
4. `emulation_prevent()` scans the raw bytes and inserts `0x03` bytes before
   any `0x000001` or `0x000002` sequences (required by the H.264 spec).
5. A 4-byte Annex B start code (`00 00 00 01`) is prepended to the output.

### Slice dispatch (`do_h264_decode`)

For each `EndPicture` call the driver:

1. Finds the `VAPictureParameterBufferH264` and all `VASliceDataBuffer` blobs
   in `pending[]`.
2. On IDR frames (detected via `nal_ref_idc` / slice type), prepends SPS+PPS.
3. Prepends `00 00 00 01` start codes to each slice NALU.
4. Packs everything into a single MPP packet; encodes the target surface ID
   as the packet `pts` (used to route the decoded frame back to the right
   surface).
5. Calls `mpi->decode_put_packet`.
6. Polls `mpi->decode_get_frame` in a loop; matches the output frame by `pts`
   to find the target surface.
7. Stores the `MppFrame` in `surface->frame`, `dup()`s the DMABUF fd into
   `surface->prime_fd`, then signals `surface->cond`.

---

## DRM PRIME 2 surface export

Firefox calls `vaExportSurfaceHandle` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` to get a zero-copy handle to the
decoded frame's GPU memory.

`rk_ExportSurfaceHandle` returns a `VADRMPRIMESurfaceDescriptor`:

```c
desc->fourcc    = VA_FOURCC_NV12;
desc->width     = surface->width;
desc->height    = surface->height;
desc->num_layers = 2;
// Layer 0: Y plane
desc->layers[0].drm_format = DRM_FORMAT_R8;
desc->layers[0].object_index[0] = 0;
desc->layers[0].offset[0] = 0;
desc->layers[0].pitch[0]  = hstride;
// Layer 1: UV plane
desc->layers[1].drm_format = DRM_FORMAT_GR88;
desc->layers[1].object_index[0] = 0;
desc->layers[1].offset[0] = hstride * vstride;
desc->layers[1].pitch[0]  = hstride;
// Object (shared DMABUF)
desc->objects[0].fd             = dup(surface->prime_fd);
desc->objects[0].size           = hstride * vstride * 3 / 2;
desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
```

The `dup()` is essential: Firefox closes the fd when done; the driver keeps the
original alive in `surface->prime_fd` (tied to `surface->frame`).

### Why `vaQuerySurfaceAttributes` matters

This is the function Firefox calls **before** attempting hardware decode to
check whether the driver supports DRM PRIME 2. If it returns 0 attributes
or omits `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`, Firefox silently falls back
to software decode. The driver must return:

```c
// Pixel format
attrs[0].type  = VASurfaceAttribPixelFormat;
attrs[0].value.value.i = VA_FOURCC_NV12;
// Memory type: both VA and DRM PRIME 2
attrs[1].type  = VASurfaceAttribMemoryType;
attrs[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA
                       | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
// Max dimensions
attrs[2].type  = VASurfaceAttribMaxWidth;
attrs[2].value.value.i = 7680;
attrs[3].type  = VASurfaceAttribMaxHeight;
attrs[3].value.value.i = 4320;
```

---

## Memory model and the CMA constraint (4K)

There are two distinct pools of memory in play, and confusing them leads to
chasing the wrong bug:

| Buffer | Allocated by | Backing memory | Must be contiguous? |
|--------|--------------|----------------|---------------------|
| Per-surface output buffer (`priv_buf`) | this driver, in `CreateSurfaces` via `mpp_buffer_group_get_internal(MPP_BUFFER_TYPE_DRM)` | **system** dma-heap (not CMA) | no |
| MPP decode DPB (reference + work frames) | MPP internally | **CMA** (the VPU does DMA into it) | **yes** |
| GPU compositor textures | Mali / Mesa in Firefox | **CMA** | **yes** |

The driver copies each decoded frame out of MPP's DPB buffer into the surface's
own `priv_buf` (in `assign_mpp_frame`) so that MPP can immediately recycle its
small internal pool — see *Per-surface output buffers* below. Because `priv_buf`
lives in system memory, the driver is **not** a significant CMA consumer; you
can allocate hundreds of MB of surfaces without touching CMA.

The CMA pressure comes entirely from **MPP's DPB + the GPU compositor**, which
share the single kernel CMA region (`cma=` on the kernel command line, default
256 MB on most RK3588 images). A 4K NV12 frame is ~12.5 MB; VP9 keeps ~10
references, so the DPB alone is ~125–200 MB. Add the GPU's 4K compositing
buffers and 256 MB is not enough.

**Symptom of CMA exhaustion (important — it does not look like a driver bug):**
the 4K context acknowledges `info_change`, decodes 70–80 frames cleanly
(`copied=1` — or `zero-copy fd=` from 3.5.0 — no `TIMEOUT`, no VA error), then Firefox reports
`NS_ERROR_DOM_MEDIA_FATAL_ERR` with **no driver-side error at all**. MPP cannot
surface a failed contiguous allocation through the libva API, so from the
driver's point of view every call succeeded. The standalone `va_barcode_test`
decodes the same 4K stream fine because it does no GPU compositing and therefore
never contends for CMA.

**Fix:** raise CMA to `cma=512M` (or more) on the kernel command line. This is a
platform/deployment setting, not a driver change. See `INSTALL.md`.

Contrast this with AV1, which is a genuine *parse* failure: AV1 never
acknowledges `info_change` and decodes zero frames, because VA-API hands MPP
headerless tile data instead of a full OBU bitstream. CMA exhaustion is the
opposite — decode works, then a resource runs out. ***Fork: that was the
behaviour before v3.0.0; AV1 decodes now — the headers are rebuilt in `src/av1.c`.***

### Per-surface output buffers

Each `RKSurface` owns a permanent `priv_buf` allocated in `CreateSurfaces` and
kept for the surface's lifetime (its `prime_fd` never changes). `assign_mpp_frame`
copies MPP's decoded pixels into it using MPP's *real* hor/ver strides
(e.g. 3840×2176 for 4K — note the 2176 ver-stride padding) and then releases the
MPP frame immediately. This avoids two earlier bugs:

**Since 3.5.0 (8-bit zero-copy):** for a client that exports each surface again after
decoding into it (Firefox, mpv `hwdec=vaapi`), an 8-bit NV12 frame whose MPP stride is
64-aligned is *not* copied: the surface keeps the MppFrame (`zc_frame`) and
ExportSurfaceHandle hands out MPP's buffer with MPP's strides. That is safe now
because the driver holds a reference on the frame until the client renders into the
surface again (BeginPicture) — MPP cannot reuse a buffer while it is referenced — which
is exactly what the aliasing bug below lacked. Readers of `priv_buf` (Chromium imports
each surface once; vaDeriveImage/vaGetImage; a sync without an export; a writable
export) get the copy at the moment they read (`surface_zc_materialize`). 10-bit is
always copied (NV15 is repacked to P010). `RKVA_ZERO_COPY=0` restores the copy
everywhere. Measured on 4K60 AV1 in Firefox: the copy was 2.6 ms of every frame on the
decode thread.

- **Buffer aliasing ("saltando frames")**: MPP's internal display pool is only
  ~3 buffers for 4K; exporting MPP's fd directly meant MPP overwrote a buffer the
  compositor was still showing. Copying into a dedicated per-surface buffer fixes
  this.
- **Stale altref export**: VP9 altref (hidden, `show_frame=0`) frames are decoded
  by MPP as references but **not** output via `decode_get_frame` in this
  environment. The driver therefore does **not** poll/block for them (blocking
  per altref accumulated latency until Firefox's pipeline underran); it sends the
  altref to MPP, marks the surface decoded immediately, and lets the permanent
  `priv_buf` keep `ExportSurfaceHandle` valid.

## `bs.h` — Exp-Golomb bitstream writer

A header-only, zero-dependency, inline bitstream writer:

```c
BSWriter bs;
bs_init(&bs, buf, buf_size);

bs_write(&bs, value, n_bits);   // write n_bits of value
bs_write1(&bs, bit);            // write single bit
bs_write_ue(&bs, val);          // write unsigned Exp-Golomb (ue(v))
bs_write_se(&bs, val);          // write signed Exp-Golomb (se(v))
bs_rbsp_trailing(&bs);          // write RBSP trailing bits (1 + zeros to byte boundary)
size_t n = bs_bytes(&bs);       // number of bytes written
```

Exp-Golomb encoding for `ue(v)`:

```
val++ → find bit length k of val → write k zeros → write val in k+1 bits
```

---

## AV1 decode pipeline (v3.0.0)

MPP's `av1d` parses every header itself and wants a low-overhead OBU stream: the sequence header once, then one
`OBU_FRAME` per picture (frame header + tile group), `obu_has_size_field` set, no temporal delimiters. VA-API clients
deliver `VADecPictureParameterBufferAV1` plus one `VASliceParameterBufferAV1` element per tile and the tile bytes.
`src/av1.c` maps the VA structures onto the structs of GStreamer's `gstav1bitwriter.c` (vendored in `src/av1/`,
LGPL-2.0-or-later, with a GLib-free shim) and writes the OBUs; `do_av1_decode()` in the main file owns the policy.

What the VA parameters do not say, and how it is handled:

- **`refresh_frame_flags`** is absent. The successor picture's `ref_frame_map[]` shows which slots the previous picture
  took, so each picture is held until the next one arrives. If the client syncs the held picture first (Firefox and
  FFmpeg's download path do, every frame — measured: 1.47 av1d jobs per picture in Firefox at 4K60 with 3.0.0), it is
  decoded at once with mask 0 — correct picture, nothing refreshed. **With an MPP that speaks protocol 2 of the
  deferred refresh** (Plaid Pi's librockchip-mpp1 `+pd32` and later, RK3588 only; `mpp_av1d_rkva_enable()` on each
  AV1 decoder) that early decode carries an `'H'` padding OBU and MPP keeps the picture with its slot state; when the
  successor arrives a `'C'` padding OBU (mask + the hold's token) stores it into the slots — no second decode. With
  any other MPP the picture is re-submitted with the real mask later **as a hidden frame**, so the second decode
  never produces an output (the two decodes are not always identical and shared the surface).
  `RKVA_AV1_NO_DEFER=1` forces the re-decode. A shown KEY frame refreshes everything and is never held.
- **Clients that never wait.** Packets reach MPP from the per-context submit thread, so EndPicture returns before the
  picture is decoded; mpv, Firefox and FFmpeg with a download wait on each picture (vaSyncSurface, or an export after
  decoding) and never notice. FFmpeg with `-hwaccel_output_format vaapi` and no download never waits and is paced, at
  no measured cost (about 77 fps at 4K either way). Chromium syncs a surface only when it imports it (before its first decode) and then composites the
  dma-buf whenever the picture is due, so when it decodes ahead (start, seek, loop) it showed the surface's previous
  picture, or green on a surface's first use (3.5.0~rc1, caught by eye in a blind A/B and by a frame checker that
  compares every presented frame with 3.0.0's). Until a context's client has waited on a picture it rendered, and
  again after 32 pictures without such a wait, EndPicture waits until the picture from `RKVA_PACE` (default 2)
  pictures earlier is in its surface. The wait holds no driver lock and uses a monotonic cap of 150 ms (300 ms for a
  decoder's first 4 pictures, which include MPP's info change; Chromium holds its process-wide VA lock across
  vaEndPicture; measured waits at 4K60: mean 12-14 ms, max 62 ms playing, 91 ms around seeks, first output up to
  133 ms after mpp_init). It ends early when the surface is re-targeted or destroyed or the decoder destroyed, and in poll mode (`RKVA_SYNC_POLL=1`, or no output thread) drains MPP itself. AV1 only: the other codecs
  still feed MPP from EndPicture, which blocks on MPP's input queue as in 3.0.0 (MPP runs with IMMEDIATE_OUT, so
  H.264/HEVC come out in decode order).
  **Start window:** waiting for the picture two back leaves a new decoder's first pictures unguarded, and Chromium
  shows its first picture at once and keeps playing through a YouTube rung switch (a new decoder) while a new 4K
  decoder is slow for its first pictures (first output 44-133 ms after mpp_init, every surface and copy a first
  touch, temporal units with four hidden pictures before the shown one). Green frames at every start and switch on
  3.0.0 and 3.5.0~rc2 (G-Man, Chrome 155 and Chromium 154 on YouTube; the start/switch frame checker: MSE 1080p ->
  4K, every presented frame from the first against a software-decode reference; and a root read-only scanout
  checker that samples the composited framebuffer every vblank on first plays of the BBB film). So a paced context's
  first `RKVA_PACE_START` (25) shown pictures are decoded at once (a held picture forced early: 'H' with a capable MPP,
  a later re-decode otherwise) and EndPicture waits for each until it is decoded (3 s limit only: a 300 ms cap fired
  on a new 4K decoder's first picture in 1 of 7 launches and showed green). 3 pictures guarded the start, the switch
  needed 12, and on screen 12 still left a stale frame right after the film's fade-in (Chromium still catching up):
  screen glitches per first play, 30 s, 3 launches each: 12 -> 3/0/1, 25 -> 0/0/0. `RKVA_PACE_START_MS` adds a time
  floor (default 0; 12 + 2 s also gave 0/0/0). Cost: about 240 ms of EndPicture time per 1080p decoder start and
  700-830 ms per 4K one (decoding that happens anyway, now in EndPicture); Chromium film drops after the start are
  unchanged (0.22 % against rc2's 0.74 % in one 60 s run each). A client that waits ends the window at its first wait.
  Hidden pictures are never forced: shown early their header differs, MPP cannot hold them and decodes them twice
  (forcing them cost half of Chromium's frames at 4K); a temporal unit has exactly one shown frame, so the shown
  picture after a hidden one resolves it in the same EndPicture, and a later show_existing_frame (no VA call in
  Chromium or FFmpeg) finds it decoded. Waiting for every shown picture (`RKVA_PACE_START=all`) closes the remaining
  mid-stream case (KI-16) but drops 17-22 % of 4K60 frames in Chromium (self-wait ~30 ms per picture). After the window, with a capable MPP, every shown picture of a paced context is still forced early (no
  wait): one MPP job per picture as for Firefox, and the newest picture is never held (KI-15 fixed there). Both apply
  only to an importer: a client that exported a surface before the context first decoded into it (Chromium's import
  at pool creation, GStreamer). mpv waits on its pictures but only after running a dozen pictures ahead; without the
  gate every mpv start waited in EndPicture for the window (53 -> 50 plays a minute in the fast-abandon test).
- **Hidden frames** (`show_frame = 0`): clients never send `show_existing_frame` to a VA driver, but they do read the
  hidden picture's surface when the stream shows it later. The driver follows a hidden picture with a synthesised
  `show_existing_frame` of one slot it refreshed; a hidden **KEY** frame instead gets a second decode of the same
  tiles as a shown `INTRA_ONLY` frame with refresh 0, because `show_existing` of a KEY makes the decoder refresh
  every slot and reset state. A hidden picture synced early is decoded as shown (INTRA_ONLY if it was a KEY).
- **Film grain:** with `apply_grain` the client renders into `current_frame` (reused between pictures) and reads
  `current_display_picture`. MPP applies no grain on RK3588 (#974), so the clean picture is keyed on the display
  surface and the reconstruct surface is only marked decoded.
- **Derived header fields:** tile log2 counts from the client's tile counts (uniform case) or the SB-aligned starts
  (non-uniform), skip-mode frames from the slots' order hints, `PrevGmParams` from the saved global motion of the
  primary reference, segmentation `update_data` passed through, loop restoration `lr_unit_shift`/`lr_uv_shift` as
  final spec values (what FFmpeg and Chromium pass).
- **Sequence header:** derived from the first picture (max size = that picture); `enable_superres` and
  `separate_uv_delta_q` are set so the per-frame bits can be expressed; a picture that no longer matches starts a new
  sequence header (MPP info change).
- **Refused** (`av1_frame_from_va` / `av1_seq_from_va` return -1): Profile 1/2, 12-bit, monochrome, super-resolution,
  large-scale tile, switch frames, an INTRA_ONLY frame with refresh 0xff, more than 64 tile columns or rows. A picture
  whose tile count does not match its header, or whose slice buffers are malformed, is refused at EndPicture; every
  refusal marks the picture's surfaces decoded so a waiting vaSyncSurface returns at once.

Changes to the vendored writer, each marked `rockchip-vaapi fix (n)` in the source: (1) segmentation params implemented (upstream had a TODO);
(2) loop-filter delta updates; (3) `PrevGmParams` taken from the struct address instead of `.gm_params`; (4)
`UsesChromaLr` only for Cr; (5) `lr_unit_shift` for 64x64 superblocks written as one bit; (6) `_av1_write_uniform()`
success treated as failure, so non-uniform tiles could never be written. Upstream GStreamer candidates.

Tools in `tests/av1/`: `av1mpp.c` (raw MPP decode of an IVF, the attribution reference), `fhdump.c` (every frame
header field, builds against gstreamer-codecparsers), `obulist.py`, `addtd.py` (temporal delimiters so dav1d/ffmpeg
accept a dumped stream), `obu2ivf.py`, `fhmatch.py`, `fluster-run.sh`, `attrib.sh` (driver vs raw MPP vs dav1d per
failed vector). Switches: `RKVA_DUMP_AV1=file` appends the stream sent to MPP; `RKVA_AV1_WRITER_DEBUG=1|2`; `RKVA_AV1_NO_GRAIN=1`
strips `apply_grain` from the written header (experiment switch; no visible effect on RK3588, where MPP applies none).

---

## Firefox process model

Firefox decodes video in the **RDD (Remote Data Decoder)** sandboxed process.
The VA-API driver runs inside RDD. By default, RDD's seccomp sandbox blocks
`/dev/dri` device access, which prevents MPP from opening the VPU. The
workaround is `MOZ_DISABLE_RDD_SANDBOX=1`.

For a hardened deployment, the correct long-term fix is to add `/dev/dri` and
Rockchip's `/dev/mpp_service` to Firefox's RDD sandbox allow-list (requires
patching Firefox's sandbox policy).

---

## Known limitations (upstream, April 2026 — annotated by the fork)

- H.264 SPS reconstruction uses `level_idc=51` (5.1) unconditionally. *(Still true; harmless
  to MPP, players take the level from the container.)*
- `num_ref_idx_l0/l1_default` in PPS is hardcoded to 0 (= 1 reference). Multi-reference
  B-frame content may require this to match the stream. ***Fixed in the fork (v2.1.2): this
  was exactly the drift/ghosting on B-frame streams (KI-1).***
- Maximum 64 surfaces / 8 concurrent decode contexts. ***Fork: 128 surfaces (v2.1.4).***
- Only H.264 header reconstruction is implemented. HEVC/VP9/AV1 decode paths call
  `do_h264_decode` as a stub. ***Fork: HEVC assembler real since v2.1.0; VP9 decodes; AV1
  since v3.0.0 — the driver re-serialises the OBU headers from the VA parameters (see "AV1 decode pipeline").***

## AI-assisted development notice

This driver was designed and implemented through interactive pair programming
with **Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), a large language
model developed by Anthropic.

**Development timeline:** 24 April 2026
**Estimated interactive development time:** ~3–4 hours across two sessions

**AI contributions:**
- Initial architecture design (VA-API vtable layout, object model, ID namespacing)
- Complete `rockchip_drv_video.c` implementation (~1,000 lines)
- H.264 Annex B SPS/PPS reconstruction algorithm (`h264.c`, `bs.h`)
- DRM PRIME 2 / DMABUF surface export implementation
- Iterative debugging: `MPP_FRAME_FLAG_EOS` compatibility, STUB macro issues,
  `max_subpic_formats` libva init requirement, `rk_QuerySurfaceAttrs`
  returning correct attributes for Firefox
- Debian package structure (`debian/`)

**Human contributions:**
- Problem identification and system analysis (RK3588 hardware context)
- Live testing on Orange Pi 5 Plus hardware
- Firefox `about:config` configuration and environment variable tuning
- Validation that video plays correctly with hardware acceleration active
- Code review and approval at each iteration
- License, authorship, and packaging decisions

All code was validated on real hardware by Eduardo García-Mádico Portabella —
EGP Sistemas.
