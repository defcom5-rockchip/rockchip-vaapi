# Reproducer: green frame on unaligned-width VP9 (RK3588 VA-API)

## Symptom
A valid VP9 stream whose **coded width is not a multiple of 16** hardware-decodes to a
**solid green frame** through this VA-API driver, while the *same* stream decodes correctly
via MPP directly and via software. Audio and UI are unaffected — only the video surface is green.

## Public reproducer
- **Video:** Simple Minds — *Don't You (Forget About Me)* (official) — https://youtu.be/CdqoNKCCt7A
- **Resolution / rate that greens:** **2970×2160 @ 25 fps** (VP9). This video's whole quality
  ladder is **1.375:1 Academy aspect → non-16-aligned widths** (2970, 1980, 1486, 990 …), so any
  of its VP9 tiers reproduce it; 2970×2160@25 is the confirmed 4K case.
- **Settings:** Firefox with VA-API hardware decode enabled (the RDD/media process holds
  `/dev/mpp_service`); YouTube serving **VP9** (not AV1). Confirm in right-click →
  **Stats for nerds**: `Codecs: vp09…`, `Current Res / Optimal Res: 2970×2160@25`.
- **Result:** solid green video.

## Intrinsic stream (so the repro survives the video being removed)
```
yt-dlp -f 313 https://youtu.be/CdqoNKCCt7A          # the 2970x2160 VP9 rendition
ffprobe -select_streams v:0 -show_entries \
  stream=codec_name,profile,width,height,pix_fmt,color_primaries,r_frame_rate <file>
#  codec_name=vp9  profile=Profile 0  width=2970  height=2160
#  pix_fmt=yuv420p  color_primaries=bt709  r_frame_rate=25/1
#  (2970 mod 16 = 10  -> unaligned coded width)
```

## Controls (prove it is the VA-API driver, not the file or the hardware)
- **GStreamer `mppvideodec` (MPP core, bypasses VA-API):** decodes 2970×2160@25 to a **perfect
  picture**. → the VPU/MPP hardware is fine.
- **AV1 rendition of the same title (software-decoded):** **perfect picture**. → the file is fine.
- **VA-API path (Firefox / this driver):** **green**. → the bug is in the VA-API surface handling.

## Root cause
`rk_CreateSurfaces()` allocated the exported placeholder surface at a **16-aligned** stride
(`(width+15)&~15` → 2976 for a 2970 width). PR #2 (`dad91a8`) already re-strides the plane copy
into the exported layout, but 16-alignment is insufficient: the RK3588 Mali/EGL DMABUF importer
expects a **64-aligned** NV12 pitch, so a 16-aligned-but-not-64-aligned stride still places the
chroma plane at the wrong offset → green. (2976 is not 64-aligned; 3008 is.)

## Fix
Align the exported surface stride to **64 bytes** (`(width+63)&~63`), matching the canonical
Rockchip pattern — cf. mainline RKVDEC2 fix `eb20dfb` ("use aligned `bytesperline`, align to 64").
Combined with PR #2's re-stride, unaligned-width VP9 now decodes correctly.

## Hardware verification
RK3588S (Orange Pi 5B), Firefox 154 VA-API, panfork Mesa 23, BSP kernel 6.1.0-1027:
the exact **2970×2160@25** stream that green-screened now decodes to a clean picture, with the
Firefox **RDD process holding `/dev/mpp_service` at ~9% CPU** (i.e. genuine hardware decode, not a
software fallback).
