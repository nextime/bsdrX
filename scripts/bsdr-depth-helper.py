#!/usr/bin/env python3
# bsdr-depth-helper.py — reference AI depth estimator for bsdrX 2D->3D "AI" mode.
#
# bsdrX (threed.c / the Android GL path) pipes small grayscale frames to this process and reads back
# a depth map, framed identically in each direction:
#
#     bytes[0:4] = b"BSDD"
#     bytes[4:6] = width  (uint16, little-endian)
#     bytes[6:8] = height (uint16, little-endian)
#     bytes[8:8+w*h] = w*h grayscale bytes   (input: luma; output: depth, 0=far .. 255=near)
#
# One reply per request, same w/h. bsdrX calls this at a reduced rate (a few fps on a ~256px frame),
# so a small CPU model is plenty — this is meant to stay usable on old laptops.
#
# Depth backends, in order of preference:
#   1. ONNX Runtime + a MiDaS-small ONNX model  (real monocular depth; --model or $BSDR_DEPTH_MODEL)
#   2. a built-in OpenCV/NumPy structure-from-focus heuristic (no model needed, rough but real cues)
#
# Wire it into bsdrX's web panel "AI depth helper command" field, e.g.:
#     python3 /path/to/scripts/bsdr-depth-helper.py --model /path/to/midas_v21_small.onnx
# MiDaS-small ONNX: https://github.com/isl-org/MiDaS  (models/model-small.onnx, ~66 MB).
#
# Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
import argparse
import os
import struct
import sys

MAGIC = b"BSDD"


def log(*a):
    print("bsdr-depth-helper:", *a, file=sys.stderr, flush=True)


def read_exact(f, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


class OnnxBackend:
    """MiDaS-small (or any single-input/single-output depth) ONNX model on CPU."""

    def __init__(self, model_path):
        import numpy as np
        import onnxruntime as ort
        self.np = np
        so = ort.SessionOptions()
        so.intra_op_num_threads = max(1, (os.cpu_count() or 2) // 2)
        self.sess = ort.InferenceSession(model_path, so, providers=["CPUExecutionProvider"])
        self.inp = self.sess.get_inputs()[0]
        # NCHW, e.g. [1,3,256,256]; fall back to 256 when a dim is dynamic
        shp = self.inp.shape
        self.ih = int(shp[2]) if isinstance(shp[2], int) else 256
        self.iw = int(shp[3]) if isinstance(shp[3], int) else 256
        log(f"ONNX depth model {model_path} input {self.iw}x{self.ih}")

    def depth(self, gray, w, h):
        np = self.np
        import cv2
        g = np.frombuffer(gray, dtype=np.uint8).reshape(h, w).astype(np.float32) / 255.0
        rgb = cv2.resize(np.stack([g, g, g], axis=0).transpose(1, 2, 0), (self.iw, self.ih))
        # ImageNet normalisation (MiDaS)
        mean = np.array([0.485, 0.456, 0.406], np.float32)
        std = np.array([0.229, 0.224, 0.225], np.float32)
        x = ((rgb - mean) / std).transpose(2, 0, 1)[None].astype(np.float32)
        out = self.sess.run(None, {self.inp.name: x})[0].squeeze()
        out = cv2.resize(out.astype(np.float32), (w, h))
        lo, hi = float(out.min()), float(out.max())
        d = (out - lo) / (hi - lo) if hi > lo else np.zeros_like(out)  # MiDaS: larger = nearer
        return (d * 255.0).clip(0, 255).astype(np.uint8).tobytes()


class HeuristicBackend:
    """No model: combine a vertical gradient with local sharpness (focus = nearer). Rough but real,
    and better than the built-in C heuristic because it uses per-pixel detail."""

    def __init__(self):
        import numpy as np
        self.np = np
        try:
            import cv2  # noqa: F401
            self.cv = True
        except Exception:
            self.cv = False
        log("using built-in heuristic depth (install onnxruntime + --model for real AI)")

    def depth(self, gray, w, h):
        np = self.np
        g = np.frombuffer(gray, dtype=np.uint8).reshape(h, w).astype(np.float32)
        # local sharpness (Laplacian magnitude, blurred) = focus cue -> nearer
        if self.cv:
            import cv2
            lap = np.abs(cv2.Laplacian(g, cv2.CV_32F, ksize=3))
            focus = cv2.blur(lap, (15, 15))
        else:
            dx = np.abs(np.diff(g, axis=1, prepend=g[:, :1]))
            dy = np.abs(np.diff(g, axis=0, prepend=g[:1, :]))
            focus = dx + dy
        f = focus / (focus.max() + 1e-6)
        grad = np.linspace(0.0, 1.0, h, dtype=np.float32)[:, None]  # bottom = nearer
        d = 0.5 * grad + 0.5 * f
        d = (d - d.min()) / (d.max() - d.min() + 1e-6)
        return (d * 255.0).clip(0, 255).astype(np.uint8).tobytes()


def make_backend(model):
    if model:
        try:
            return OnnxBackend(model)
        except Exception as e:
            log(f"ONNX backend failed ({e}); falling back to the heuristic")
    return HeuristicBackend()


def main():
    ap = argparse.ArgumentParser(description="bsdrX 2D->3D AI depth helper")
    ap.add_argument("--model", default=os.environ.get("BSDR_DEPTH_MODEL"),
                    help="MiDaS-small ONNX model (else a built-in heuristic is used)")
    args = ap.parse_args()
    backend = make_backend(args.model)

    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    while True:
        hdr = read_exact(stdin, 8)
        if hdr is None:
            break
        if hdr[:4] != MAGIC:
            log("bad frame header; exiting")
            break
        w, h = struct.unpack_from("<HH", hdr, 4)
        gray = read_exact(stdin, w * h)
        if gray is None:
            break
        try:
            depth = backend.depth(gray, w, h)
        except Exception as e:
            log(f"depth failed ({e}); returning mid-grey")
            depth = bytes([128]) * (w * h)
        stdout.write(MAGIC + struct.pack("<HH", w, h) + depth)
        stdout.flush()


if __name__ == "__main__":
    main()
