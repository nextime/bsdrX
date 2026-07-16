/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 */
/* Media-effect registry — the process-wide hook a media plugin registers so the core routes voice/video
 * through it (voice-changer, 2D->3D). Kept as a tiny global (not app state) because the hot paths that
 * consume it (micsub/micsniff for audio; capture for video) don't all hold an app pointer. The plugin
 * manager sets/clears these via the ABI host services; the core calls apply_* in the media path and
 * falls back to its own processing (or none) when nothing is registered. Guarded by an internal mutex,
 * and the effect callback is invoked UNDER the lock so a plugin unload can't race a call in flight. */
#ifndef BSDR_MEDIAFX_H
#define BSDR_MEDIAFX_H

#include <stdint.h>
#include "bsdr/plugin.h"   /* bsdr_audio_fx_fn */

/* Set (or clear, with NULL) the voice/audio effect. Clearing blocks until any in-flight call returns. */
void bsdr_mediafx_set_audio(bsdr_audio_fx_fn fn, void *user);

/* If a plugin audio effect is registered, run it over `pcm` in place and return 1; else return 0 (the
 * caller then applies its own built-in effect / passthrough). */
int  bsdr_mediafx_apply_audio(int16_t *pcm, int frames, int rate, int channels);

/* Same-dimensions video-effect chain (encode path). add() inserts (or with fn=NULL removes) an effect
 * keyed by `user`, ordered by `order`; remove_owner() drops all for a `user` (plugin unload). apply()
 * runs the chain in order over the NV12 frame in place and returns the number of effects applied. */
void bsdr_mediafx_video_add(bsdr_video_fx_fn fn, void *user, int order);
void bsdr_mediafx_video_remove_owner(void *user);
int  bsdr_mediafx_apply_video(uint8_t *y, int y_stride, uint8_t *uv, int uv_stride, int width, int height);

/* Single dim-changing video-source transform (2D->3D). set() copies the struct by value (NULL clears,
 * blocking until any in-flight apply returns). dims() asks a registered transform for the SBS output
 * size and returns 1 if one claimed it (0 = none / declined -> caller uses built-in). apply() runs the
 * transform src->dst under the lock and returns 1 if applied (0 = none registered). */
void bsdr_mediafx_set_video_src(const bsdr_video_src_fx *fx);
int  bsdr_mediafx_video_src_active(void);   /* 1 if a transform is currently registered */

/* Depth estimator (2d-3d plugin) for a host that renders stereo itself (Android GL). set() registers/
 * clears (NULL blocks until any in-flight call returns). apply() runs it into `out` (w*h floats) under
 * the lock and returns 1 if handled + filled, 0 if none registered or the estimate failed. */
void bsdr_mediafx_set_depth(bsdr_depth_fx_fn fn, void *user);
int  bsdr_mediafx_depth_active(void);
int  bsdr_mediafx_apply_depth(int tier, const uint8_t *gray, int w, int h, float *out);

/* Packed-RGB face-swap interface (faceswap plugin) for a host with RGB frames (Android GL). set() copies
 * the struct (NULL clears, blocking on any in-flight call). process() swaps faces in place (returns the
 * face count >=0, or -1 if none registered / not ready); set_source() sets the identity (0 ok, -1 none). */
void bsdr_mediafx_set_face(const bsdr_face_fx *fx);
int  bsdr_mediafx_face_active(void);
int  bsdr_mediafx_face_process(uint8_t *rgb, int w, int h);
int  bsdr_mediafx_face_set_source(const uint8_t *rgb, int w, int h);
int  bsdr_mediafx_video_src_dims(int in_w, int in_h, int *out_w, int *out_h);
int  bsdr_mediafx_apply_video_src(const uint8_t *sy, int sys, const uint8_t *suv, int suvs, int in_w, int in_h,
                                  uint8_t *dy, int dys, uint8_t *duv, int duvs, int out_w, int out_h);

#endif /* BSDR_MEDIAFX_H */
