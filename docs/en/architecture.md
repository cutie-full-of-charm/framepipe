# Architecture and frame lifecycle

> Version at the time of writing: `0.4.2.dev4`; interfaces and limitations may change.

## Pipeline

In general, the processing pipeline looks like this:

```text
file / RTSP / another FFmpeg-recognized source
  │
  V
FFmpeg demux + video decoder -- HWAccel (SOFTWARE / CUDA / VAAPI / D3D11VA)
  │
  V
InputTransfer: RefTransfer, or hardware VRAM -> CPU RAM transfer
  │
  V
IFrameProcessor: CPU / CUDA / VAAPI / OpenCL
  │                pixel-format conversion and/or resizing
  V
OutputTransfer: no copy / CUDA->CPU / VAAPI->CPU
  │
  V
Frame: CpuPacked / CpuPlanar / Cuda / Vaapi / OpenCLBuffer / OpenCLImage
  │
  V
Reader: latest frame / new frame / frame on request
```

Important points:

1. `hwaccel` -- decoding. Even if decoding runs on the GPU, the frame may be downloaded to the host (roughly speaking, from VRAM to RAM).
2. `postprocessor` -- processing. For example, VAAPI->OpenCL combines VAAPI decoding with OpenCL conversion.
3. `output_memory` -- placement after processing. RAM, CUDA, VAAPI surfaces, and OpenCL GPU buffers are different ways to store the result.
4. `out_format` -- output format. `NATIVE` preserves the native representation within the pipeline. You can inspect the pipeline's actual output pixel format via `frame.pix_fmt`.

Supported combinations: [compatibility.md](compatibility.md).

## Reading modes

The basic mode -- the one originally designed and developed in the most detail -- is **latest-drop**: a producer runs in a separate thread, independently of the Python consumer, and each new frame overwrites the previous one. The other modes were, in a sense, bolted onto that original design; some more successfully than others. See the separate section for details.

The slot diagram, differences between `read*`/`next*`, `realtime_file`, and the shared read position are described in [reading-models.md](reading-models.md). The important point here is that the frame delivery model is independent of `PostProcessor` and `MemoryType`.

## Errors and states

`State`: `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, `STOPPED`. The state changes to `STREAMING` after a keyframe is received. For more on the lifecycle and access to the last terminal error, see [reader-api.md](reader-api.md).
