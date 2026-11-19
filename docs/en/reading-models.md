# Reading modes and frame delivery

> Documented version: `0.4.2.dev4`. This page describes the behavior of the public `framepipe.Reader` Python wrapper and `src/reader.cpp` in this version. The API may change.

## Two independent decisions: delivery and representation

Keep these aspects separate:

1. **Delivery mode:** inspect the current slot (`read`), return it only if updated (`read_new`), wait for an update (`read_latest`), or explicitly request the next decoded frame (`on_demand` + `next`).
2. **Result representation:** receive a `numpy.ndarray` (`read*` methods without `_frame`) or a typed `Frame` (`read*_frame`). Choosing `Frame` does not change delivery semantics.

In ordinary background mode (`on_demand=False`), a producer runs in another thread. After decoding/postprocessing it stores the `Frame` in one current-frame slot, increases `frame.id`, and wakes any waiting consumers. The public API does **not** expose a queue of published frames.

```text
Decoder thread / producer:          F1 ── F2 ── F3 ── F4 ── F5
                                      │    │    │    │    │
Published slot:                    [F1] [F2] [F3] [F4] [F5]
                                      ^         ^         ^
Slow consumer:                     read      read      read
                                      F1        F3        F5

F2 and F4 were decoded and published but never retrieved by the consumer.
```

This is **latest / drop at the delivery layer**: a newer frame replaces the older slot value. Source rate, decoder speed, postprocessing, GPU synchronization, and Python-side processing all affect the behavior.

## Method overview

| Intent | NumPy (packed CPU only) | `Frame` object | Blocking? | May skip published frames? |
| --- | --- | --- | --- | --- |
| Read the current frame | `read()` | `read_frame()` | No | Yes; the same frame can be returned repeatedly |
| Read only if updated | `read_new()` | `read_new_frame()` | No | Yes; duplicate `id` values are filtered |
| Wait for a new frame | `read_latest(timeout)` | `read_latest_frame(timeout=1.0)` | Yes, up to `timeout` seconds | Yes; returns the latest publication, not a queue |
| Request the next file frame | `next()` | `next_frame()` | Yes, until a frame, EOF, error, or stop | Not **because of a slow consumer** |

The first three operations can return `None` if no suitable frame is available. `read_latest*` can also return `None` after a timeout, shutdown, or on-demand termination. For `next*`, `None` indicates EOF or stopping before another frame could be obtained. In practice, `read_frame()` may return `None` before the first publication, although the Python wrapper's annotation does not reflect this.

**NumPy API restriction:** `read()`, `read_new()`, and `read_latest()` require `output_memory=MemoryType.CPU` and `out_format` equal to `BGR24`, `RGB24`, or `GRAY8`. For GPU or `NATIVE`, call the `*_frame()` methods. The Python `next()` wrapper does not precheck the selected format, but NumPy conversion in C++ still requires a packed CPU result.

### `read()` / `read_frame()`: snapshot of the current slot

These return the latest available result immediately. They do not wait for the first frame and do **not** mark the frame as consumed for the `read_new*` / `read_latest*` methods.

```python
from framepipe import Reader

with Reader("video.mp4", name="snapshot", realtime_file=True) as reader:
    frame = reader.read_frame()  # Frame or None; calls may return the same id
    if frame is not None:
        print(frame.id, frame.width, frame.height)
```

Useful for rendering in your own loop, monitoring, or “show me the latest available image.” Fast polling can return the same frame many times; this call does not wait for a new publication.

### `read_new()` / `read_new_frame()`: non-blocking latest, without duplicates

The current slot is returned only if its `id` differs from the last frame returned by `read_new*` or `read_latest*`; otherwise the result is `None`. The producer does not slow down or maintain a pending-frame queue, so a fast sequence of publications between two reads can be missed by the consumer.

```python
from framepipe import Reader

with Reader("video.mp4", name="preview", realtime_file=True) as reader:
    while reader.is_opened():
        frame = reader.read_new()  # ndarray or None; never waits
        if frame is None:
            continue
        # Display the frame or pass it to another processor.
```

This is usually desirable for real-time ML/CV when processing is slower than the source: no queue of stale frames accumulates. The tight polling loop shown here can consume CPU while there are no new publications. In applications, `read_latest*` is often preferable.

### `read_latest(timeout)` / `read_latest_frame(timeout)`: wait for a new latest frame

This behaves like `read_new*`, except that if there is no update it waits for the next publication, up to the specified timeout. If a new, unread `id` is already available, it returns immediately. `timeout` is in seconds and must be nonnegative; `timeout=0` makes the call non-blocking.

```python
from framepipe import Reader

with Reader("rtsp://camera.example/live", name="detector") as reader:
    while reader.is_opened():
        frame = reader.read_latest_frame(timeout=1.0)
        if frame is None:
            # Timeout or shutdown, not an empty video frame.
            continue
        # Run inference on the freshest result, not on a backlog.
```

