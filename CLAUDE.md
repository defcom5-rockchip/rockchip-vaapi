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

## Codec state (2026-09-04, all hardware-verified on RK3588S)
Works in hardware, pixel-identical to software decode:
- H.264 8-bit, **including B-frame streams** (High profile, 720p60 broadcast-style)
- HEVC Main 8-bit and **Main10 10-bit**, including B-frames — a full 12-minute
  4096×1714 Main10 feature plays through on `vaapi-copy`, all 17,616 frames, no
  put failures
- VP9 Profile 0 (incl. non-16-aligned widths) and Profile 2 via copy-back

Not available, and why:
- **AV1** — not implemented; VA-API supplies headerless tile data, MPP needs full OBU.
- **Zero-copy 10-bit display** — impossible on panfork Mesa 23: it advertises
  16-bit GL formats it cannot render, so any 10-bit surface reaching GL shows
  as a solid blue field. Copy-back (`--hwdec=vaapi-copy`) is correct and is what
  the shipped mpv config uses. Clears at Mesa ≥25 / Panthor, not in this driver.
- **Some HEVC streams** index SPS-level RPS sets or use inter-RPS prediction;
  those tables are not in the VA-API struct. Detected and logged, not guessed.

Client notes:
- **Firefox** hardware-decodes H.264, VP9 **and HEVC** — HEVC needs the
  `media.hevc.enabled` pref, which defaults off on Linux. A `strings` grep for
  `VAProfileHEVC` in libxul proves nothing: Firefox references VA profiles as
  enums, and `VAProfileH264` is equally absent while H.264 demonstrably works.
- **Chrome** hardware-decodes HEVC only with `--enable-features=PlatformHEVCDecoderSupport`,
  and needs GPU compositing on — which reintroduces the ANGLE UI flicker on this
  GPU stack. Firefox has no such trade-off.
- **VLC 3.x** loads this driver but still software-decodes: its VA-API interop is
  X11-era and runs under XWayland here. Not a driver defect; nothing to fix here.
- **mpv** is the reference client. `--hwdec=vaapi-copy` is the correct mode;
  plain `hwdec=auto` makes mpv pick its own rkmpp path and bypass this driver.

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
