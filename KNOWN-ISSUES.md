# Known Issues

Hardware-verified on Orange Pi 5B (RK3588S) unless noted. Reports from other
RK3588/RK3588S boards are welcome — decode silicon is identical across the two,
so behaviour should match.

Current as of **v3.5.0**; fixes that landed in the v4.0.0 development tree are marked as such.

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

**Status:** fixed in v2.1.0; `HEVCMain` advertised since then · **Severity:** was total

HEVC had never decoded through this driver, at any bit depth — there was no HEVC bitstream
assembler, so the decoder received a stream it could not parse and returned empty (green)
buffers. The assembler now exists: VPS synthesised, SPS/PPS reconstructed from the VA-API
parameters, Annex B stitching, with two requirements found on hardware — `max_num_reorder_pics`
must be 0 for a stateless bridge, and the reference-picture-set count must be taken from
`num_short_term_ref_pic_sets` and never revised mid-stream (re-sending a changed SPS flushes
the decoder's picture buffer and destroys the references B-frames depend on).

Verified pixel-identical to software decode at 8-bit and 10-bit, with a full 12-minute
4096×1714 Main10 feature playing through end to end. **HEVC Main (8-bit) is now advertised
and hardware-decodes in both Firefox and Chrome.** Main10 is advertised since v2.1.3 — see KI-3.

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
P010. These measurements were made on panfork (Mesa). On Arm's proprietary Mali GL driver,
clients that import P010 plane by plane draw it near-black — see KI-11.

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

## KI-5: HEVC slices that reference SPS-level reference-picture sets decode with wrong references (can stall the decoder)

**Status:** open (next release) · **Severity:** wrong picture or a decoder stall · measured 2026-10-23

Slices that reference SPS-level RPS entries by index (`short_term_ref_pic_set_sps_flag=1`) or use inter-RPS prediction
cannot be rebuilt from what VA-API provides — the set tables are not in the parameter struct. The driver detects this,
logs a WARNING, and continues with the slice header as written: the hardware then uses the wrong references. There is
**no software fallback** (an earlier version of this entry said there was). Measured with the JCT-VC-HEVC_V1 suite through
`ffmpeg -hwaccel vaapi`: most Main vectors fail this way; `AMP_A_Samsung_7` decodes one frame and then stalls the decoder
(`SyncSurface: TIMEOUT`, 178 rkvdec resets during the HEVC suite). Streams that write their reference sets inline in the
slice header (what the x265-encoded test files used here do) are unaffected. The planned fix rebuilds the picture's RPS
from `ReferenceFrames` and rewrites the slice header with an explicit inline set; long-term SPS reference sets and custom
scaling lists (5 of the 147 conformance vectors) need the same treatment.

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

## KI-6: Chromium builds that decode through libv4l-rkmpp do not use this driver — 10-bit aborted there (mitigated in the fork)

**Status:** historical for Pi Desktop — its Chromium moved to the VA-API route in Pi Desktop 2.0.3 and now uses this
driver (10-bit hardware decode with a correct picture, checked 2026-10-09). Still applies to Chromium builds that decode
through libv4l-rkmpp.

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

## KI-10: VP9 reset storm after ~65,536 frames in one session — fixed in MPP (not in this driver)

**Status:** external, root cause found and fixed in MPP · **Severity:** frames lost until the player is restarted

Long VP9 playback (8- or 10-bit) broke after roughly **65,536 decoded frames in one session** — about 18 minutes at
60 fps — with `dmesg` filling with `mpp_rkvdec2 ...rkvdec-core: resetting for err 0x23` / `0x107` and frames silently
lost. **This driver is not involved**: it reproduces with Rockchip's own headless `mpi_dec_test`.

**Root cause (October 2026):** VP9 has no picture order count, so MPP's parser labels frames with a counter that never
wraps, and the RK3588's two decoder cores behave as if they compare those labels in 16 bits when one waits for a frame
on the other. Once the counter passes 65,535 the wait breaks. Upstream MPP had hidden this by keeping one VP9 frame in
flight on RK3588 (`support_fast_mode = 0`; the cores still alternate); the Ubuntu-Rockchip PPAs and Jellyfin's bundled
MPP revert that guard, run two frames at once and so hit the fault. Keeping the counter within 16 bits removes it with
two frames in flight: in its first form (`+pd33`, two tasks) 77,400 decoder jobs with zero resets and about 27 % more
10-bit throughput than the one-frame guard (Orange Pi 5B, `mpi_dec_test`); Chromium, Firefox and mpv ran 4K60 VP9 past
the old failure point with no resets, and the same change ran clean on an RK3588 (Orange Pi 5 Plus).

**Fix:** librockchip-mpp1 `1.5.0+git20260805.a8b19653+ds-0ubuntu1~rk1+pd33` or later (Plaid Pi; patch 0008 in its
source package). `+pd34` carries the form Rockchip's reviewer asked for in
[rockchip-linux/mpp PR #976](https://github.com/rockchip-linux/mpp/pull/976): no RK3588 exception, MPP's default task
count. On synthetic 4K60 clips it decodes at 214 / 233 fps (10-/8-bit) against 149 / 181 with the guard, and ran
201,600 frames through three wraps at 214 / 231 fps with 0 resets (`mpi_dec_test`, Orange Pi 5B). Rockchip reproduced
the fault and says the change will go through its internal Gerrit review when merged; the PR is still open. `+pd34` is
published for Ubuntu 24.04 and 26.04 at
https://github.com/defcom5-rockchip/mpp/releases/tag/pd34: `sudo apt install ./librockchip-mpp1_…_arm64.deb` (exact
file names per Ubuntu release, and the Plaid Pi 5.0 case, in the release notes). Any MPP with upstream's one-frame
guard is also safe, at the lower throughput.

**Workaround** if you cannot change the library — costs ~17 % decode throughput, lost at reboot:

    echo 1 > /proc/mpp_service/rkvdec-core1/disable_work

---

## KI-11: 10-bit draws near-black on Arm's proprietary Mali GL driver — external

**Status:** GL driver bug, not this driver · Mesa (panfork) was unaffected in our tests

Arm's proprietary Mali GL driver (libmali g24p0) imports a `DRM_FORMAT_R16` dma-buf as an integer texture
(`GL_R16UI`), and a normal shader sampler reads it as 0. Clients that import a P010 frame plane by plane — mpv's
VA-API zero-copy path and Firefox — therefore draw 10-bit video with no brightness: near-black, only colour left. The
decoded frames are correct; copy-back draws them right. The newer g29p1 driver is worse: it imports the chroma plane
(`GR1616`) as an integer texture too. Chrome and Chromium draw 10-bit correctly on the same driver (checked by eye
with a VP9 Profile 2 clip), probably because they import the whole frame, which the driver converts correctly.

**Fixes:** mpv 0.41 with the fork's patches (Plaid Pi's `+pd1`/`+pd2`) queries the texture's internal format after the import
and samples an integer luma plane as such in the shader (no copy; the refused 8-bit chroma plane is copied). Firefox: Plaid Pi
loads a small library (libpidesktop-ffmali) as Firefox's EGL/GL library that redoes the imports, so the unmodified Firefox
works; a Firefox patch with the same check in `DMABufSurfaceYUV::CreateTexture()` is under review (Bugzilla 2078820) — 10-bit VP9 Profile 2 and AV1 then draw correctly from hardware (checked by eye; HEVC Main10 uses the same import path and is expected to follow, not yet checked).

