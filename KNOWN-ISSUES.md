# Known Issues

Hardware-verified on Orange Pi 5B (RK3588S) unless noted. Reports from other
RK3588/RK3588S boards are welcome — decode silicon is identical across the two,
so behaviour should match.

Current as of **v2.0.0 "Reframe"**.

---

## KI-1: H.264 B-frame corruption — ROOT CAUSE FOUND, fixed in v2.1.2

**Status:** fixed in v2.1.2 · **v2.1.1 and earlier are affected — update.**

The full story, because it hid from everyone including us: the driver synthesises the
H.264 parameter sets, and the PPS hardcoded `num_ref_idx_default_active = 1` as a
"conservative default". Real encoders set it higher (broadcast content: 4; typical x264:
2), and slices rely on that default exactly once the decoder's reference buffer fills —
about half a second in. So every I-frame was pixel-perfect, playback *started* clean, and
the picture then drifted into ghosting. Short verification windows (ours included) sailed
past it, which is how it shipped: **v2.1.1's "correct from start" claim was measured on
12-frame windows and is wrong beyond the first GOP.** It also masqueraded as a
seek-only bug for a full day of investigation.

v2.1.2 learns the true defaults from the stream itself (any slice that doesn't override
them states them) and re-emits a corrected PPS once. Verified 0.0/255 across 144-frame
runs on broadcast-style and open-GOP reproducers, all seek cases included, plus the full
regression suite at the new 48-frame minimum depth.

If you saw ghosting or "flicker" on H.264 with any earlier version, it was this.

## KI-2: HEVC — FIXED, and now advertised (8-bit)

**Status:** fixed on `deep-ink`; `HEVCMain` restored to the advertised list · **Severity:** was total

