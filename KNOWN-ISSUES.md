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

## KI-3: 10-bit zero-copy display — FIXED in v2.1.3 (a mistyped fourcc, never the GPU)

**Status:** fixed in v2.1.3 · **v2.0.0 – v2.1.2 are affected — update.**

**What users saw:** 10-bit content (HEVC Main10, VP9 Profile 2, H.264 High10) never
displayed through the zero-copy path. mpv `--hwdec=vaapi` and Firefox silently fell back
to software decode; Chromium showed green. Earlier versions of this file blamed a
"panfork 16-bit texture wall" and declared the display path blocked until Mesa ≥ 25.
That diagnosis was wrong.

**Root cause:** the 10-bit `SEPARATE_LAYERS` export labelled the chroma plane with the
fourcc `0x36315247`. That spells `"GR16"`, which is not a DRM format. `DRM_FORMAT_GR1616`
is `fourcc('G','R','3','2')` = `0x32335247`. Mesa rejected every 10-bit UV plane with
`EGL_BAD_MATCH: unknown drm fourcc format` — a message that only appears with
`EGL_LOG_LEVEL=debug`. The luma plane (`R16`) was fine, so every consumer got a valid Y
plane and a refused UV plane, and each handled that its own way (fallback or green).

**Why it hid for three releases:**
1. mpv's format probe passed — the driver's pre-decode placeholder surface exports as
   8-bit (`R8`/`GR88`), so the probe never touched the 10-bit branch.
2. mpv 0.38 also mis-renders *software-uploaded* 10-bit frames on this GPU stack (a
   separate, real symptom), which made "16-bit textures are broken" plausible.
3. Nobody captured Mesa's own error text. `EGL_LOG_LEVEL=debug` names the failing check
   in one line. That is now a rule in AGENTS.md.

**How it was found (2026-09-06):** a forum report of `mpv --hwdec=rkmpp
--vf=scale_rkrga=force_yuv=auto` displaying P010 correctly on the same panfork stack.
mpv's `dmabuf_interop_gl` splits that P010 into exactly the `R16` + `GR1616` EGLImages
this driver exports — same GPU, same fourccs, same modifier — so the GPU was exonerated
and the descriptor was the only variable left. Modifier and buffer-allocator variants
changed nothing; the fourcc did.

**Measured on RK3588S (Orange Pi 5B, BSP 6.1, panfork Mesa 23.0.5), 4K HEVC Main10:**

| test | v2.1.2 | v2.1.3 |
|---|---|---|
| mpv `--hwdec=vaapi`, EGL import failures per 40 frames | 40 | **0** |
| mpv VO format | `yuv420p10` (software fallback) | **`vaapi[p010]` zero-copy**, correct picture |
| Firefox, Main10 | software fallback | **hardware, real time** (360 ten-bit surfaces in 35 s), correct picture |
| 8-bit H.264 / HEVC zero-copy (regression) | correct | correct |

**What is still gated:** Main10, High10 and VP9 Profile 2 remain behind
`RKVA_ADVERTISE_ALL=1` until the Chromium re-test is eyeballed; the default menu will
flip once it is. The `[tenbit-panfork-guard]` mpv profile (software 10-bit → 8-bit) is
still shipped for the software-decode case.

**Reproduce the failure on an old build:** `EGL_LOG_LEVEL=debug RKVA_ADVERTISE_ALL=1 mpv
--hwdec=vaapi <main10 file> 2>&1 | grep "EGL user error"`.

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