**Workarounds for unpatched players:** mpv `--hwdec=vaapi-copy` (still decoded in hardware). For a client that cannot
be told to copy, set `RKVA_HIDE_10BIT=1` in that client's environment only, for example `env RKVA_HIDE_10BIT=1 firefox`:
it then decodes 10-bit in software and keeps 8-bit in hardware. From v3.0.0 the switch also covers AV1 (8- and
10-bit AV1 share one VA profile, so the driver answers 8-bit only for every profile and refuses a 10-bit AV1 picture
before the decoder sees it; the client falls back to software). Don't set it system-wide; any value, even `0`, hides 10-bit.

---

## KI-12: abandoned decoders, several decoders or threads at once — green, black or wrong frames, crashes — FIXED in v2.5.0

**Status:** fixed in v2.5.0 · **v2.2.0 and earlier affected**

- **A player that abandons decoders**, one after another: mpv trying zero-copy, giving up and destroying its decoder
  within a fraction of a second left about 70 MB of decoder buffers mapped at 4K each time. A looping player exhausted
  the video decoder's IOMMU after about 55 attempts and the picture went green.
- **Several decoders on one VA display**, as in Firefox's media process (one display for all videos on a page):
  decoders starting at the same moment could claim the same internal slot. Frames went to the wrong decoder (black or
  green tiles), or the process crashed.
