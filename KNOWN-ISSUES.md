# Known Issues

Hardware-verified on Orange Pi 5B (RK3588S) unless noted. Reports from other
RK3588/RK3588S boards are welcome — decode silicon is identical across the two,
so behaviour should match.

Current as of **v2.0.0 "Reframe"**.

---

## KI-1: H.264 streams with B-frames decode to a corrupted picture

**Status:** open, reproducible, present in v2.0 · **Severity:** wrong picture on affected content

H.264 video that uses B-frames decodes in hardware but comes out wrong: ghosting and
doubled edges, occasional solid green frames. H.264 without B-frames is pixel-perfect, so
the split is clean and easy to check with `ffprobe … -show_entries stream=has_b_frames`.

This matters more than an edge case: most real-world H.264 uses B-frames. Streaming sites
mostly serve VP9 to browsers on this platform, which is why the fault is easy to miss —
it shows up on local files and downloads.

Measured objectively (hardware frames compared pixel-by-pixel against a software reference,
`tools/hevc-ladder.sh`): mean absolute difference ≈ 36/255 on affected frames, versus 0.00
for every codec that works. Reproducer: any H.264 High-profile clip encoded with B-frames.

Two things were ruled out while investigating: it is not the missing reordering constraint
(adding an H.264 VUI declaring `max_num_reorder_frames = 0` changed which streams took the
hardware path but not the corruption, and was reverted), and it is not decoded-picture-buffer
sizing (a generous `max_dec_frame_buffering` made no difference). The remaining suspect is
reference-list handling — the same class of defect the HEVC path had with its reference-picture
sets, in the code that builds H.264 reference lists.

**Mitigated since v2.1.1:** H.264 output is now routed through the same decode-order queue
the other codecs use, instead of a shortcut that assumed output order matches submit order.
Affected streams no longer produce a corrupted picture — players fall back to software decode
and show the correct image. Hardware decode of B-frame H.264 is still not working, so this
issue stays open; a wrong picture is worse than a slow one.

**Workaround:** none needed for correctness; the picture is right. If the CPU cost matters,
re-encoding without B-frames restores hardware decode.

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
