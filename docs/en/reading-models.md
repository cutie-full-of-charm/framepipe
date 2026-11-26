# Frame reading and delivery models

> Version at the time of writing: `0.4.2.dev4`.  
> This page describes the actual semantics of the public Python `framepipe.Reader` wrapper and the `src/reader.cpp` implementation in this version. The API may change in the future.

## Two independent dimensions: retrieval and representation

There are two things to distinguish:

1. Delivery model: read the current slot (`read`), read it only when it changes (`read_new`), wait for an update (`read_latest`), or request decoding of the next frame (`on_demand` + `next`).
2. Result representation: get a `numpy.ndarray` (`read*` without `_frame`) or a `Frame` object (`read*_frame`). Choosing `Frame` does not change the delivery model.

In normal mode (`on_demand=False`), a producer runs in a separate thread. After decoding/postprocessing, it writes a `Frame` into the single current slot, increments `frame.id`, and wakes waiting consumers. The API does not expose a separate queue of published frames.

```text
Decoding thread / producer:       F1 ── F2 ── F3 ── F4 ── F5
                                    │    │    │    │    │
Published slot:                  [F1] [F2] [F3] [F4] [F5]
                                    ^         ^         ^
Slow consumer:                    read      read      read
                                    F1        F3        F5

F2 and F4 were decoded and published, but the consumer did not retrieve them
```

This is latest/drop at the delivery level -- the old slot value is replaced by the new one. The source rate, decoder, postprocessor, GPU synchronization, and Python processing time all matter.

## Summary of methods

| Goal | NumPy (packed CPU only) | `Frame` object | Blocking? | Can frames be skipped between calls? |
| --- | --- | --- | --- | --- |
| Read the current frame | `read()` | `read_frame()` | No | Yes; the same frame may be returned repeatedly |
| Read only if there's a new frame | `read_new()` | `read_new_frame()` | No | Yes; repeated `id` values are filtered out |
| Wait for a new frame | `read_latest(timeout)` | `read_latest_frame(timeout=1.0)` | Yes, for at most `timeout` seconds | Yes; returns the latest publication, not a queue |
| Request the next frame (file) | `next()` | `next_frame()` | Yes, until frame/EOF/error/stop | Does not skip frames *because the consumer is slow* |

In any of the first three rows, the result may be `None` if no suitable frame exists. For `read_latest*`, this also happens on timeout, stop, or completion of on-demand reading. For `next*`, `None` means EOF/stop with no new frame. In practice, `read_frame()` can also return `None` before the first publication, even though the Python wrapper's annotation does not reflect this.

NumPy API limitation: `read()`, `read_new()`, and `read_latest()` require `output_memory=MemoryType.CPU` and an `out_format` of `BGR24`, `RGB24`, or `GRAY8`. For GPU or `NATIVE` output, use the `*_frame()` methods. The Python wrapper's `next()` does not check the format beforehand, but the C++ side still requires a packed CPU frame for NumPy conversion.

### `read()` and `read_frame()` -- snapshot of the current slot

Return the most recent available result immediately. They do not wait for the first frame and do not mark the frame as "read" for `read_new*`/`read_latest*`.

```python
from framepipe import Reader

with Reader("video.mp4", name="snapshot", realtime_file=True) as reader:
    frame = reader.read_frame()  # Frame or None; repeated calls may yield the same id
    if frame is not None:
        print(frame.id, frame.width, frame.height)
```

Useful for rendering in your own loop, monitoring, and the "show me whatever is available right now" case. With frequent polling, the same frame may be returned many times. There is no wait for a new publication here.

### `read_new()` and `read_new_frame()` -- non-blocking latest, without duplicates

Return the current slot only if its `id` differs from the last one delivered through `read_new*` or `read_latest*`; otherwise they return `None`. The background producer does not slow down or put anything in a queue, so frames published quickly between two calls will be skipped by the consumer.

```python
from framepipe import Reader

with Reader("video.mp4", name="preview", realtime_file=True) as reader:
    while reader.is_opened():
        frame = reader.read_new()  # ndarray or None; no waiting
        if frame is None:
            continue
        # Display the frame or pass it to a handler
```

If the loop is busy with expensive processing, this is usually desirable for real-time ML/CV: there is no queue of old frames building up. However, the loop shown above actively polls the Reader and can consume CPU while no frames are being published. In application code, `read_latest*` is often preferable.

### `read_latest(timeout)` and `read_latest_frame(timeout)` -- wait for a new latest frame

The behavior is close to `read_new*`, but instead of immediately returning `None` when there is no update, the call waits for a new publication or for the specified time to expire. If a new, unread `id` is already available, it returns immediately. `timeout` is in seconds and must be non-negative. `timeout=0` turns the call into a non-blocking check.

```python
from framepipe import Reader

with Reader("rtsp://camera.example/live", name="detector") as reader:
    while reader.is_opened():
        frame = reader.read_latest_frame(timeout=1.0)
        if frame is None:
            # Timeout or stop: this is not an "empty video frame".
            continue
        # inference(frame): process the current frame, not a queue of stale ones
```

