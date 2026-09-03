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
1. `rk_QueryConfigProfiles` lists ONLY verified codecs (H264 CB/Main/High, VP8,
   VP9Profile0). HEVC/High10/VP9Profile2 are absent BY DESIGN — advertising
   them routed browsers and media servers into green/corrupt playback. Dev
   override for testing: `RKVA_ADVERTISE_ALL=1`.
2. `vaDeriveImage` fails on purpose. It cannot alias MPP memory; the old fake
   success handed out empty buffers (all copy-back readers got zeros). Clients
   fall back to vaCreateImage + vaGetImage, which works.
3. Export stride is 64-aligned (`(width+63)&~63`). 16-align green-screens
   non-16-aligned VP9 widths (2970×2160 reproducer in REPRO.md).
4. 10-bit MPP output is packed NV15 with a BYTE hor_stride (3840w → 4864); the
   decode copy repacks it to true P010 (`nv15_row_to_p010`, `<<6`). Removing
   the repack re-mislabels bytes.

## Codec state (2026-09-03)
- Works in hardware: H.264 8-bit; VP9 8-bit incl. odd widths.
- HEVC (all depths): decode never runs — there is NO HEVC bitstream assembler;
  rk_EndPicture routes everything to do_h264_decode. Green = zero-filled
  placeholders. The fix is writing hevc.h (VPS/SPS/PPS synthesis from VA
  params; pattern = the in-tree h264.h).
- VP9 Profile 2 / 10-bit: repack is bit-correct (raw-P010 dump verified);
  plays via `mpv --hwdec=vaapi-copy`. Zero-copy display is IMPOSSIBLE on
  panfork Mesa 23 (it advertises 16-bit GL formats it cannot render → solid
  blue). That wall falls at Mesa ≥25 / Panthor, not in this driver.

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
