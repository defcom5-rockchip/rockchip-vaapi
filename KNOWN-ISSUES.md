# Known Issues

Hardware-verified on Orange Pi 5B (RK3588S) unless noted. Reports from other
RK3588/RK3588S boards are welcome — decode silicon is identical across the two,
so behaviour should match.

Current as of **v2.0.0 "Reframe"**.

---

## KI-1: H.264 B-frame corruption — ROOT CAUSE FOUND, fixed in v2.1.2

**Status:** fixed in v2.1.2 · **v2.1.1 and earlier are affected — update.**

The full story, because it hid from everyone including us: the driver synthesises the
H.264 parameter sets, and the PPS hardcoded `num_ref_idx_default_active = 1` as a
"conservative default". Real encoders set it higher (broadcast content: 4; typical x264:
2), and slices rely on that default exactly once the decoder's reference buffer fills —
about half a second in. So every I-frame was pixel-perfect, playback *started* clean, and
the picture then drifted into ghosting. Short verification windows (ours included) sailed
past it, which is how it shipped: **v2.1.1's "correct from start" claim was measured on
12-frame windows and is wrong beyond the first GOP.** It also masqueraded as a
seek-only bug for a full day of investigation.

v2.1.2 learns the true defaults from the stream itself (any slice that doesn't override
them states them) and re-emits a corrected PPS once. Verified 0.0/255 across 144-frame
runs on broadcast-style and open-GOP reproducers, all seek cases included, plus the full
regression suite at the new 48-frame minimum depth.

If you saw ghosting or "flicker" on H.264 with any earlier version, it was this.

## KI-2: HEVC — FIXED, and now advertised (8-bit)

**Status:** fixed on `deep-ink`; `HEVCMain` restored to the advertised list · **Severity:** was total

HEVC had never decoded through this driver, at any bit depth — there was no HEVC bitstream
assembler, so the decoder received a stream it could not parse and returned empty (green)
buffers. The assembler now exists: VPS synthesised, SPS/PPS reconstructed from the VA-API
parameters, Annex B stitching, with two requirements found on hardware — `max_num_reorder_pics`
must be 0 for a stateless bridge, and the reference-picture-set count must be taken from
`num_short_term_ref_pic_sets` and never revised mid-stream (re-sending a changed SPS flushes
the decoder's picture buffer and destroys the references B-frames depend on).

Verified pixel-identical to software decode at 8-bit and 10-bit, with a full 12-minute
4096×1714 Main10 feature playing through end to end. **HEVC Main (8-bit) is now advertised
and hardware-decodes in both Firefox and Chrome.** Main10 stays unadvertised — see KI-3.

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