- **Several threads**, as in ffmpeg 8 (decoding and frame download on different threads): image and slice buffers
  created on different threads could collide ("invalid VABufferID", I/O errors, heap corruption).

Measured on an Orange Pi 5B. Four 10-bit decodes on one VA display, every frame compared with a software decode:
bit-exact in 55 of 55 runs on the release candidate and 15 of 15 on the final code with hardware decode forced
(2.2.0: 0 of 20). Abandoned-decoder loops: 10,923 decoders in 60 minutes on the candidate and 1,857 in 10 minutes on
the final code, with the number of held decoder buffers staying flat (2.2.0: 273 held after 30 seconds).

---

## KI-13: AV1 — what the hardware path does not do (v3.0.0)

**Status:** limits, by design or by the hardware · decode is otherwise bit-exact against dav1d

- **Film grain needs the MPP fix.** MPP's av1d HAL never set the register that turns grain synthesis on (rockchip-mpp
  issue #974; a pull request with the one-line fix is prepared, 2026-10-25). With MPP up to `+pd34` a stream with
  `apply_grain` decodes to the clean reference picture; with Plaid Pi's `+pd35` the hardware applies the grain, bit-exact
  against dav1d on 12 streams (446 frames) including inherited parameters, 4K60 and AFBC output. No player adds grain a
  second time on this path: ffmpeg's hwaccel sets `apply_grain` only when it does not export the grain itself, and mpv's
  `film-grain=cpu` is the only mode that renders grain on the Mali driver (measured 2026-10-24). `RKVA_AV1_NO_GRAIN=1`
  switches it off for a decode.
- **Tiny frames and spatial layers.** 21 of libaom's 36 tiny-frame vectors (`av1-1-b8-01-size-*`, both dimensions
  ≤ 66 px) and the two spatial-layer vectors (`svc-L2T*`) fail on the decoder itself: the raw MPP decoder, fed the
  original stream, does not match dav1d on them either (same mismatch counts for the size vectors; for svc the raw
  decoder drops a frame and differs on another, the driver differs on all eight). Not met in practice (web video
  starts at 144p).
- **A frame-size change inside one VA context** (libaom `sizeup`/`sizedown`: 352×288 ↔ 1280×720 mid-stream): FFmpeg
  keeps the surfaces of the first size, so the picture is cropped to the surface or sits in the corner of it, and the
  raw MPP decoder fails `sizeup` as well (11 of 20 frames). The driver never writes past the surface (v3.0.0; earlier
  test builds corrupted the heap here). Browsers create a new decoder on a resolution change; these vectors do not.
  Measured 2026-10-24 (v3.5.0, MPP `+pd34` and `+pd35` identically): on `sizeup`, ffmpeg's own download path can **read**
  past the end of the frame it downloaded — the frame is the old size, the stream says the new one, and libswscale reads
  the difference: 3 of 20 runs of Fluster's exact command segfault in memcpy; the rest finish with the known wrong md5,
  which can include adjacent heap bytes in the picture (out-of-bounds read, never a write). Fix planned for the
  resolution-change path in v4.0.0; the measurement is in the maintainer's notes (not yet in `evidence/`).
