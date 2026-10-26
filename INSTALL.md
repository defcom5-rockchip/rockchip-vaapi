# Installation guide — rockchip-vaapi

## System requirements

### Contiguous memory (CMA) — required for 4K

The RK3588 VPU decodes into **physically contiguous** memory (CMA —
Contiguous Memory Allocator), and the Mali GPU also draws compositing buffers
from the same CMA pool. A single 4K NV12 frame is ~12.5 MB and the VP9 decoder
keeps ~10 reference frames in flight (~125–200 MB), so 4K decode **plus** 4K GPU
compositing together need well over the default `cma=256M`.

With only 256 MB CMA, 4K playback in Firefox decodes correctly for a few
seconds and then dies with `NS_ERROR_DOM_MEDIA_FATAL_ERR` and falls back to
software — the VPU can no longer allocate a contiguous buffer once the GPU has
taken its share. (1080p frames are 4× smaller and never hit the limit.)

**Set `cma=512M`** (or higher) on the kernel command line and reboot:

- Armbian / mainline u-boot: edit `/boot/armbianEnv.txt`, add or change
  `extraargs=cma=512M` (append to any existing `extraargs`).
- extlinux: edit the `append` line in `/boot/extlinux/extlinux.conf`, add `cma=512M`.

Verify after reboot:

```bash
cat /proc/cmdline | tr ' ' '\n' | grep cma     # → cma=512M
grep CmaTotal /proc/meminfo                     # → CmaTotal: 524288 kB
```

> The driver's own per-surface buffers come from MPP's DRM allocator (IOMMU-mapped
> system memory, not CMA), so they are not the constraint; the CMA pressure is MPP's decode DPB plus the
> GPU compositor. See `docs/DEVELOPMENT.md` for details.

## Option A: Install from Debian package (recommended)

```bash
sudo apt install ./rockchip-vaapi_4.0.0-1_arm64.deb
```

**Upgrading from 3.x:** the same command. If an image holds the package (`apt-mark showhold` lists it), add
`--allow-change-held-packages`. Your environment answer (below) carries over; `sudo dpkg-reconfigure rockchip-vaapi`
changes it.

**Which MPP do I need?** Every codec decodes with any `librockchip-mpp1` ≥ 1.5.0 (stock Ubuntu, the Ubuntu-Rockchip
PPAs, Armbian), but the PPA builds lose frames in VP9 sessions past ~65,536 frames (KI-10). Two more things need Plaid Pi's MPP build on RK3588: 4K60 AV1 in Firefox (otherwise each
early-synced AV1 picture is decoded twice and Firefox cannot keep up, KNOWN-ISSUES KI-14) and clean AV1 last frames in
Chrome/Chromium (KI-15). The minimum is `1.5.0+git20260805.a8b19653+ds-0ubuntu1~rk1+pd32` (patches in its source
package); `+pd33` and later also fix the VP9 reset storm (KI-10). `+pd35` (which also applies AV1 film grain) is published for Ubuntu 24.04 and 26.04 at
https://github.com/defcom5-rockchip/mpp/releases/tag/pd35:
`sudo apt install ./librockchip-mpp1_…_arm64.deb` (the release notes give the exact file name for each Ubuntu release
and how to go back). The package's `Recommends` only hints at this; the proof that the patched
MPP is in use is the line "deferred refresh available" in an `RK_VAAPI_LOG` log. GStreamer additionally needs
`GST_VA_ALL_DRIVERS=1` (KI-18).

This installs `rockchip_drv_video.so` to `/usr/lib/aarch64-linux-gnu/dri/` with the
`panthor`/`panfrost` name links (KI-9), and replaces the older `rockchip-vaapi-driver`
package if you had it. If an image holds the package against upgrades
(`apt-mark showhold`), add `--allow-change-held-packages`. To go back to the old package, use apt too
(`sudo apt install ./rockchip-vaapi-driver_<version>_arm64.deb`); plain `dpkg -i` refuses because the two conflict.

The package sets `LIBVA_DRIVER_NAME`, `LIBVA_DRIVERS_PATH`, `MOZ_DISABLE_RDD_SANDBOX=1`, `MOZ_ENABLE_WAYLAND=1` and
`MOZ_X11_EGL=1` system-wide by default (`/etc/environment.d/60-rockchip-vaapi.conf`, `/etc/profile.d/rockchip-vaapi.sh`).
`MOZ_DISABLE_RDD_SANDBOX=1` turns off the sandbox of Firefox's media process for every user. To opt out:
`sudo dpkg-reconfigure rockchip-vaapi` and answer No.
To uninstall: `sudo apt remove rockchip-vaapi`.

## Option B: Build and install from source

### 1. Install build dependencies

```bash
sudo apt install gcc pkg-config libva-dev librockchip-mpp-dev librga-dev
```

### 2. Build

```bash
cd /path/to/rockchip-vaapi
make
```

On a release without a `librga-dev` package (Ubuntu 26.04 "resolute" at the time of writing): unpack the headers from the
librga source, write a `librga.pc` whose `Cflags` points at them and whose `Libs` is `-l:librga.so.2` (or pass
`RGA_CFLAGS=… RGA_LIBS=-l:librga.so.2` to make), and build the package with `dpkg-buildpackage -d`; the 4.0.0 .deb was built
that way, with the same linkage a `librga-dev` build gives.

Expected output: `rockchip_drv_video.so` in the project root. Without `librga-dev` the
build stops with a message; `make RGA=no` builds without the RGA paths (10-bit is then
converted on the CPU).

### 3. Install

```bash
sudo make install
# installs to /usr/lib/aarch64-linux-gnu/dri/
```

To install to a custom path:

