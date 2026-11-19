# Architecture and frame lifecycle

> Documented version: `0.4.2.dev4`. Interfaces and restrictions may change.

## Pipeline

A typical processing pipeline looks like this:

```text
file / RTSP / another FFmpeg-recognized source
  │
  v
FFmpeg demux + video decoder — HWAccel (SOFTWARE / CUDA / VAAPI / D3D11VA)
  │
  v
InputTransfer: RefTransfer or hardware VRAM -> CPU RAM transfer
  │
  v
IFrameProcessor: CPU / CUDA / VAAPI / OpenCL
  │                pixel conversion and/or resizing
  v
OutputTransfer: no copy / CUDA -> CPU / VAAPI -> CPU
  │
  v
Frame: CpuPacked / CpuPlanar / Cuda / Vaapi / OpenCLBuffer / OpenCLImage
  │
  v
Reader: current frame / new frame / request next frame
```

Key points:

1. **`hwaccel` controls decoding.** Even when decoding takes place on a GPU, a CPU postprocessor may download the decoded frame to host RAM.
2. **`postprocessor` controls processing.** For instance, VAAPI-to-OpenCL uses VAAPI for decoding and OpenCL for color conversion.
3. **`output_memory` controls result placement.** CPU RAM, CUDA device memory, VAAPI surfaces, and OpenCL buffers are different representations.
4. **`out_format` controls the requested format.** `NATIVE` preserves the representation along the selected pipeline. Inspect `frame.pix_fmt` to identify the actual output pixel format.

For supported combinations, see [compatibility.md](compatibility.md).

## Reading modes

### Latest-drop

This is the original, primary, and best-developed mode. It prioritizes freshness without allowing a queue of increasingly stale frames to accumulate. A producer runs in its own thread, independently of the Python consumer, and each newly published frame replaces the previous one. Other reading modes were added later and are not yet equally mature. See [reading-models.md](reading-models.md) for individual method semantics.

### Background decoding

With `auto_start=True` (the default), Reader creates a worker thread that processes incoming frames continuously and publishes the most recent result in one slot. `read_frame()` / `read()` return the slot's current contents (potentially the same frame more than once); `read_new_frame()` / `read_new()` return it only when its ID changes; `read_latest_frame(timeout)` waits for a new publication, up to the timeout. A slower consumer can miss intermediate frames.

### On-demand decoding (`on_demand=True`)

`reader.next_frame()` requests a frame. This mode is for file sources, requires `reader.start()` if `auto_start=False`, and is incompatible with `realtime_file=True` and recognized live sources. Unlike latest-frame access, it allows sequential processing at the consumer's pace, without losing frames solely because Python is slower than the producer. This is not a promise to recover frames lost through decode errors. In the full repository, this is demonstrated by `examples/06_zero_copy_inference.py`.

### PTS and real-time pacing

`realtime_file=True` attempts to reproduce the PTS intervals of a local file. The current implementation polls in 1 ms increments, which may become problematic at very high frame rates. This setting is ignored for recognized live sources. Frame PTS metadata is still available, for example when matching frames from different sources by timestamp.

## Errors and states

Reader states are `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, and `STOPPED`. `STREAMING` is entered after receiving a keyframe. In background mode, a failure causes a new session to be attempted after `reconnect_delay_sec`; in on-demand mode, errors are returned to the caller and processing stops.

VAAPI has a special path for skipping individual damaged surfaces. After 25 consecutive decode errors, it attempts session recovery without rebuilding the FFmpeg and codec contexts. This is not a guarantee of recovery from every driver error.

For API details see [reader-api.md](reader-api.md); for confidence levels and limitations see [support-status.md](support-status.md).
