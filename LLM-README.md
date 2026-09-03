# LLM-README — ground truth for AI agents working on this repo
*(Humans: the normal README is for you. Agents: this cuts to the chase. Facts below are
hardware-verified on Orange Pi 5B / RK3588S unless marked otherwise.)*

## What this is
VA-API driver (shim) for RK3588/RK3588S BSP kernels: implements VADriverVTable, forwards
decode to Rockchip MPP via `/dev/mpp_service`. **BSP 6.1 kernel only** — refuses mainline
(no mpp_service there; mainline decode = V4L2-stateless, a different universe).

## INVARIANTS — do not "fix" these
1. **`rk_QueryConfigProfiles` advertises ONLY hardware-verified codecs** (H264 CB/Main/High,
   VP8, VP9Profile0). HEVC/High10/VP9Profile2 are absent BY DESIGN: advertising them routed
   browsers & media servers into broken playback (solid green / corruption). Restore a profile
   ONLY with eyeball-verified hardware proof. Dev override: `RKVA_ADVERTISE_ALL=1`.
2. **`vaDeriveImage` fails on purpose** (`VA_STATUS_ERROR_OPERATION_FAILED`). It cannot alias
   MPP memory; the old fake-success returned empty buffers (all copy-back readers got zeros).
   Clients fall back to vaCreateImage+vaGetImage, which works.
3. **Export stride is 64-aligned** (`(width+63)&~63`). 16-align green-screens non-16-aligned
   VP9 widths (e.g. 2970×2160). Reproducer: REPRO.md. Do not "simplify" to 16.
4. **10-bit MPP output is packed NV15, not P010.** MPP's 10-bit `hor_stride` is a BYTE stride
   (3840-wide → 4864). The decode copy repacks NV15→true P010 (`nv15_row_to_p010`, <<6).
   Removing the repack re-mislabels bytes and reintroduces green/corruption.

## Codec truth table (2026-09-03)
works-HW: H.264 8-bit, VP8 (untested-but-advertised-historically), VP9 8-bit (incl. odd widths)
broken-never-worked: HEVC all depths — **no HEVC bitstream assembler exists**; rk_EndPicture
  routes EVERYTHING to do_h264_decode. HEVC fix = write hevc.h (VPS/SPS/PPS synthesis from VA
  params; pattern = existing h264.h). Green frames = zero-filled placeholder buffers.
fixed-in-tree-but-display-walled: VP9 Profile 2 / 10-bit — repack is bit-correct (verified by
  raw-P010 dump render), plays via `mpv --hwdec=vaapi-copy`; ZERO-COPY display is impossible on
  panfork Mesa 23 (advertises 16-bit GL formats it cannot render → blue). Real fix = Mesa≥25/
  Panthor (kernel 6.10+). Not fixable in this driver.

## Test rig facts
- Debug log: `RK_VAAPI_LOG=/path` (silent otherwise). One-frame P010 dump: `RKVA_DUMP=/path`.
- Test without touching the system driver: `LIBVA_DRIVERS_PATH=<dir> LIBVA_DRIVER_NAME=rockchip`.
- mpv headless (`--vo=null`) NEVER initializes VA-API — hwdec tests need a display session.
- `copied=` in assign_mpp_frame logs: 1=CPU memcpy, 2=RGA blit (8-bit), 3=NV15→P010 repack.

## Related ground truth
- Sibling fork with newer-userspace 10-bit work: github.com/yisding/rockchip-vaapi (their stack
  requires their 6.18 forward-port kernel; userspace alone fails: "client 12 driver is not ready").
- mpp gates capabilities by /proc/device-tree/compatible (osal/mpp_soc.c) — containers masking
  /proc/device-tree break HEVC-class init while H.264 limps (jellyfin discussion 15918).