- **Refused, so the client falls back to software:** Profile 1/2 (4:4:4, 4:2:2), 12-bit, monochrome, super-resolution
  frames, large-scale tile frames, switch frames, more than 64 tile columns or rows. ffmpeg's hwaccel path (`-c:v av1 -hwaccel vaapi`) reports an error instead of falling
  back — use `-c:v libdav1d`; players that pick libdav1d themselves fall back cleanly.
- Chromium's `48delayed` stress vector: 27 of 557 frames differ in its last 30, where the raw MPP decoder diverges
  from dav1d as well.

Conformance runs (Fluster, `ffmpeg -hwaccel vaapi`; every failure except monochrome and `sizedown` attributed
against the raw decoder):
libaom AV1-TEST-VECTORS 213/242, Chromium 10-bit 22/23, Chromium 8-bit 9/13 — files and the attribution table in
`evidence/2026-10-11-av1-fluster/`, scripts in `tests/av1/`.

## KI-14: AV1 decoded twice per early-synced picture without Plaid Pi's MPP (v3.5.0)

**Status:** by design; fixed where MPP has the feature

VA-API never tells the driver `refresh_frame_flags`; it is read from the next picture's reference map. A client that
syncs each picture before submitting the next (Firefox, FFmpeg's download path) therefore forces an early decode
with refresh 0. With librockchip-mpp1 `1.5.0+git20260805.a8b19653+ds-0ubuntu1~rk1+pd32` or later (Plaid Pi; patches
in its source package) on RK3588, MPP keeps that decode and the driver commits its reference slots afterwards: one
AV1-engine job per picture. With any other MPP (stock, distro, non-RK3588) the driver decodes such a picture a second
time with the real mask: correct, but about 1.5 jobs per picture — at 10.5 ms per 4K job that is nearly the whole
16.7 ms 4K60 frame budget. With 3.0.0 Firefox dropped most 4K60 AV1 frames there and switched to software; 3.5.0 on
such MPP builds has only been measured through ffmpeg (same speed as 3.0.0), not in Firefox. `RK_VAAPI_LOG` says which
path is in use ("deferred refresh available" / "off (this MPP lacks it ...)" — the only proof that the patched MPP is
active; a package version alone is not); `RKVA_AV1_NO_DEFER` (any value) forces the re-decode.

**Workaround:** install Plaid Pi's MPP (`+pd32` or later) on RK3588 — published for Ubuntu 24.04 and 26.04 at
https://github.com/defcom5-rockchip/mpp/releases/tag/pd34. Firefox tip: `media.video-queue.hw-accel-size` = 10
in about:config lets Firefox queue more decoded frames (YouTube 4K60 AV1 drops went from about 8 % to 1.2–1.5 % after
start-up on an Orange Pi 5B).

## KI-15: AV1 in Chrome/Chromium with other MPP builds — the newest picture waits for the next (last frame, WebRTC) (v3.5.0)

**Status:** fixed with Plaid Pi's MPP · open with other MPP builds, same as 3.0.0

The driver learns a picture's `refresh_frame_flags` from the next picture, so it holds the newest picture until its
successor arrives, unless the client waits on it (vaSyncSurface, or an export after decoding), which forces an early
decode. FFmpeg, mpv and Firefox wait on every picture. Chromium waits on a surface only when it imports it.

With librockchip-mpp1 `+pd32` or later on RK3588 (the deferred refresh of KI-14), 3.5.0 decodes every shown picture of
such a client at once (refresh 0, slots committed later: one MPP job per picture), so nothing is held and the last
frame of a stream comes out right. With any other MPP, decoding every picture early would mean decoding it twice, so
there the newest picture is still held: the last frame of a non-looping AV1 video in Chromium shows the surface's old
content, and low-delay playback (WebRTC, WebCodecs) may show stale frames. 3.0.0 behaves the same way.

**Workaround:** Plaid Pi's MPP (`+pd32` or later) on RK3588 — published for Ubuntu 24.04 and 26.04 at
https://github.com/defcom5-rockchip/mpp/releases/tag/pd34.

## KI-16: AV1 in Chrome/Chromium — a rare old frame mid-stream when the browser runs out of decoded pictures (v3.5.0)

**Status:** open · same as 3.0.0 · start-up flicker fixed in 3.5.0

Chrome and Chromium never wait for a picture they decoded: they sync each surface once, when they import it, and
composite its dma-buf whenever the picture is due; vaapi_wrapper.cc even notes that vaEndPicture "should not be
blocking ... evidence points to it actually waiting". MPP has no dma-buf fence, so a picture composited before MPP has
written it shows the surface's previous content (or green on a surface's first use).

