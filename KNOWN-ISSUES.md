# Known Issues

Hardware-verified on Orange Pi 5B (RK3588S) unless noted. Reports from other
RK3588/RK3588S boards are welcome — decode silicon is identical across the two,
so behaviour should match.

Current as of **v2.0.0 "Reframe"**.

---

## KI-1: Some H.264 files show a green corner and stutter

**Status:** open, needs bisect · **Severity:** playback corruption on affected content

Standard progressive H.264 decodes correctly, but at least one 720p60 High-profile
file with B-frames (`has_b_frames=1`, level 4.1) plays with a green corner and
visible stutter. The inherited B-frame handling appears incomplete for some
reference patterns.

**If you hit this, a report helps** — post the output of:

```sh
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,level,width,height,r_frame_rate,field_order,has_b_frames \
  -of default=noprint_wrappers=1 <file>
```

**Workaround:** the file will play correctly with software decode
(`mpv --hwdec=no`, or by not passing the hardware flags to the browser).

---

## KI-2: HEVC decode — FIXED on the `deep-ink` branch, pending soak

**Status:** fixed in development, not yet in the advertised menu · **Severity:** feature absent in releases

HEVC never worked in this driver — there was no HEVC bitstream assembler; decode requests
were mis-routed and MPP received a stream it could not parse (solid green at every bit
depth, for everyone, since the driver existed).

The assembler now exists (`src/hevc.c`, branch `deep-ink`): VPS synthesised from scratch,
SPS/PPS reconstructed from the VA-API parameters, Annex B stitching, plus two non-obvious
requirements discovered on hardware — `max_num_reorder_pics` must be 0 for a stateless
VA-API bridge, and the original SPS's reference-picture-set count class must be detected
(deterministically, from `st_rps_bits`) because it changes the slice-header bit layout.
**Verified bit-exact against software decode** (pixel-identical frames, 8-bit AND 4K
HDR10 Main10) with objective tooling committed in `tools/`.

HEVC returns to the default advertised menu after real-playback soak — not before.
Until then: test with `RKVA_ADVERTISE_ALL=1`.

## KI-3: 10-bit content (VP9 Profile 2, H.264 High10) is not advertised

**Status:** conversion fixed; display path blocked upstream · **Severity:** feature absent

Two separate problems, one fixed:

1. **Layout (fixed, in the `deep-ink` branch):** MPP returns 10-bit frames as
   packed NV15, which the driver exported labelled as P010 — different byte
   layouts, hence corruption. The repack to true P010 is written and verified
   bit-correct on hardware.
2. **Display (blocked):** the GPU stack these BSP kernels ship with (panfork,
   Mesa 23) advertises 16-bit texture and dmabuf formats it cannot actually
   render — any 10-bit frame reaching GL shows as a solid blue field. This is
   outside the driver; it clears with Mesa ≥ 25 / the Panthor driver, which
   needs a newer kernel than the BSP 6.1 line.

Because of (2), 10-bit stays unadvertised for now: correct software playback
beats a blue screen. The copy-back path does work today if you want to test it
(`mpv --hwdec=vaapi-copy` with `RKVA_ADVERTISE_ALL=1`).

---

## KI-4: Chromium-family UI flicker is not a driver issue

**Status:** upstream/GPU stack · **Severity:** cosmetic

Flickering while typing or redrawing UI in Chromium/Chrome comes from ANGLE
(mandatory in modern Chromium) on the panfork GPU stack, independent of video
decode. Firefox does not use ANGLE and is unaffected. If video *content* is
corrupt rather than the UI flickering, that is a different problem — please
report it.

---

## KI-5: some HEVC streams use SPS-indexed reference-picture sets (not yet reconstructible)

**Status:** open (Phase 1.6 candidate) · **Severity:** affected streams fall back to software

Slices that reference SPS-level RPS entries by index (`short_term_ref_pic_set_sps_flag=1`)
or use inter-RPS prediction cannot be rebuilt from what VA-API provides — the set tables
are not in the parameter struct. The driver detects this and logs a WARNING rather than
guessing. The planned fix rebuilds the picture's RPS from `ReferenceFrames` and rewrites
the slice header with an explicit inline set. Long-term SPS reference sets and custom
scaling lists are likewise not yet handled (defaults are used).

## Reporting

Open an issue with: board model, kernel (`uname -a`), distro, driver version
(`vainfo` prints it), the codec/resolution involved, and `ffprobe` output for a
local file that reproduces it. For browser reports, `about:support` (Firefox) or
`chrome://gpu` (Chrome) plus whether `sudo fuser -v /dev/mpp_service` shows the
decoder process during playback.