This waits for a change to the **current slot**; it is not an API for retrieving every frame accumulated during processing. A consumer can still miss intermediate publications. The NumPy `read_latest(timeout)` requires an explicit `timeout` in the Python wrapper; `read_latest_frame()` defaults to `1.0`.

**Shared read cursor:** `read_new`, `read_new_frame`, `read_latest`, and `read_latest_frame` share one C++ `_last_read_id` per Reader. If a consumer retrieves `id=42` with `read_latest_frame()`, the next `read_new()` does not return `id=42` again. These are not four independent subscriptions. Multiple concurrent consumers of one Reader also share this cursor, rather than each receiving an identical stream of results.

### Example: read-latest-drop for an inference loop

```python
from framepipe import Reader

with Reader("rtsp://camera.example/live", name="edge-inference") as reader:
    previous_id = None
    while reader.is_opened():
        frame = reader.read_latest_frame(timeout=0.5)
        if frame is None:
            continue
        if previous_id is not None and frame.id > previous_id + 1:
            print("Missed publications:", frame.id - previous_id - 1)
        previous_id = frame.id
        # ML/CV processing may take significantly longer than a frame interval.
        # No queue of already published frames accumulates here.
```

Differences between IDs represent missed **Reader publications**, not necessarily lost source-stream frames: `Frame.id` is assigned to the processed result when it is published. Measuring end-to-end latency requires separate instrumentation that accounts for PTS, arrival time, source buffering, and downstream processing.

## File sources: `realtime_file=False` and `realtime_file=True`

Both options use the same latest-frame slot when `on_demand=False`. The `realtime_file` option changes the producer's **pace**, not the semantics of `read*`.

| Setting | File-processing behavior | What the consumer sees |
| --- | --- | --- |
| `realtime_file=False` (default) | Decode as fast as the pipeline allows, without artificial PTS-based delays | A slow consumer may see only the last few frames, or even one frame; do not use this for a complete sequential file pass |
| `realtime_file=True` | Wait between frames using PTS relative to the first frame | Closer to original playback speed, but `read*` still accesses one replaceable slot and may miss publications |

PTS pacing is currently basic: it polls with `sleep_for(1 ms)`, uses a monotonic clock, and skips waiting if timestamps/time base are unavailable or playback is already behind schedule. This is **not** precise A/V synchronization or a stable-FPS guarantee. For sources classified as live, the `realtime_file` pacing logic is not applied.

## `on_demand=True`: sequential pull for files

This is a separate producer mode. The worker starts, then waits for a request from `next_frame()` / `next()`. Each request delivers one successfully processed frame; the next request is not made until the consumer calls `next*` again. The call blocks until a result, EOF, an error, or shutdown. Producing a frame can require reading multiple packets, and the VAAPI recovery path may skip damaged frames.

```python
from framepipe import Reader

with Reader("video.mp4", name="offline", auto_start=False, on_demand=True) as reader:
    reader.start()  # Required when auto_start=False.
    while True:
        frame = reader.next_frame()  # Frame / None at EOF
        if frame is None:
            break
        # Finish processing this frame before requesting the next one.
        print(frame.id, frame.pts)
```

- Only for sources classified by Reader as `FILE`. Passing a recognized live URL raises an exception in the constructor.
- Incompatible with `realtime_file=True`. There is no need to wait for PTS intervals: the consumer controls the pace.
- `next_frame()` supports CPU/GPU/NATIVE output; `next()` returns NumPy only for packed CPU output.
- With the default `auto_start=True`, the constructor starts the worker and it waits for the first request; do **not** call `reader.start()` again.
- At EOF, `next*` returns `None`. Errors in an on-demand session propagate to the caller instead of triggering indefinite background reconnection.
- This mode does not guarantee recovery of every source frame in the presence of decode errors or corrupted hardware surfaces. It avoids frame loss **caused solely by a slower Python consumer**.

The repository's `examples/06_zero_copy_inference.py` uses on-demand reading for CUDA + PyTorch: the next result is requested only after inference on the previous frame completes.

> Do not substitute `read_latest*` for `next*` in on-demand mode: `read_latest*` waits for an already published frame to change but does **not** request decoding. Call `next_frame()` or `next()` to advance the file.

## Choosing a reading model

| Goal | Recommended API | Reason |
| --- | --- | --- |
| Camera / edge inference prioritizing freshness | `read_latest_frame(timeout)` | Blocking latest-drop, no stale-frame output queue |
| GUI or periodic inspection of the current image | `read()` / `read_frame()` | Immediate snapshot of the latest slot |
| Custom non-blocking processing loop | `read_new()` / `read_new_frame()` | No duplicates; `None` immediately when unchanged |
| Demonstrate a file as a real-time stream | `realtime_file=True` + `read_new()` / `read_latest()` | Approximate PTS playback, still latest-drop |
| Offline inference or conversion, frame by frame | `on_demand=True` + `next_frame()` | Consumer sets the pace; no replacement of unread results |
| CUDA / OpenCL / VAAPI surfaces or `NATIVE` | Methods ending in `_frame` | Return a typed frame rather than forcing NumPy |
