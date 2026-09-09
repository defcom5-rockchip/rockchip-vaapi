# Jellyfin 12 on RK3588 with the Rockchip VA-API driver

How to run Jellyfin 12.0 on an RK3588/RK3588S board (Orange Pi 5/5B/5 Plus and friends) so that the
**server transcodes in hardware** and the **browser clients on the same board direct-play in hardware**
through this driver. Written from a Pi Desktop 2.0.2 bench (Ubuntu 24.04, BSP 6.1 kernel, panfork Mesa),
but the server part is the same on Armbian and plain Ubuntu with a vendor kernel.

Two different pieces of hardware acceleration are involved, and it helps to keep them apart:

| piece | who uses it | what it needs |
|---|---|---|
| **Server transcoding** (`jellyfin-ffmpeg`) | Jellyfin when a client cannot direct-play | MPP decode/encode, RGA scaling, OpenCL tone-mapping — all inside `jellyfin-ffmpeg`; no VA-API involved |
| **Client direct play** (Firefox, Chromium, mpv on the board) | the player in the browser | this driver (`rockchip-vaapi`), libva, and the browser's own decode path |

## 1. Install the server

Jellyfin publishes Ubuntu 24.04 / Debian arm64 packages with an FFmpeg that already carries the Rockchip lanes
(`--enable-rkmpp --enable-rkrga --enable-opencl`). Nothing has to be built.

```bash
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://repo.jellyfin.org/jellyfin_team.gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/jellyfin.gpg
sudo tee /etc/apt/sources.list.d/jellyfin.sources >/dev/null <<'SRC'
Types: deb
URIs: https://repo.jellyfin.org/ubuntu
Suites: noble
Components: main
Architectures: arm64
Signed-By: /etc/apt/keyrings/jellyfin.gpg
SRC
sudo apt update
sudo apt install jellyfin          # pulls jellyfin-server, jellyfin-web, jellyfin-ffmpeg8
```

(On Debian replace `ubuntu`/`noble` with `debian`/your codename. Jellyfin 12 no longer builds for Focal or Bullseye.)

Check what you got:

```bash
dpkg -l | grep jellyfin
/usr/lib/jellyfin-ffmpeg/ffmpeg -hide_banner -decoders | grep rkmpp     # av1 h264 hevc vp8 vp9 mpeg2 …
/usr/lib/jellyfin-ffmpeg/ffmpeg -hide_banner -encoders | grep rkmpp     # h264_rkmpp hevc_rkmpp mjpeg_rkmpp
/usr/lib/jellyfin-ffmpeg/ffmpeg -hide_banner -filters  | grep rkrga     # scale_rkrga vpp_rkrga overlay_rkrga
```

## 2. Let the service reach the hardware

The VPU and the RGA are `video`-group devices, the GPU render node is `render`:

```bash
ls -la /dev/mpp_service /dev/rga /dev/dri/renderD128
sudo usermod -aG video,render jellyfin
sudo systemctl restart jellyfin
```

Without this the dashboard lets you select Rockchip MPP but every transcode fails with a permission error
in `/var/log/jellyfin/ffmpeg-transcode-*.txt`.

## 3. Dashboard settings

Open `http://<board-ip>:8096`, finish the startup wizard, then **Dashboard → Playback → Transcoding**:

- Hardware acceleration: **Rockchip MPP**
- Enable hardware decoding for: H.264, HEVC, VP9, AV1 (and the 10-bit HEVC / VP9 boxes)
- **Enable hardware encoding**
- **Enable tone mapping** (HDR → SDR for clients that cannot show HDR)

Tone mapping on Rockchip runs on the GPU through OpenCL. On Pi Desktop the OpenCL ICD is already there
(`/etc/OpenCL/vendors/mali-arm64.icd` from `libmali-g610-x11`; the desktop's GL stays on Mesa/panfork — the ICD is
the only consumer of the blob). On other distributions install the OpenCL-capable `libmali` for the G610 and make sure
the ICD file points at it.

## 4. What the server does on this hardware (measured)

Run as the `jellyfin` user from its working directory, `jellyfin-ffmpeg` 8.1.2 on an Orange Pi 5B, 3840x2160p60
HEVC Main10 HDR10 source:

| pipeline | result |
|---|---|
| MPP decode → `scale_rkrga` → `h264_rkmpp`, 1080p 8 Mbit/s | 237 fps |
| MPP decode → `scale_rkrga` → `hevc_rkmpp`, 4K 20 Mbit/s | 125 fps |
| MPP decode → `scale_rkrga` → `tonemap_opencl` (bt2390) → `h264_rkmpp`, 1080p SDR | 175 fps (stock g13p0 blob), 189 fps (g24p0) |

Realtime for that source is 60 fps, so a single 4K60 HDR stream transcodes at about three times realtime with
tone mapping, and the CPU stays nearly idle. The chain Jellyfin builds is the same one you can test by hand:

