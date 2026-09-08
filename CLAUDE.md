# Agent instructions

Working contract for agents in this repository (a VA-API driver shim for
RK3588/RK3588S BSP kernels; decode via Rockchip MPP through /dev/mpp_service).
Ground truth below is hardware-verified on an Orange Pi 5B unless marked.

## The prime rule: hardware verification gates everything
No codec profile is advertised, no fix is merged to `main`, and no release is
cut on the strength of code reading alone. The bar is an eyeball-verified run
on real RK3588 hardware, with the driver log (`RK_VAAPI_LOG=/path`) as receipt.
Releases are triggered by the human maintainer, never autonomously.

## Invariants — do not "fix" these
1. `rk_QueryConfigProfiles` lists ONLY verified codecs. As of v2.1 that is
   H264 CB/Main/High, VP8, VP9Profile0 and **HEVC Main**. HEVC **Main10**,
   H264High10 and VP9Profile2 are absent BY DESIGN — they decode correctly but
   the panfork GL stack cannot present 10-bit surfaces, so advertising them
   makes Chrome direct-play into a green screen. Copy-back clients opt in per
   process with `RKVA_ADVERTISE_ALL=1`. Restoring a profile is the maintainer's
   call after hardware soak, never an agent's.
2. Export stride is 64-aligned (`(width+63)&~63`). 16-align green-screens
   non-16-aligned VP9 widths (2970×2160 reproducer in REPRO.md).
3. 10-bit MPP output is packed NV15 with a BYTE hor_stride (3840w → 4864); the
   decode copy repacks it to true P010 (`nv15_row_to_p010`, NEON path plus
   scalar tail — `tests/nv15-unpack-test.c` proves them equivalent for every
   width 1..4224). Removing the repack re-mislabels bytes.
4. **Never state a constraint the decoder cannot rely on, and never re-state a
   parameter set with different content.** Two bugs came from breaking this:
   re-emitting a corrected SPS mid-stream flushes the DPB and destroys the
   references B-frames need; and omitting the H.264 VUI leaves
   `max_num_reorder_frames` at its worst-case default, so the decoder holds
   pictures a stateless client is already blocking on. Both are fixed; keep
   them fixed.
5. `vaDeriveImage` aliases the decoded MPP buffer and its VAImage buffer is
   flagged `borrowed` — `vaDestroyBuffer` must not free it.

- **Never write a DRM fourcc as a hex literal, and never blame the GPU stack without Mesa's own error text.** KI-3 lived for three releases because `DRM_FORMAT_GR1616` was typed as `0x36315247` ("GR16", not a format) and nobody ran the failing client with `EGL_LOG_LEVEL=debug`, which names the rejected check in one line. Fourccs go through `RK_FOURCC()`; a "blocked upstream" claim needs the exact refusing function quoted.
- **A probe that passes is not a frame that displays.** The pre-decode placeholder exports as 8-bit, so mpv's VA-API format probe cannot exercise the 10-bit branch. Prove a path with real decoded frames (import-failure count = 0 in the same run that shows `hwdec-current`).

- **Read the inherited driver's own limitations list before debugging its symptoms.**
  Upstream's `docs/DEVELOPMENT.md` (April 2026) listed the hardcoded PPS `num_ref_idx`
  default as a known limitation; the fork rediscovered it as KI-1 months later at a cost of
  days. When a symptom appears, grep the upstream docs for the mechanism first.

- **A surface's description must be right from the moment it is created, not from its
  first decode.** Chrome exports a surface before decoding into it; deciding the bit depth
  from the last decoded frame described every 10-bit surface as 8-bit (KI-8, three releases).
  `rt_10bit` + `surf_is_10bit()` are the rule; `tests/va-export-before-decode.c` is the check —
  run it against any change to CreateSurfaces / ExportSurfaceHandle / DeriveImage.
- **libva names the driver after the render node's kernel driver.** On Panthor/Panfrost
  stacks that is not "rockchip"; the deb ships symlinks (KI-9). Keep them when repackaging.

