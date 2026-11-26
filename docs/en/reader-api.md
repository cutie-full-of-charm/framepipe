# Python API: `Reader`

> Version at the time of writing: `0.4.2.dev4`.  
> This page describes the Python `framepipe.Reader` wrapper (`framepipe/reader.py`).

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
| `name` | required | Reader name, used in logs |
| `verbose` | `False` | More detailed logging |
| `videostream_id` | `0` | Ordinal position of the video stream in the container (not the index among all streams) |
| `reconnect_delay_sec` | `1.0` | Delay before starting a new session after an error in background mode |
| `degraded_timeout_sec` | `3.0` | Age threshold of the latest frame for `is_alive()` |
| `av_container_options` | `None` | Dictionary of FFmpeg demuxer/container options; the C++ argument is called `av_options` |
| `out_format` | `OutputFormat.BGR24` | BGR24, RGB24, GRAY8, NATIVE |
| `hwaccel` | `HWAccel.SOFTWARE` | Where decoding happens |
| `postprocessor` | `PostProcessor.CPU` | How frames are processed |
| `output_memory` | `MemoryType.CPU` | Where results are placed |
| `out_w`, `out_h` | `0`, `0` | Target dimensions if both are positive |
| `scale` | `1.0` | Dimension multiplier if both target dimensions are not specified |
| `realtime_file` | `False` | Pace file playback according to PTS (naive 1 ms polling) |
| `auto_start` | `True` | Start the worker thread automatically |
| `on_demand` | `False` | Retrieve file frames one at a time using `next_frame()` |

Live-source detection is based on URL prefixes: `rtsp://`, `rtmp://`, `rtmps://`, `udp://`, `srt://`, `mms://`. For recognized live sources, a set of low-latency FFmpeg options is enabled (`rtsp_transport=tcp`, `fflags=nobuffer`, `flags=low_delay`, `max_delay=0`, `analyzeduration=0`, `probesize=32`); user-supplied `av_container_options` take precedence for the same keys.

## Frame retrieval methods

`Reader` separates **how** you retrieve a frame (immediately / only if new / wait for a new one / request the next one) from **how it is represented**: a `numpy.ndarray` through `read*`, or a `Frame` object through methods ending in `_frame`.

- `read()` / `read_frame()`, `read_new()` / `read_new_frame()`, and `read_latest(timeout)` / `read_latest_frame(timeout=1.0)` operate on the latest published frame.
- `next()` / `next_frame()` request the next frame in `on_demand=True` mode (file sources only).
- `read_tensor()` / `read_new_tensor()` use PyTorch and DLPack; `read_dlpack()` / `read_new_dlpack()` return a `Frame`, not a ready-made capsule.

For a detailed table of return types, blocking behavior, frame drops, and examples, see [reading-models.md](reading-models.md). For methods available on specific frame classes, see [memory-and-frames.md](memory-and-frames.md).

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

- `start()` -- explicitly starts the worker thread; useful with `auto_start=False`. Calling it again when the thread is already running raises an error.
- `stop()` / `release()` -- stop the Reader. Calling `stop()` multiple times on the Python wrapper is safe. After `stop()`, read methods accessing `._reader` raise `ReaderStopped`, while some properties return placeholder values.
- `is_opened()` -- the Reader has not stopped and is not in `STOPPED`. This does not promise that a valid frame has already been received.
- `is_alive()` -- whether a frame has been published recently relative to `degraded_timeout_sec`.
- `warmuped` -- at least one frame has been published (`frames_processed > 0`).
- `get_last_exception()` -- returns the last stored terminal error (`RuntimeError` or `None`) without raising it. The getter is unavailable via `WrappedReader` after `stop()`, because the wrapper clears `_impl`.

`State`: `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, `STOPPED`. A Python subclass/wrapper can override `._on_state_change(name, old, new)` to handle state-change logging; the C++ class exposes a public `set_state_callback()` method, but `WrappedReader` does not expose it as a separate method.

## Properties

- `frame_pix_fmt`: pix_fmt of the most recent result, not necessarily the source file's format.
- `hwaccel_active`: name of the actually activated FFmpeg hardware acceleration backend after the session starts; before that, it may be `SOFTWARE`.
- `frames_processed`, `packets_processed`: counters (published frames and processed packets).
- `last_pts_seconds`: PTS of the current frame in seconds.
- `last_recv_time`: monotonic time of the last publication, not a Unix timestamp.
- `state`, `name` -- current state and name.