HEVC had never decoded through this driver, at any bit depth — there was no HEVC bitstream
assembler, so the decoder received a stream it could not parse and returned empty (green)
buffers. The assembler now exists: VPS synthesised, SPS/PPS reconstructed from the VA-API
parameters, Annex B stitching, with two requirements found on hardware — `max_num_reorder_pics`
must be 0 for a stateless bridge, and the reference-picture-set count must be taken from
`num_short_term_ref_pic_sets` and never revised mid-stream (re-sending a changed SPS flushes
the decoder's picture buffer and destroys the references B-frames depend on).

Verified pixel-identical to software decode at 8-bit and 10-bit, with a full 12-minute
4096×1714 Main10 feature playing through end to end. **HEVC Main (8-bit) is now advertised
and hardware-decodes in both Firefox and Chrome.** Main10 stays unadvertised — see KI-3.

## KI-3: 10-bit zero-copy display — FIXED in v2.1.3 (a mistyped fourcc, never the GPU)

**Status:** fixed in v2.1.3 · **v2.0.0 – v2.1.2 are affected — update.**

**What users saw:** 10-bit content (HEVC Main10, VP9 Profile 2, H.264 High10) never
displayed through the zero-copy path. mpv `--hwdec=vaapi` and Firefox silently fell back
to software decode; Chromium showed green. Earlier versions of this file blamed a
"panfork 16-bit texture wall" and declared the display path blocked until Mesa ≥ 25.
That diagnosis was wrong.

**Root cause:** the 10-bit `SEPARATE_LAYERS` export labelled the chroma plane with the
fourcc `0x36315247`. That spells `"GR16"`, which is not a DRM format. `DRM_FORMAT_GR1616`
is `fourcc('G','R','3','2')` = `0x32335247`. Mesa rejected every 10-bit UV plane with
`EGL_BAD_MATCH: unknown drm fourcc format` — a message that only appears with
`EGL_LOG_LEVEL=debug`. The luma plane (`R16`) was fine, so every consumer got a valid Y
plane and a refused UV plane, and each handled that its own way (fallback or green).

**Why it hid for three releases:**
1. mpv's format probe passed — the driver's pre-decode placeholder surface exports as
   8-bit (`R8`/`GR88`), so the probe never touched the 10-bit branch.
2. mpv 0.38 also mis-renders *software-uploaded* 10-bit frames on this GPU stack (a
   separate, real symptom), which made "16-bit textures are broken" plausible.
3. Nobody captured Mesa's own error text. `EGL_LOG_LEVEL=debug` names the failing check
   in one line. That is now a rule in AGENTS.md.

**How it was found (2026-09-06):** a forum report of `mpv --hwdec=rkmpp
--vf=scale_rkrga=force_yuv=auto` displaying P010 correctly on the same panfork stack.
mpv's `dmabuf_interop_gl` splits that P010 into exactly the `R16` + `GR1616` EGLImages
this driver exports — same GPU, same fourccs, same modifier — so the GPU was exonerated
and the descriptor was the only variable left. Modifier and buffer-allocator variants
changed nothing; the fourcc did.

**Measured on RK3588S (Orange Pi 5B, BSP 6.1, panfork Mesa 23.0.5), 4K HEVC Main10:**

| test | v2.1.2 | v2.1.3 |
|---|---|---|
| mpv `--hwdec=vaapi`, EGL import failures per 40 frames | 40 | **0** |
| mpv VO format | `yuv420p10` (software fallback) | **`vaapi[p010]` zero-copy**, correct picture |
| Firefox, Main10 | software fallback | **hardware, real time** (360 ten-bit surfaces in 35 s), correct picture |
| 8-bit H.264 / HEVC zero-copy (regression) | correct | correct |

**Menu:** Main10, High10 and VP9 Profile 2 are advertised by default from v2.1.3.
`RKVA_HIDE_10BIT=1` restores the 8-bit-only menu for a client that still cannot present
P010. (Pi Desktop's own Chromium never sees this menu — see KI-6.) The `[tenbit-panfork-guard]`
mpv profile shipped in the image config is now believed unnecessary: software-decoded
10-bit also displayed correctly in mpv 0.38 on 2026-09-06; it will be retired with the
next image once re-measured.

**Reproduce the failure on an old build:** `EGL_LOG_LEVEL=debug RKVA_ADVERTISE_ALL=1 mpv
--hwdec=vaapi <main10 file> 2>&1 | grep "EGL user error"`.

---

## KI-4: Chromium-family UI flicker is not a driver issue

**Status:** upstream/GPU stack · **Severity:** cosmetic

Flickering while typing or redrawing UI in Chromium/Chrome comes from ANGLE
(mandatory in modern Chromium) on the panfork GPU stack, independent of video
decode. Firefox does not use ANGLE and is unaffected. If video *content* is
corrupt rather than the UI flickering, that is a different problem — please
report it.

---

## KI-5: some HEVC streams use SPS-indexed reference-picture sets (not yet reconstructible)

**Status:** open (Phase 1.6 candidate) · **Severity:** affected streams fall back to software

Slices that reference SPS-level RPS entries by index (`short_term_ref_pic_set_sps_flag=1`)
or use inter-RPS prediction cannot be rebuilt from what VA-API provides — the set tables
are not in the parameter struct. The driver detects this and logs a WARNING rather than
guessing. The planned fix rebuilds the picture's RPS from `ReferenceFrames` and rewrites
the slice header with an explicit inline set. Long-term SPS reference sets and custom
scaling lists are likewise not yet handled (defaults are used).

## KI-7: surface pool exhaustion on 4K in Chrome — FIXED in v2.1.4

**Status:** fixed in v2.1.4 · **v2.1.3 and earlier affected** (Chrome, 4K; other clients rarely)

**What users saw (forum report, 2026-09-07, google-chrome with the VA-API decoder, 4K 10-bit):**
```
vaapi_wrapper.cc: vaCreateSurfaces (allocate mode) failed, VA error: resource allocation failed
```
five times, then garbled video, then a green freeze.

**Root cause:** the driver kept a fixed pool of 64 surfaces. Chrome's Linux VA-API decoder
allocates one VA surface per output frame, sizes its frame pool at reference frames + 1 + a
renderer estimate, and holds frames while the compositor is busy; a 4K stream can exceed 64.
Worse, when a surface's private buffer could not be allocated the driver returned success
with an empty surface, so the client decoded into a half-built pool.

**Fix:** pool raised to 128, exhaustion logged loudly, and any allocation failure now rolls
back and returns `VA_STATUS_ERROR_ALLOCATION_FAILED`, so the client falls back to software
instead of corrupting. **Why not more than 128:** every surface carries a private
3-bytes-per-pixel buffer (19.7 MB at 4K) mapped through the Rockchip DRM IOMMU, whose
I/O-virtual space is 4 GB; measured on RK3588S, ~200 live 4K surfaces hit
`rockchip_gem_iommu_map: out of I/O virtual memory` and decode failed. 128 keeps 4K near
2.5 GB. Receipts: 89 live 4K surfaces clean; a 229-surface request fails cleanly with zero
decode errors and ffmpeg falls back.

---

## KI-6: Chromium on Pi Desktop does not use this driver — its own decode lane aborted on 10-bit (mitigated in the fork)

**Status:** mitigated outside this driver (defcom5-libv4l-rkmpp v1.8.0-defcom5.1, 2026-09-07) · real 10-bit output in Chromium still open

The `+rkmpp` Chromium shipped on Pi Desktop images (`chromium 132 …+rkmpp`) decodes video through **libv4l-rkmpp**, a V4L2 plug-in that talks to MPP directly. It
never loads `rockchip_drv_video.so` (verified 2026-09-06: no driver log across an 8-bit and a
10-bit playback, while the session journal shows `libv4l2` and `mpp` initialising inside the
Chromium process). Nothing in this driver's profile menu affects it.

**What was broken:** the PPA's plug-in advertised HEVC Main 10, H.264 High 10 and VP9 Profile 2
but outputs NV12 only, and aborted the whole GPU process on the first 10-bit frame:
```
chromium: ../src/libv4l-rkmpp-dec.c:247: rkmpp_apply_info_change:
  Assertion `dec->video_info.mpp_format == MPP_FMT_YUV420SP' failed.
GPU process exited unexpectedly: exit_code=6
```
(upstream issue #21, open since 2024).

**Mitigation, shipped in the images:** [defcom5-libv4l-rkmpp](https://github.com/defcom5-rockchip/defcom5-libv4l-rkmpp)
(fork of upstream master `c5bc0ae`) hides the 10-bit profiles so Chromium reports them
unsupported and falls back (software VP9; a media server transcodes HEVC), and replaces the
assert with error-flagged frames. Measured on RK3588S with `PlatformHEVCDecoderSupport` on:
8-bit HEVC 1080p in hardware (1543 frames / 25 s), 8-bit H.264 in hardware, 10-bit HEVC 4K and
VP9 P2 4K **0 GPU crashes** (was 1 per attempt).

**Still open:** hardware 10-bit in Chromium needs NV15 → NV12 on RGA3 inside the plug-in (the
RK3588 decoder cannot down-convert itself). Stock VA-API Chromium builds elsewhere go through
this driver instead and got the v2.1.3/v2.1.4 fixes.

---

## KI-8: undecoded surfaces were described as 8-bit — Chrome's 10-bit green — FIXED in v2.1.5

**Status:** fixed in v2.1.5 · **v2.1.4 and earlier affected** (Chromium with the VA-API decoder, mpv 0.36)

**What users saw:** Chromium (VA-API builds) playing 10-bit HEVC or VP9 Profile 2: garbled
frames, then a green screen, with `vaCreateSurfaces (allocate mode) failed` in its log. mpv
0.36 with `--hwdec=vaapi`: silent fallback to software on 10-bit.

**Root cause:** Chrome's decoder creates a surface, **exports it, and only then decodes into
it**. The driver decided "10-bit or 8-bit" from the last decoded frame, which did not exist
yet, so every export described a 10-bit surface as 8-bit NV12 (`R8`/`GR88`, byte pitch =
width). Chrome imported P010 pixels through that description. A tester's driver log showed it
exactly: 11,983 `CreateSurfaces2` calls, zero `10bit=1` exports. mpv 0.36's format probe does
the same export-before-decode and rejected the mismatch; Firefox and mpv 0.38 happened to
export after decode or tolerate the probe.

**Fix:** the surface remembers the bit depth it was **created** with (RT format
`VA_RT_FORMAT_YUV420_10` or a `P010` pixel-format attribute) and every description of it
(export, derive, get-image) follows that until a frame has been decoded. Verified with a
small libva test (`tests/va-export-before-decode.c`): a 10-bit surface now exports as
`P010` / `R16` + `GR1616`, pitch 2×width, before any decode; 8-bit surfaces unchanged.
Firefox regression: 201 ten-bit exports, no mismatch warnings.

---

## KI-9: libva cannot find the driver on Panthor / Panfrost GPU stacks without help — shipped fix in v2.1.5

**Status:** worked around in the package · **affects** images whose GPU is driven by the
mainline `panthor` (or `panfrost`) kernel driver with Mesa ≥ 24, e.g. Armbian vendor kernels
with the Panthor backport and a kisak Mesa

libva derives the VA driver name from the render node's kernel driver, so on those images it
looks for `panthor_drv_video.so` (or `panfrost_…`) and gives up, and mpv, vainfo and
GStreamer fall back to software while Firefox, which opens a different node, still works.
From v2.1.5 the package ships `panthor_drv_video.so` and `panfrost_drv_video.so` as symlinks
to the driver, so auto-detection succeeds. The equivalent manual fix on any version is
`LIBVA_DRIVER_NAME=rockchip` in the environment (Pi Desktop sets it system-wide).

---

## KI-10: garbled 10-bit VP9 after ~18 minutes — update your MPP library

**Status:** external, root cause identified · **Severity:** playback corrupts until the player is restarted

Long 10-bit VP9 playback (VP9 **Profile 2**) breaks after roughly **65,536 decoded frames in one session** —
about 18 minutes at 60 fps, 45 minutes at 24 fps. The picture corrupts and `dmesg` fills with
`mpp_rkvdec2 ...rkvdec-core: resetting for err 0x23` at roughly 24 resets per second.

**This driver is not involved.** It reproduces with Rockchip's own headless `mpi_dec_test` — no VA-API, no
display, no dma-buf export.

The fault is in the `librockchip_mpp` your system is using. Several downstream builds carry a patch that
reverts upstream's `fix[hal_vp9d]: not support fast mode for rk3588`, re-enabling two parallel VP9 HAL tasks
on a SoC where upstream deliberately disables them. Upstream MPP is unaffected at every point tested,
including the exact commit those builds branch from — reverting that one patch runs clean past 73,000 frames.

**Fix:** use an MPP built from upstream — https://github.com/rockchip-linux/mpp

**Not affected:** 8-bit VP9 (Profile 0) at any length, and HEVC Main10 — both verified clean past 70,000
frames on the same affected library. Anything that recreates the decoder resets the count, so short clips and
browser streaming (which rebuilds the decoder on quality changes) rarely show it.

**Workaround** if you cannot change the library — costs ~10-17% decode throughput, well clear of real-time:

    echo 1 > /proc/mpp_service/rkvdec-core1/disable_work

---

## Reporting

Open an issue with: board model, kernel (`uname -a`), distro, driver version
(`vainfo` prints it), the codec/resolution involved, and `ffprobe` output for a
local file that reproduces it. For browser reports, `about:support` (Firefox) or
`chrome://gpu` (Chrome) plus whether `sudo fuser -v /dev/mpp_service` shows the
decoder process during playback.