- **Playback start and quality switches — fixed in 3.5.0.** A new decoder (start, every YouTube quality step) decodes
  its first 25 shown pictures before vaEndPicture returns (`RKVA_PACE_START`). 3.0.0 and the 3.5.0 release candidates
  flickered there. Measured on an Orange Pi 5B with Chromium 154 and MPP `+pd32`: first plays of the Big Buck Bunny
  4K60 AV1 film with a scanout checker (every vblank of the composited screen): glitches in the first 6 s in 6 of 10
  launches with the earlier candidate, none seen in 10 with 3.5.0; a Media Source Extensions 1080p→4K start/switch
  frame checker: 0 bad frames in every start and switch window. Not measured with other MPP builds (there the start
  window re-decodes instead).
- **Still open: a rare old frame mid-stream.** When Chromium's renderer runs dry — a heavy 4K KEY frame (100-155 ms in
  the decoder), a hard stretch of the video — it shows each picture as soon as it has it, sometimes before the decode
  has landed: one old frame for one or two screen refreshes, occasionally in 4K60 tests (the 3.5.0 candidates alike;
  3.0.0 showed more). 3.5.0 also closes one driver-side way for this to happen (a frame finishing after its surface was
  re-targeted), so it may now be rarer. Waiting for every picture (`RKVA_PACE_START=all`) closes it but drops 17-22 % of 4K60
  frames. Next version: a dma-buf fence on each surface (Rockchip's Chromium patches report the Mali blob's implicit
  sync as unreliable, so this needs testing), or choosing MPP's reference slots in the driver so no picture is ever
  held. Firefox, mpv and FFmpeg wait on their pictures and are not affected.

---

## KI-17: H.264 streams with custom scaling matrices decode wrong (all High-profile depths) — FIXED in v4.0.0

**Status:** fixed in v4.0.0 (development tree, 2026-10-25) · **Severity:** was: wrong picture · measured 2026-10-23, re-measured 2026-10-25

`h264.c` rebuilt the SPS/PPS it sends to MPP with `pic_scaling_matrix_present_flag = 0` and never read
`VAIQMatrixBufferH264`, so the hardware dequantised every High-profile stream with flat matrices: streams that carry custom
matrices (x264 `--cqm`, many Blu-ray and broadcast encodes, the JVT FRExt vectors) came out slightly wrong at every bit depth.

