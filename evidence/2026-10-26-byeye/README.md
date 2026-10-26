# By-eye checks of driver 4.0.0 on a Plaid Pi 6.0 image — 2026-10-26

Orange Pi 5B (RK3588S), a freshly flashed Plaid Pi 6.0 release image (rockchip-vaapi 4.0.0-1 installed from the package,
MPP `+pd35`, mpv 0.41 `+pd3`), Wayland desktop on HDMI. The maintainer watched; the driver side was checked over SSH while
each clip played: which process held `/dev/mpp_service` (the hardware decoder) and its CPU use from `top`.

Clips (20 s each, encoded on a PC from a real 4K60 H.264 clip, ffmpeg + libx264):
- `interlaced-1080i25-h264.mp4` — High profile, 1920×1080, interlaced top field first (MBAFF), 25 frames/s.
- `high10-1080p60-h264.mp4` — High 10, 1920×1080, progressive, 60 frames/s.

| check | decoder | CPU | by eye |
|---|---|---|---|
| `mpv --fs --loop` interlaced clip (mpv default: no deinterlace) | mpv holds `/dev/mpp_service` | ~36 % | smooth, comb lines on moving edges (weaved output, KI-20) |
| `mpv --fs --loop --deinterlace=yes` interlaced clip | mpv holds `/dev/mpp_service` | ~107–146 % | no comb lines |
| Firefox 159, High 10 clip as `file://` | Firefox's RDD process holds `/dev/mpp_service`, libva and the driver mapped | RDD ~8 % | normal colours, no blocking or smearing |

Not checked: interlaced H.264 in Firefox. Chromium and Chrome do not hand High 10 H.264 to VA-API
(`../2026-10-25-full-regression/browsers.txt`) and decode interlaced H.264 in software (KNOWN-ISSUES KI-20).
