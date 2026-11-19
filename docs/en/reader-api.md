# Python API: `Reader`

> Documented version: `0.4.2.dev4`. This page describes the `framepipe.Reader` Python wrapper (`framepipe/reader.py`).

## Creating a Reader

```python
from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, Reader

reader = Reader(
    source="video.mp4",
    name="camera-01",
    out_format=OutputFormat.BGR24,
    hwaccel=HWAccel.SOFTWARE,
    postprocessor=PostProcessor.CPU,
    output_memory=MemoryType.CPU,
)
```

| Argument | Default | Purpose |
| --- | --- | --- |
| `source` | required | Video file path or source URL |
| `name` | required | Reader name, also used in logs |
| `verbose` | `False` | More detailed logging |
| `videostream_id` | `0` | Video-stream ordinal in the container, not the index among all streams |
| `reconnect_delay_sec` | `1.0` | Pause before starting another session after an error in background mode |
| `degraded_timeout_sec` | `3.0` | Age threshold for the most recent frame in `is_alive()` |
| `av_container_options` | `None` | FFmpeg demuxer/container options; the C++ argument is named `av_options` |
| `out_format` | `OutputFormat.BGR24` | `BGR24`, `RGB24`, `GRAY8`, `NATIVE` |
| `hwaccel` | `HWAccel.SOFTWARE` | Decoding backend |
| `postprocessor` | `PostProcessor.CPU` | Frame processing backend |
| `output_memory` | `MemoryType.CPU` | Result location |
| `out_w`, `out_h` | `0`, `0` | Target size when both dimensions are positive |
| `scale` | `1.0` | Scaling multiplier if both explicit target dimensions are not provided |
| `realtime_file` | `False` | Approximate PTS-based pacing for file sources (currently 1 ms polling) |
| `auto_start` | `True` | Start the worker thread automatically |
| `on_demand` | `False` | Request file frames one at a time using `next_frame()` |

The wrapper recognizes live sources from URL prefixes: `rtsp://`, `rtmp://`, `rtmps://`, `udp://`, `srt://`, and `mms://`. It sets low-latency FFmpeg defaults for these sources (`rtsp_transport=tcp`, `fflags=nobuffer`, `flags=low_delay`, `max_delay=0`, `analyzeduration=0`, `probesize=32`). User-supplied `av_container_options` override options with the same keys.

## Delivery model versus frame representation

The Reader API separates **how** to get a frame (immediately, only if new, wait for a new one, or request the next) from **what** to get (a NumPy array or a `Frame` object). Background mode implements latest-drop, which is the original priority for real-time ML/CV and edge systems; `on_demand` is intended for sequential file processing. The full model, diagrams, and recommendations are in [reading-models.md](reading-models.md).

## Frame access methods

| Method | Return value | Use case |
| --- | --- | --- |
| `read_frame()` | Latest `Frame` or `None` | Read the current result repeatedly; CPU/GPU/NATIVE |
| `read_new_frame()` | New `Frame` or `None` | Avoid duplicates, though intermediate publications may be missed |
| `read_latest_frame(timeout=1.0)` | `Frame` or `None` | Wait up to the given timeout for a new frame |
| `next_frame()` | `Frame` or `None` at EOF | File sources with `on_demand=True`; request one result |
| `read()` | NumPy array or `None` | Packed CPU output only (`RGB24`, `BGR24`, `GRAY8`) |
| `read_new()` | NumPy array or `None` | Same, but only for an updated frame |
| `read_latest(timeout)` | NumPy array or `None` | Same, waiting for an updated frame |
| `next()` | NumPy array or `None` | On-demand + packed CPU; use `next_frame()` for GPU/NATIVE |
| `read_tensor()`, `read_new_tensor()` | PyTorch tensor or `None` | DLPack for supported packed CPU/CUDA; rejects OpenCL |
| `read_dlpack()`, `read_new_dlpack()` | `Frame` or `None` | Get a `Frame` supporting DLPack for appropriate packed types |

## Lifecycle

```python
from framepipe import Reader

with Reader("video.mp4", name="demo") as reader:
    while reader.is_opened():
        frame = reader.read_new_frame()
        if frame is not None:
            print(frame.id)
# The context manager calls stop().
```

- `start()` explicitly starts the worker; useful with `auto_start=False`. Starting an already running worker raises an error.
- `stop()` / `release()` stop the Reader. Repeated `stop()` calls through the Python wrapper are safe. After stopping, reading through `._reader` raises `ReaderStopped`; some wrapper properties return placeholder values.
- `is_opened()` indicates that the Reader has not been stopped and is not in `STOPPED`. It does **not** guarantee that a valid frame has been published.
- `is_alive()` checks whether a frame has been published recently, using `degraded_timeout_sec`.
- `warmuped` is true once at least one frame has been published (`frames_processed > 0`).

The `State` values are `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, and `STOPPED`. A Python subclass/wrapper can override `._on_state_change(name, old, new)` for state-change logging. The underlying C++ class has a public `set_state_callback()`, but `WrappedReader` does not expose it as a separate public Python method.

## Properties

- `frame_pix_fmt`: pixel format of the most recent result, not necessarily the source's input format.
- `hwaccel_active`: actual activated FFmpeg hardware decoder after session startup; it may report `SOFTWARE` before startup.
- `frames_processed`, `packets_processed`: counters of published frames and processed packets.
- `last_pts_seconds`: most recent frame's PTS in seconds.
- `last_recv_time`: monotonic timestamp of the last publication, **not** a Unix timestamp.
- `state`, `name`: current state and configured name.

More on [memory and frame objects](memory-and-frames.md) and [architecture](architecture.md).