This waits for an update to the current slot; it is not an API for reading every frame accumulated during processing. The consumer can still skip published frames between calls. In the Python wrapper, `read_latest` (NumPy) requires an explicit `timeout`; `read_latest_frame` has a default of `1.0`.

Shared cursor: `read_new`, `read_new_frame`, `read_latest`, and `read_latest_frame` all use the same `_last_read_id` for a given Reader in C++. If one consumer has already obtained `id=42` via `read_latest_frame()`, the next `read_new()` will not return that `id` again. These are not four independent subscribers. Multiple concurrent consumers of a single Reader also share this cursor rather than each receiving the same sequence.

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
            print("Skipped publications:", frame.id - previous_id - 1)
        previous_id = frame.id
        # Relatively slow ML/CV processing may happen here.
        # No backlog of previously published frames accumulates.
```

The difference between IDs counts Reader publications missed by the consumer, not necessarily dropped frames from the source stream: `Frame.id` is assigned when an already processed result is published. Measuring end-to-end latency requires separate measurements accounting for PTS, arrival time, source buffering, and consumer speed.

## Files: `realtime_file=False` and `realtime_file=True`

Both modes use the same background latest slot when `on_demand=False`. `realtime_file` changes the producer's pace, not the semantics of `read*`.

| Parameter | File processing mode | What the consumer sees |
| --- | --- | --- |
| `realtime_file=False` (default) | Decode without artificial PTS-based waiting, as fast as the pipeline allows | A slow consumer may see only the last few frames, or even a single frame; not suitable for a complete pass through a file |
| `realtime_file=True` | A delay is inserted between frames based on PTS relative to the first frame | Closer to playback at the source rate, but `read*` still sees a single overwritten slot and may skip publications |

The current PTS pacing implementation is naive: it polls with `sleep_for(1 ms)`, uses a monotonic clock, and skips waiting when PTS is absent, the time base is invalid, or playback is already behind schedule. This is not accurate audio/video synchronization, nor does it guarantee stable FPS. For sources recognized as live, `realtime_file` pacing is not applied at all.

## `on_demand=True` -- sequential pull for files

This is a separate producer mode: the worker thread starts but waits for a request from `next_frame()` / `next()`, then delivers one successfully processed frame. No new request arrives until the consumer calls `next*` again. The call blocks until a result, EOF, or an error (and may also finish after `stop()`). Reading packets and decoding may require multiple packets for one output frame, and corrupted VAAPI frames may be skipped by the recovery mechanism.

```python
from framepipe import Reader

with Reader("video.mp4", name="offline", auto_start=False, on_demand=True) as reader:
    reader.start()  # required when auto_start=False
    while True:
        frame = reader.next_frame()  # Frame / None (EOF)
        if frame is None:
            break
        # Process the current frame before requesting the next one
        print(frame.id, frame.pts)
```

- Only for sources that Reader classifies as FILE. With a URL recognized as live, the constructor raises an exception.
- `on_demand=True` is incompatible with `realtime_file=True`. It does not need an artificial PTS delay: the consumer determines the pace.
- `next_frame()` supports CPU/GPU/NATIVE. `next()` returns NumPy only for packed CPU output.
- With `auto_start=True` (the default), the constructor starts the thread, which then waits for the first request; there is no need to call `reader.start()` again.
- At EOF, `next*` returns `None`. Errors inside the on-demand session propagate to the calling code rather than triggering endless reconnect attempts.
- This mode does not guarantee that every frame from the source codec survives decoding errors or corrupted surfaces. Its purpose is sequential delivery of results without dropping them merely because the Python consumer is slow.

`on_demand` is used by [06_zero_copy_inference](../../examples/06_zero_copy_inference.py): the next frame is requested after inference on the previous one.

> Do not substitute `read_latest*` for `next*` in `on_demand` mode: `read_latest*` does not request decoding; it only waits for the already published slot to change. To advance through the file, call `next_frame()` or `next()`.

## Choosing a model

| Goal | Choice | Why |
| --- | --- | --- |
| Camera / edge inference where freshness matters most | `read_latest_frame(timeout)` | Convenient blocking latest-drop, with no queue of stale frames |
| GUI or periodic check of the available image | `read()` / `read_frame()` | Immediate snapshot of the latest state |
| Custom non-blocking processing loop | `read_new()` / `read_new_frame()` | No duplicates; returns `None` immediately if there is no update |
| Video file played at an approximately real-time rate for a demo | `realtime_file=True` + `read_new()`/`read_latest()` | File is paced roughly by PTS, but still uses latest-drop |
| Offline inference/conversion, processing frame by frame | `on_demand=True` + `next_frame()` | Consumer sets the pace; unread results are not overwritten |
| CUDA / OpenCL / VAAPI surfaces, `NATIVE` | Methods ending in `_frame` | Return typed storage instead of forcing NumPy |
