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

This driver advertises only what has been verified to decode correctly on
hardware. Anything not listed here is deliberately **not** offered to clients,
so players and media servers fall back to paths that work (software decode,
server-side transcode) instead of rendering a broken picture.

| Codec | Profile | Status |
|-------|---------|--------|
| H.264 | Constrained Baseline, Main, High | ✅ hardware decode (see KI-1 for a B-frame edge case) |
| VP8   | Version 0–3 | ✅ advertised |
| VP9   | Profile 0 (8-bit) | ✅ hardware decode, including non-16-aligned widths |
| H.264 | High10 | ✅ hardware decode + zero-copy display, advertised since v2.1.3 |
| HEVC  | Main (8-bit) | ✅ hardware decode, advertised (v2.1.0+), bit-exact vs software |
| HEVC  | Main10 | ✅ hardware decode + zero-copy display, advertised since v2.1.3 (mpv and Firefox verified; see KI-3 for the history) |
| VP9   | Profile 2 (10-bit) | ✅ hardware decode + zero-copy display, advertised since v2.1.3 |
| AV1   | any | ❌ not implemented (VA-API supplies headerless tile data; MPP needs full OBU) |

> **Panthor / Panfrost GPU stacks (mainline Mesa):** libva auto-detects the driver from v2.1.5 (shipped `panthor_drv_video.so` / `panfrost_drv_video.so` symlinks); on older versions set `LIBVA_DRIVER_NAME=rockchip`. See KI-9.
>
> **Menu switch:** `RKVA_HIDE_10BIT=1` hides the three 10-bit profiles again if a client cannot present P010.
>
> **Jellyfin web (measured 2026-09-07 on Pi Desktop):** Jellyfin decides direct play by `canPlayType`. Firefox with this driver answers *probably* for HEVC Main10 (`hvc1.2.4.L153`), VP9 Profile 2 and Matroska, and MediaCapabilities reports 4K Main10 as supported, smooth and power-efficient — so Jellyfin **direct-plays 10-bit in hardware in Firefox**. The image's Chromium answers *no* to Main10/VP9 P2 (and yes to 8-bit HEVC in hardware), so Jellyfin transcodes 10-bit there instead of crashing.
>
> **Chromium note:** the `+rkmpp` Chromium from the liujianfeng1994 PPA (shipped on Pi Desktop) decodes through libv4l-rkmpp, not through this driver; see KI-6. Firefox and mpv use this driver.

Earlier releases advertised HEVC, High10 and VP9 Profile 2. They never decoded
correctly — see [KNOWN-ISSUES](KNOWN-ISSUES.md) for the full story and progress.

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

## License and attribution

**LGPL-2.1-or-later** — see [LICENSE](LICENSE). Per-file copyright is recorded in
[debian/copyright](debian/copyright); every source file carries an SPDX identifier.

This is a fork, and the lineage matters:

- **Eduardo García-Mádico Portabella** (`woodyst`) wrote the original driver —
  the VA-API vtable, the MPP plumbing, the H.264 parameter-set reconstruction and
  the bit writer this fork still builds on. Upstream: https://github.com/woodyst/rockchip-vaapi
- **truongsinh** contributed the surface-stride fixes carried here from upstream PR #2.
- **defcom5-rockchip** maintains this fork: the HEVC bitstream assembler (`src/hevc.c`,
  original work — upstream has no HEVC), the NV15 to P010 repack with its NEON path,
  B-frame and reference-routing fixes, `vaDeriveImage`, the honest profile
  advertisement, and the test tooling. Developed with Claude as co-engineer;
  contributions are attributed in the commit trailers.

Rockchip MPP, libva and the other libraries this links against carry their own
licenses and are not redistributed here.

## Contributors

| who | what |
|---|---|
| **Eduardo García-Mádico Portabella** (`woodyst`) | original driver: VA-API ↔ MPP bridge, H.264 path, the bit writer this fork builds on |
| **TruongSinh Tran-Nguyen** (`truongsinh`) | surface-stride fixes (upstream PR #2) |
| **defcom5-rockchip** | fork maintainer: 10-bit zero-copy (GR1616 fix), Main10/High10/VP9 P2, Chrome created-depth export, Panthor/Panfrost libva names, the RGA3 NV15→P010 export lane, the KI test ladder, releases — and the RK3588 kernel-side fixes that surfaced along the way (Armbian `linux-rockchip` PR #548) |
| **Claude** (Anthropic) — Fable 5.1, Fable 5, Opus 5, Sonnet 4.6 | co-engineer on the fork, credited per commit in the `Co-Authored-By` trailers: root-cause work, patches, test tooling and the documentation |
| **JFL** (Armbian forum) | test partner on the Orange Pi 5 Plus / Armbian vendor kernel: confirmed Chrome 10-bit HEVC and VP9 Profile 2 hardware playback with 2.1.5, measured the 2.2.0-rc1 RGA lane (97 → 1 dropped frames at 4K60 HDR), and reported the Panthor libva naming that became the shipped symlinks |
| **nyanmisaka** (`ffmpeg-rockchip`, `jellyfin-ffmpeg`) | the RGA3 P010 recipe (`RK_FORMAT_YCbCr_420_SP_10B` + the 10-bit flags) the export lane follows, and the Rockchip lanes in Jellyfin's FFmpeg |

## RGA export lane (10-bit)

Since 2.2.0 the per-frame copy into the exported surface uses the RK3588's
RGA3 2D engine for 10-bit streams too: MPP's packed NV15 is converted to
true P010 in hardware (same `RK_FORMAT_YCbCr_420_SP_10B` on both sides,
`is_10b_compact = is_10b_endian = 1` on the P010 side, byte-pitch strides),
verified bit-exact against the previous CPU repack on 4K HDR10 content.
At 3840x2160 the blit takes about 3.8 ms on one RGA3 core and removes about
5 s of CPU time per 20 s of 4K60 playback from the decoding process.
`RKVA_RGA_P010=0` forces the CPU repack for 10-bit; `RKVA_RGA_COPY=0` turns every RGA copy off (8-bit and 10-bit) for A/B tests against the CPU paths. The 8-bit NV12 copy has used RGA
(`imcopy`) whenever the driver is built with `librga-dev` present; 2.2.0 is
the first release built that way, so `librga2` is now a runtime dependency.

