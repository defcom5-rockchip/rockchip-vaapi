# rockchip-vaapi

VA-API driver for Rockchip RK3588 / RK3576 that bridges **libva** to
**librockchip-mpp** (MPP), enabling hardware-accelerated video decode in
applications such as Firefox.

**Author:** Eduardo García-Mádico Portabella — EGP Sistemas
**Contact:** woodyst@gmail.com
**License:** LGPL-2.1-or-later

---

## What it does

The Rockchip RK3588 SoC includes a dedicated VPU capable of decoding H.264,
HEVC, VP9 and AV1 at up to 8K resolution. However, no vendor-supplied VA-API
driver exists for it. This project fills that gap by implementing the complete
`VADriverVTable` (VA-API 1.20) and forwarding decode work to the Rockchip MPP
library, which in turn uses the hardware VPU.

Key features:

- H.264 / HEVC / VP9 / AV1 hardware decode
- Zero-copy DRM PRIME 2 surface export (NV12, DMABUF)
- Compatible with Firefox 128+ (VA-API PDM path, RDD process)
- Implements the full VA-API 1.20 vtable (`__vaDriverInit_1_20`)

## Supported hardware

| SoC | Board (tested) |
|-----|---------------|
| RK3588 | Orange Pi 5 Plus |
| RK3588S | Orange Pi 5B (tested); Orange Pi 5 / Rock 5B (should work) |
| RK3576 | Likely compatible (untested) |

## Supported codecs

| Codec | Profile | Max resolution |
|-------|---------|---------------|
| H.264 | Constrained Baseline, Main, High, High10 | 4K |
| HEVC | Main, Main10 | 8K |
| VP9 | Profile 0, 2 | 8K |
| AV1 | Profile 0, 1 | 8K |

## Dependencies

Runtime:
- `libva2` (>= 2.0)
- `librockchip-mpp1`

Build:
- `libva-dev`
- `librockchip-mpp-dev`
- `pkg-config`, `gcc`

## Quick start

```bash
# Build and install
make
sudo make install

# Launch Firefox with hardware decode
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
MOZ_DISABLE_RDD_SANDBOX=1 \
firefox
```

In Firefox, also enable via `about:config`:

| Preference | Value |
|-----------|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

## Verifying hardware decode

After starting Firefox and playing a video, check the driver log:

```bash
# Should show mpp_create OK, BeginPicture, EndPicture, ExportSurfaceHandle
LIBVA_DRIVER_NAME=rockchip MOZ_DISABLE_RDD_SANDBOX=1 firefox 2>&1 | grep rk-vaapi
```

You can also check VPU activity:

```bash
cat /sys/class/devfreq/*/cur_freq   # VPU frequency rises under load
```

## Permanent Firefox launcher

Create `/usr/local/bin/firefox-hw`:

```bash
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
```

```bash
chmod +x /usr/local/bin/firefox-hw
```

## Development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for architecture, VA-API
internals, MPP integration details, and how to add support for new codecs.

## AI-assisted development

This driver was designed and implemented with the assistance of
**Claude Sonnet 4.6** (model ID: `claude-sonnet-4-6`), an AI model developed
by Anthropic. Total interactive development time: approximately **3–4 hours**
across two sessions (24 April 2026).

The AI assisted with: architecture design, VA-API vtable implementation,
H.264 Annex B SPS/PPS reconstruction via Exp-Golomb encoding, MPP API
integration, DMABUF/DRM PRIME 2 surface export, and iterative debugging of
Firefox integration issues.

All code was reviewed, tested, and validated on real hardware by
Eduardo García-Mádico Portabella — EGP Sistemas.

### Fork additions — defcom5-rockchip

This fork (`defcom5-rockchip/rockchip-vaapi`) builds on the base driver above, adding:

- **truongsinh's PR #2** (chroma-plane re-stride into the exported layout, optional RGA
  blit offload, RTFormat / surface-attribute reporting) — previously unmerged upstream.
- **VP9 unaligned-width fix** — 64-align the exported surface stride so VP9 streams whose
  coded width is not a multiple of 16 (e.g. 2970×2160@25, 1.375:1 Academy-ratio content)
  no longer render as a solid green frame. See [REPRO.md](REPRO.md).

These fork additions were developed with the assistance of **Claude (Fable 5)** and were
reviewed, tested, and validated on real hardware — **Orange Pi 5B (RK3588S)**, panfork
Mesa 23, Firefox 154 VA-API — by **defcom5-rockchip**. (The base-driver validation credit
above belongs to Eduardo García-Mádico Portabella; it does not cover these fork additions.)
