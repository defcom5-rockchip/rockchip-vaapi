/*
 * av1.h — AV1 OBU re-serialisation from VA-API decode parameters, for Rockchip MPP
 *
 * MPP's av1d wants a complete low-overhead OBU stream (sequence header once, then one OBU_FRAME per frame) and
 * parses every header itself; VA-API clients hand the driver parsed parameters and tile payloads only.  This module
 * turns a VADecPictureParameterBufferAV1 (+ slice parameters/data) back into the bytes MPP needs, with the small
 * amount of decoder state the frame header depends on (reference order hints, saved global-motion parameters).
 *
 * Copyright (C) 2026 defcom5-rockchip
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#define GST_USE_UNSTABLE_API 1
#include "av1/gstav1bitwriter.h"

/* What the driver remembers about a decoded picture (keyed by its surface); the eight reference slots point at these. */
typedef struct {
    bool     valid;
    uint8_t  frame_type;          /* VA frame_type of that picture */
    uint32_t order_hint;
    uint16_t width, height;       /* upscaled width, frame height */
    int32_t  gm[8][6];            /* SavedGmParams: identity after intra frames */
    uint8_t  gm_type[8];
    uint8_t  feature_enabled[8][8]; /* that picture's segmentation features, for a successor that keeps them (update_data = 0) */
    int16_t  feature_data[8][8];
} Av1RefInfo;

/* Assumption the submit policy relies on: MPP's av1d does not enforce showable_frame when it meets show_existing_frame
   (the driver shows hidden pictures that way, and libaom's non-showable ARFs would otherwise be refused).  Fluster
   passing on the vectors that have them is the evidence. */

/* One tile's payload: [slice_data_offset, +slice_data_size) of a VASliceData buffer. */
typedef struct { const uint8_t *data; size_t len; } Av1Tile;

static inline void av1_gm_identity(int32_t gm[8][6]) { for (int r = 0; r < 8; r++) for (int i = 0; i < 6; i++) gm[r][i] = (i % 3 == 2) ? (1 << 16) : 0; }

/* Sequence header from the first picture of a sequence (max frame size = that picture's size). 0 on success. */
int av1_seq_from_va(const VADecPictureParameterBufferAV1 *pp, GstAV1SequenceHeaderOBU *seq);

/* Does this picture's sequence-level information still match the sequence header in use? */
bool av1_seq_matches(const VADecPictureParameterBufferAV1 *pp, const GstAV1SequenceHeaderOBU *seq);

/* Frame header for one picture.  refs[i] describes the picture currently in reference slot i (ref_frame_map[i]);
 * refresh_frame_flags is supplied by the caller (deferral logic).  Returns 0 on success, -1 if the picture uses a
 * feature the stack cannot carry (then the caller falls back / fails the picture). */
int av1_frame_from_va(const VADecPictureParameterBufferAV1 *pp, const GstAV1SequenceHeaderOBU *seq,
                      const Av1RefInfo refs[8], uint8_t refresh_frame_flags, GstAV1FrameHeaderOBU *fh);

/* After a picture is submitted: what its slots must remember. */
void av1_ref_from_frame(const VADecPictureParameterBufferAV1 *pp, const GstAV1FrameHeaderOBU *fh, Av1RefInfo *out);

/* OBU writers.  Return bytes written, 0 on failure. */
size_t av1_write_seq_obu(const GstAV1SequenceHeaderOBU *seq, uint8_t *dst, size_t cap);
/* One OBU_FRAME: header bits, byte alignment, the tile-group flag byte when there is more than one tile, every tile
 * but the last prefixed with a 4-byte little-endian size (the header is written with tile_size_bytes = 4). */
size_t av1_write_frame_obu(const GstAV1FrameHeaderOBU *fh, const GstAV1SequenceHeaderOBU *seq,
                           const Av1Tile *tiles, int ntiles, uint8_t *dst, size_t cap);
/* A FRAME_HEADER OBU with show_existing_frame = 1 for `slot` (makes MPP output that slot's picture).  av1_write_frame_obu
   keeps a 64 KiB header scratch on the stack: fine on client threads (8 MB default), noted because vaSyncSurface runs it. */
size_t av1_write_show_existing(const GstAV1SequenceHeaderOBU *seq, int slot, uint8_t frame_type, uint8_t *dst, size_t cap);
