# framepipe 

**Russian:** [README.ru.md](README.ru.md) | [Русская документация](docs/ru/README.md)

**framepipe** is a Python library for decoding and preparing video frames from files and network streams. It uses FFmpeg through a C++ binding and provides CPU arrays or supported GPU-backed frame objects.

Originally built for **real-time ML/CV and edge inference**, framepipe now also supports more general video pipelines. Its most developed usage pattern remains **read-latest-drop**: a background decoder publishes the newest frame without building up a queue for a slow consumer. For sequential file processing, use `on_demand=True` instead.

> **Documentation version: `0.4.2.dev4` (development).** The Reader API and supported combinations may change. VAAPI has received the most attention in development and testing; D3D11VA is experimental and has barely been tested. Prebuilt wheels are currently planned for **manylinux only**.

## What it provides

- Decoding via `HWAccel.SOFTWARE`, `CUDA`, `VAAPI`, or experimental `D3D11VA`.
- `PostProcessor.CPU`, `CUDA`, `VAAPI`, or `OPENCL` for supported conversion and resizing paths.
- `OutputFormat.BGR24`, `RGB24`, `GRAY8`, or `NATIVE`, with `MemoryType.CPU` / `GPU` where supported.
- NumPy arrays for packed CPU output; typed frame objects, DLPack, and native GPU handles on supported paths.
- Latest-frame background reading and on-demand sequential file reading.

**Support depends on the entire decoder -> postprocessor -> format -> memory combination**, not just on individual enum values. See the [compatibility matrix](docs/en/compatibility.md) before selecting a GPU path.

## Quick start

Package is available via PyPi
```sh
uv add framepipe
# or
pip install framepipe
# or you can download .whl from Releases page 
```

Sequential processing of a file with the default CPU/BGR output:

```python
from framepipe import Reader

with Reader("video.mp4", name="demo", on_demand=True) as reader:
    while (image := reader.next()) is not None:
        print(image.shape)  # NumPy H x W x 3
```

For live inference, `read_latest()` waits for the newest available frame and does not build up an output queue:

```python
from framepipe import Reader

with Reader("rtsp://camera.example/live", name="camera") as reader:
    while reader.is_opened():
        image = reader.read_latest(timeout=1.0)
        if image is not None:
            print(image.shape)
```

For GPU or `NATIVE` output, prefer the methods ending in `_frame`, such as `read_latest_frame()` and `next_frame()`. Details: [reading models](docs/en/reading-models.md) and [Reader API](docs/en/reader-api.md).

## Documentation

| Topic | Page |
| --- | --- |
| Overview and pipeline | [Architecture](docs/en/architecture.md) |
| Supported combinations | [Compatibility matrix](docs/en/compatibility.md) |
| Decoding | [HWAccel backends](docs/en/hwaccels.md) |
| Conversion and resizing | [Postprocessors](docs/en/postprocessors.md) |
| Frame storage and access | [Memory types and frames](docs/en/memory-and-frames.md) |
| Format and color handling | [Pixel formats and color](docs/en/pixel-formats-and-color.md) |
| `read` / `read_new` / `read_latest` / `on_demand` | [Reading models](docs/en/reading-models.md) |
| Reader methods and options | [Reader API](docs/en/reader-api.md) |

This is a **development snapshot**, not a claim of universal hardware or codec compatibility. More formats, improved color handling, broader platform testing, and API refinements are planned.

## Лицензия
Code is licensed under MPL 2.0 (see `LICENSE`).
