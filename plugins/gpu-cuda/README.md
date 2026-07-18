# gpu-cuda — GPU acceleration (CUDA) provider

A **payload** plugin (no code): it delivers the ONNX Runtime **CUDA execution provider**
(`libonnxruntime_providers_cuda.so` + `libonnxruntime_providers_shared.so`) next to the agent's
bundled ONNX Runtime, so the AI plugins — **voice changer (RVC)**, **2D→3D depth**, **face swap** —
run their neural nets on an **NVIDIA GPU** in real time instead of on the CPU.

- **Not a dependency.** The AI plugins run on CPU without this; installing it just enables the GPU
  tiers. When the GPU/runtime isn't present they fall back to CPU with an honest status message.
- **Host requirement:** an NVIDIA GPU with the **CUDA 12 runtime + cuDNN 9** available on the loader
  path (the driver's userspace runtime — no full CUDA toolkit needed).
- **Platform:** Linux / x86_64 only (the provider `.so` exists only there). Windows uses DirectML and
  macOS uses CoreML, both built into their ONNX Runtime — no payload plugin needed.