Fix: the client's scaling lists (VA-API hands them over in raster order; ffmpeg resolves the SPS/PPS fall-back rules before
it fills the buffer) are written into the PPS as explicit `pic_scaling_list` entries in zig-zag order whenever they are not
flat. With it, the PPS is now rebuilt for every picture and sent whenever it differs from the one MPP has, under the
`pic_parameter_set_id` the slices name: streams that switch PPS ids or change a PPS between pictures (weighted prediction,
QP offsets, CABAC/CAVLC) are right too. A PPS with eight explicit lists of extreme values is about 440 bytes, and the writer
reserves twice that for escaping; the first version of the fix silently dropped such a PPS because its buffers were 512
bytes (FRExt1/FRExt3_Panasonic), so the buffers are 2 KB.

Measured on the JVT suites through `ffmpeg -hwaccel vaapi` (Fluster, 2026-10-25, `evidence/2026-10-25-h264-fluster/`): JVT-FR-EXT 27/69 → 67/69, JVT-AVC_V1
74/135 → 111/135 together with KI-20 and the slice fix of KI-21, no vector regressed. Still failing in FR-EXT: FREH10-1/2 (High 10
initial QP: ffmpeg's `pic_init_qp_minus26` includes the bit-depth offset that the bitstream value must not; GStreamer and
Chromium send the coded value, so the driver cannot subtract it blindly).

Found on the way: **monochrome (4:0:0) High streams crashed ffmpeg** (HPCAMOLQ/HPCVMOLQ_BRCM_B; HPCVMOLQ crashed with 3.5.0 too).
MPP returns a luma-only frame for them and the driver copied a chroma plane the frame does not have, past the end of the
buffer. The copy now fills the surface's chroma plane with grey; both vectors decode bit-exact.

The 3.5.0 entry under this number said that Firefox decodes High 10 in software and that the hardware output differed from
software; the second part was this bug. Firefox's software decode of High 10 is Firefox's choice, not the driver's.

## KI-20: interlaced H.264 (field pictures, PAFF, MBAFF) was mostly refused — FIXED in v4.0.0, output is weaved

**Status:** fixed in v4.0.0 (development tree, 2026-10-25) · **Severity:** was: no hardware decode for interlaced streams · re-measured 2026-10-25

Three things were wrong, none of them in the hardware:

- **Two Begin/EndPicture rounds for one frame.** A VA-API client decodes the two fields of a frame into the same surface
  with two pictures. The driver treated the second as a new picture: it bumped the surface's generation (the packets carry
  it in the PTS, and MPP tags the pair's output with the second field's PTS, so the output looked stale and was dropped) and
  queued the surface twice. The second field of a pair now keeps the surface state and the queue entry of the first.
- **MPP deinterlaced on its own.** MPP's decoder is created with `base:enable_vproc = MPP_VPROC_MODE_DEINTELACE`: a field
  pair came back through its IEP deinterlacer, as one or two frames per pair that never equal the decoded picture. The
  driver turns that off; a VA-API client expects the decoded frame, one per picture pair.
- **The option was set but not applied.** The driver built its `MppDecCfg` from `mpp_dec_cfg_init()` instead of the decoder's
  live configuration, and MPP applies only values that differ from what the object holds, so turning a default-on option
  off was a no-op. It reads the live configuration first now (this also means `base:split_parse = 0` was never applied
  before; MPP accepted our per-picture packets either way).

What this means for players: interlaced H.264 (DVB/ATSC recordings, DVD/Blu-ray remuxes) now decodes in hardware and comes
out **weaved**, as every other VA-API driver delivers it; the player has to deinterlace. This driver has no VA-API
video-processing (VPP) entry points, so the `vaapi` deinterlace filters of mpv, ffmpeg and GStreamer are not available.
mpv 0.41 does not deinterlace by default; with `--deinterlace=yes` (or `auto`) it tries `vavpp`, logs that it failed, and
falls back to downloading the frame and running `bwdif` in software, which works at a copy per frame (`hwdec=vaapi-copy`
avoids the detour). Chromium and Chrome never reach the driver with interlaced H.264: their own decoder refuses it and
they decode in software, as before. Firefox decodes it in hardware now and shows it weaved, as it showed the software
decode before.