```bash
FF=/usr/lib/jellyfin-ffmpeg/ffmpeg
sudo -u jellyfin sh -c "cd /var/lib/jellyfin && exec $FF -nostdin -hide_banner -v warning -benchmark \
  -init_hw_device rkmpp=rk -init_hw_device opencl=ocl@rk \
  -hwaccel rkmpp -hwaccel_output_format drm_prime -filter_hw_device ocl -i /path/to/hdr.mkv \
  -vf 'scale_rkrga=w=1920:h=1080:format=p010:afbc=0,hwmap=derive_device=opencl:mode=read,tonemap_opencl=format=nv12:p=bt709:t=bt709:m=bt709:tonemap=bt2390:peak=100:desat=0,hwmap=derive_device=rkmpp:reverse=1' \
  -c:v h264_rkmpp -b:v 8M -frames:v 600 -f null -"
```

The `cd /var/lib/jellyfin` matters: the Mali OpenCL compiler reads the current directory, and if the `jellyfin` user
cannot open it every OpenCL program fails with `Failed to build program: -43` (the build log says
`error: Failed to open directory: ./`). That is a test-harness mistake, not a broken blob — the service itself always
runs from `/var/lib/jellyfin`.

## 5. Client side: direct play in the browser through this driver

Jellyfin web decides per file whether the browser can direct-play it; anything it cannot, the server transcodes
(section 4). What the browsers on the board can do with `rockchip-vaapi` installed:

| client on the board | 8-bit H.264 / HEVC | 10-bit HEVC (Main10) / VP9 Profile 2 | note |
|---|---|---|---|
| **Firefox** (VA-API via this driver) | direct play, hardware | direct play, hardware | Jellyfin web probes `hvc1.2.4.L123/L153`; Firefox answers "probably" and MediaCapabilities reports power-efficient |
| **Chromium** (Pi Desktop build, V4L2 → MPP path) | direct play, hardware (HEVC needs the `PlatformHEVCDecoderSupport` flag the image ships) | transcoded by the server | the libv4l-rkmpp lane hides 10-bit profiles on purpose (2.0.2+), so 10-bit falls back to a server transcode instead of a green screen |
| **Chrome** (Google build, VA-API) | direct play, hardware | direct play, hardware since driver 2.1.5 | confirmed on an Orange Pi 5 Plus / Armbian |
| **mpv** / players using libva | hardware | hardware, zero-copy P010 | `hwdec=vaapi`; for 4K60 add `video-sync=display-resample` on GNOME Wayland |

Driver requirements for the client side:

- `rockchip-vaapi-driver` 2.1.5 or later (2.2.0-rc1 adds the RGA export lane: 10-bit surfaces converted to P010 in
  hardware, about five times less CPU at 4K60; needs `librga2`).
- libva finds the driver by the render node's kernel driver name; the package ships `panthor_drv_video.so` and
  `panfrost_drv_video.so` symlinks so no `LIBVA_DRIVER_NAME` is needed. If a distribution names things differently,
  `LIBVA_DRIVER_NAME=rockchip` in the environment does the same.
- The BSP kernel must expose `/dev/mpp_service` and the user must be in `video`.

Quick check that the browser is really decoding in hardware: play a 10-bit file in Firefox, then
`sudo cat /sys/kernel/debug/mpp_service/session_summary` (or watch `top`: hardware decode leaves the CPU idle;
software decode of 4K HEVC pins several cores).

## 6. Library notes for 12.0

- `.aiff` is now recognised as audio (it used to be filed as an image) and `.aifc` is new. Both are PCM containers
  browsers do not decode natively, so playing them in the web client means a server-side audio transcode; a `Music`
  library with a few AIFF files is the quickest way to see it.
- Subtitle settings moved from the global page to per-library settings.
- Upgrading from 10.10.7 / 10.11.x: back up the data directory first, remove repository plugins (they must be rebuilt for
  .NET 10), and expect a full library scan after the upgrade. The legacy `/emby/*` and `/mediabrowser/*` routes are gone.

## 7. Troubleshooting

| symptom | cause / fix |
|---|---|
| transcode fails immediately, log shows `Permission denied` on `/dev/mpp_service` or `/dev/rga` | `jellyfin` not in `video` (and `render`): section 2 |
| `Failed to build program: -43` in a manual ffmpeg test | you ran it from a directory the `jellyfin` user cannot read; `cd /var/lib/jellyfin` first |
| tone mapping greyed out / OpenCL device not found | no OpenCL ICD: `ls /etc/OpenCL/vendors/`; install the OpenCL-capable libmali for the G610 |
| 10-bit HEVC transcodes even in Firefox | driver older than 2.1.3, or libva did not find it (`vainfo` shows a different driver); check `vainfo --display wayland` |
| Chromium shows green/garbled on 10-bit | the V4L2 path in a Chromium build without the 10-bit hide (pre-2.0.2 images); update, or use Firefox for HDR |
| board loses Ethernet after suspend | not Jellyfin: vendor 6.1 stmmac resume race, fixed in Armbian `rk-6.1-rkr7.2` (armbian/linux-rockchip#548) and in the Pi Desktop kernel |

## 8. Pointers

- Driver: https://github.com/defcom5-rockchip/rockchip-vaapi (releases, KNOWN-ISSUES.md)
- Jellyfin 12.0 release notes: https://github.com/jellyfin/jellyfin/releases/tag/v12.0
- Jellyfin hardware acceleration docs (Rockchip page): https://jellyfin.org/docs/general/administration/hardware-acceleration/rockchip