## Codec state (2026-09-06, all hardware-verified on RK3588S)
Works in hardware, pixel-identical to software decode:
- H.264 8-bit, **including B-frame streams** (High profile, 720p60 broadcast-style)
- HEVC Main 8-bit and **Main10 10-bit**, including B-frames — a full 12-minute
  4096×1714 Main10 feature plays through on `vaapi-copy`, all 17,616 frames, no
  put failures; zero-copy `hwdec=vaapi` verified on 4K Main10 and VP9 P2 (v2.1.3)
- Menu: all of the above advertised by default since v2.1.3; `RKVA_HIDE_10BIT=1`
  hides the 10-bit trio
- VP9 Profile 0 (incl. non-16-aligned widths) and Profile 2 (copy-back and zero-copy)

Not available, and why:
- **AV1** — not implemented; VA-API supplies headerless tile data, MPP needs full OBU.
- **Zero-copy 10-bit display** — WORKS since v2.1.3 (mpv `hwdec=vaapi`, Firefox).
  Until then the export named the UV plane with a non-existent fourcc ("GR16") and
  Mesa refused it; that was misdiagnosed as a panfork limitation for three releases.
  KI-3 has the forensics. Copy-back (`vaapi-copy`) remains correct too.
- **Some HEVC streams** index SPS-level RPS sets or use inter-RPS prediction;
  those tables are not in the VA-API struct. Detected and logged, not guessed.

Client notes:
- **Firefox** hardware-decodes H.264, VP9 **and HEVC** — HEVC needs the
  `media.hevc.enabled` pref, which defaults off on Linux. A `strings` grep for
  `VAProfileHEVC` in libxul proves nothing: Firefox references VA profiles as
  enums, and `VAProfileH264` is equally absent while H.264 demonstrably works.
- **Chromium on Pi Desktop** (the `+rkmpp` PPA build) does NOT use this driver at
  all: it decodes through libv4l-rkmpp (V4L2 plug-in → MPP). Verified 2026-09-06 with
  the driver log armed. Its 10-bit path aborts the GPU process (KI-6). Stock VA-API
  Chromium builds elsewhere do use this driver and need
  `--enable-features=PlatformHEVCDecoderSupport` for HEVC.
- **Jellyfin web** gates 10-bit direct play on `canPlayType("hvc1.2.4.L123/L153")` and
  MediaCapabilities. Measured 2026-09-07: Firefox + this driver → *probably* / supported,
  smooth, power-efficient (10-bit direct play in hardware); the image's Chromium → *no*
  (transcode). Its 8-bit probe `hvc1.1.L120` is malformed and fails everywhere; the
  `hvc1.1.0.L120` fallback passes. Re-measure with `~/probe/canplay.html` on the test rig
  before changing anything that touches the profile menu.
- **VLC 3.x** loads this driver but still software-decodes: its VA-API interop is
  X11-era and runs under XWayland here. Not a driver defect; nothing to fix here.
- **mpv** is the reference client. `--hwdec=vaapi` (zero-copy) and `--hwdec=vaapi-copy`
  are both correct since v2.1.3; plain `hwdec=auto` makes mpv pick its own rkmpp path
  and bypass this driver (bare NV15 → blue). Never judge a 10-bit result from a
  `--no-config` run without checking the menu the client saw (`vainfo --display wayland`).

## Test rig
- Never touch the system driver: `LIBVA_DRIVERS_PATH=<dir> LIBVA_DRIVER_NAME=rockchip`.
- Logs are silent unless `RK_VAAPI_LOG=/path`. One-frame P010 dump: `RKVA_DUMP=/path`.
- mpv with `--vo=null` never initializes VA-API; hwdec tests need a display session.
- assign_mpp_frame `copied=`: 1 CPU memcpy · 2 RGA blit (8-bit) · 3 NV15→P010 repack.

## Workflow
- Development on feature branches (current: `deep-ink`); `main` is released truth.
- Commit as defcom5-rockchip; `Co-authored-by: Claude <model> <noreply@anthropic.com>`
  is welcome and standard here.
- Related ground truth: yisding/rockchip-vaapi (10-bit work; requires their 6.18
  forward-port kernel — userspace alone fails with "client 12 driver is not
  ready"). mpp gates capabilities via /proc/device-tree/compatible
  (osal/mpp_soc.c) — masked device-tree in containers breaks HEVC-class init.