Measured (Fluster, 2026-10-25): of the 58 JVT-AVC_V1 and FR-EXT vectors whose names mark field, PAFF or MBAFF coding (list in
`evidence/2026-10-25-h264-fluster/`), 52 pass on the hardware, 31 of them for the first time. Still failing:
Sharp_MP_Field_1/2/3 and Sharp_MP_PAFF_1r2/2r (picture order count type 1, whose offsets VA-API does not carry) and
CVFI2_SVA_C (not investigated).

## KI-21: a few H.264 parameter cases differ from a software decode (v3.5.0) — mostly fixed in v4.0.0

**Status:** open for the remaining cases · **Severity:** wrong picture on specific streams · measured 2026-10-23, re-measured 2026-10-25

Fixed by the per-picture PPS of KI-17: explicit weighted prediction (CAWP1/5, CVWP1/5), QP and chroma-QP offsets (CACQP3,
CVCANLMA2), several PPS ids (MPS_MW_A, HCHP1). Fixed separately: **pictures with several CAVLC slices** (BA1_FT_C, BASQP1,
CI1_FT_B, SL1_SVA_B, SVA_Base_B, SVA_CL1_E, SVA_FM1_E, CVFC1_Sony_C, and the CVFI1/CVFI2/CVNLFI1/CVNLFI2 Sony field vectors). ffmpeg hands the driver each slice NAL up to the next start
code, and when that start code is the 4-byte form its first zero stays on the previous NAL; a software decoder ignores the
trailing zero; the hardware parsed CAVLC slices wrongly with it (the twelve vectors above, none of them CABAC). The driver strips trailing
zero bytes from slice data before it sends them.

Still open (JVT-AVC_V1 2026-10-25, 24 failing vectors): picture order count type 1 (BAMQ2, NLMQ2, LS_SVA_D, MR9_BT_B, the
Sharp field vectors; the offsets are not in the VA-API picture parameters, a fix rewrites the slices to type 0), long-term
references with reordering (MR1_BT_A wrong; MR2–MR5_TANDBERG and MR6–MR8_BT refused), CVFI2_SVA_C, and the classes the hardware or VA-API do not
cover: FMO/ASO (FM*, BA3_SVA_C), SP slices (SP1, sp2).

## KI-18: GStreamer needs `GST_VA_ALL_DRIVERS=1` (v3.5.0)

**Status:** external (GStreamer's VA plugin whitelists drivers) · untested as an importing client

GStreamer's `va` plugin only registers decoders for drivers it knows; set `GST_VA_ALL_DRIVERS=1` in the environment to
use this one. The import-ahead behaviour this driver paces for (KI-16) has only been tested with Chromium; GStreamer
pipelines were tried at 1080p60 AV1 only.

---

## KI-19: 8-bit video in Firefox on Arm's proprietary Mali driver needs a helper or a Firefox patch (v3.5.0)

**Status:** external (Mozilla Bug 2078820, patch v3 submitted) · Plaid Pi ships the helper

With Arm's libmali (the usual GPU driver on vendor kernels), Firefox's import of the 8-bit chroma plane (GR88) fails with
`EGL_BAD_MATCH` and Firefox falls back to software for every 8-bit video; 10-bit imports fine. This is Firefox + the GL
driver, not this VA-API driver. Plaid Pi ships a small library on Firefox's library path that redoes that import as R16;
a Firefox patch that does the same is under review (Bug 2078820). With Mesa (Panthor) the problem does not exist. The
Firefox figures in this file were measured with that helper or patch in place.

---

## Reporting

Open an issue with: board model, kernel (`uname -a`), distro, driver version
(`vainfo` prints it), the codec/resolution involved, and `ffprobe` output for a
local file that reproduces it. For browser reports, `about:support` (Firefox) or
`chrome://gpu` (Chrome) plus whether `sudo fuser -v /dev/mpp_service` shows the
decoder process during playback.