```bash
sudo install -m 755 rockchip_drv_video.so /your/path/rockchip_drv_video.so
```

## Option C: Build your own Debian package

```bash
# Create orig tarball (exclude build artifacts and debian/)
tar --exclude='rockchip-vaapi/debian' \
    --exclude='rockchip-vaapi/*.so' \
    --exclude='rockchip-vaapi/src/*.o' \
    -czf rockchip-vaapi_4.0.0.orig.tar.gz rockchip-vaapi/

# Build binary package
cd rockchip-vaapi
dpkg-buildpackage -us -uc -b

# Build source package
dpkg-buildpackage -us -uc -S
```

## Verifying the installation

```bash
# Check the driver file is in place
ls -l /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so

# Verify VA-API sees the driver
LIBVA_DRIVER_NAME=rockchip \
LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
vainfo
```

Expected `vainfo` output:

```
libva info: VA-API version 1.20.0
libva info: User environment variable requested driver 'rockchip'
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so
libva info: Found init function __vaDriverInit_1_20
...
VA profile VAProfileH264Main               : VAEntrypointVLD
VA profile VAProfileH264High               : VAEntrypointVLD
VA profile VAProfileHEVCMain               : VAEntrypointVLD
...
```

## Configuring Firefox

### Environment variables (required)

| Variable | Value | Reason |
|----------|-------|--------|
| `LIBVA_DRIVER_NAME` | `rockchip` | Selects this driver |
| `LIBVA_DRIVERS_PATH` | `/usr/lib/aarch64-linux-gnu/dri` | Driver search path |
| `MOZ_DISABLE_RDD_SANDBOX=1` | `1` | Allows RDD process to open `/dev/dri` |

### about:config (required)

Open `about:config` in Firefox and set:

| Key | Value |
|-----|-------|
| `media.hardware-video-decoding.enabled` | `true` |
| `media.ffmpeg.vaapi.enabled` | `true` |
| `media.rdd-ffmpeg.enabled` | `true` |

### Permanent launcher

```bash
sudo tee /usr/local/bin/firefox-hw > /dev/null <<'EOF'
#!/bin/sh
export LIBVA_DRIVER_NAME=rockchip
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export MOZ_DISABLE_RDD_SANDBOX=1
exec /usr/bin/firefox "$@"
EOF
sudo chmod +x /usr/local/bin/firefox-hw
```

## Troubleshooting

**`vainfo` reports "driver not found"**
Verify `LIBVA_DRIVER_NAME=rockchip` and that the `.so` exists at
`/usr/lib/aarch64-linux-gnu/dri/rockchip_drv_video.so`.

**Firefox still decodes in software**
While a video plays, `sudo fuser -v /dev/mpp_service` must list Firefox's RDD process; or start Firefox with
`RK_VAAPI_LOG=/tmp/rk-vaapi.log` and look for `mpp_init OK` and `ExportSurfaceHandle` in that file. If neither shows,
verify the three `about:config` keys and that `MOZ_DISABLE_RDD_SANDBOX=1` is set.

**`/dev/dri` permission denied in RDD process**
Add your user to the `video` and `render` groups:
```bash
sudo usermod -aG video,render $USER
```
Then log out and back in (or use `newgrp video`).

**No frames decoded / black screen**
Enable verbose logging by setting `RK_VAAPI_LOG` to a file path:
```bash
LIBVA_DRIVER_NAME=rockchip RK_VAAPI_LOG=/tmp/rk.log MOZ_DISABLE_RDD_SANDBOX=1 firefox
tail -f /tmp/rk.log | grep -E "copied|TIMEOUT|ERROR|failed"
```
Look for errors after `BeginPicture` or `EndPicture`. Missing SPS/PPS or
MPP decode errors will appear there. Without `RK_VAAPI_LOG`, the driver
produces no output (logging is disabled by default for performance).

**4K plays for a few seconds then falls back to software
(`NS_ERROR_DOM_MEDIA_FATAL_ERR`)**
This is CMA exhaustion. The driver log will show frames decoded normally
(`copied=1`, no errors) for ~75 frames then a fatal error with no driver-side
cause — MPP cannot report a failed contiguous allocation through libva.
Increase CMA to `cma=512M` (see *System requirements* above).

Note: on some RK3588 boards the DTB hardcodes the CMA region with a fixed
physical address (`reg` property), which overrides any `cma=` command-line
parameter. If `grep CmaTotal /proc/meminfo` still shows 262144 kB after adding
`cma=512M`, patch the DTB directly:

```bash
sudo cp /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb.bak
sudo fdtput -t x /boot/dtb/rockchip/rk3588-orangepi-5-plus.dtb \
        /reserved-memory/cma reg 0x00 0x10000000 0x00 0x20000000
```

Then reboot and verify `grep CmaTotal /proc/meminfo` → `524288 kB`.

## Panthor / Panfrost GPU stacks

libva derives the VA driver name from the render node's kernel driver. On images whose GPU runs on the mainline `panthor` or `panfrost` driver (Armbian vendor kernels with the Panthor backport, kisak Mesa), that name is not `rockchip`. From v2.1.5 the package ships `panthor_drv_video.so` and `panfrost_drv_video.so` symlinks so auto-detection works; on older versions export `LIBVA_DRIVER_NAME=rockchip` (e.g. in `/etc/environment`).

## Checking an export before decode

`cc tests/va-export-before-decode.c -o va-export-before-decode -lva -lva-drm && LIBVA_DRIVER_NAME=rockchip ./va-export-before-decode` — a 10-bit surface must export as `P010` with layers `R16` + `GR32` (the fourcc of DRM_FORMAT_GR1616), pitch
2 × width, before any frame is decoded (KI-8).
