# framepipe Documentation

> Documented API version: `0.4.2.dev4` (dev). These docs reflect the current code and known limitations. More formats and backends are planned.

| File | Contents |
| --- | --- |
| [architecture.md](architecture.md) | Why decoding, postprocessing, and memory parameters are independent |
| [compatibility.md](compatibility.md) | Compatibility matrix for supported combinations and output formats |
| [hwaccels.md](hwaccels.md) | `HWAccel.SOFTWARE / CUDA / VAAPI / D3D11VA` and decoding limitations |
| [postprocessors.md](postprocessors.md) | `PostProcessor.CPU / CUDA / VAAPI / OPENCL`: what they do, how they resize and convert pixel formats |
| [memory-and-frames.md](memory-and-frames.md) | `MemoryType`, concrete frame classes, NumPy, DLPack, kDLOpenCL, VAAPI surfaces |
| [pixel-formats-and-color.md](pixel-formats-and-color.md) | All about color matrices, color spaces, etc. |
| [reading-models.md](reading-models.md) | Reading model: latest-drop, read/read_new/read_latest, `realtime_file`, `on_demand`, and `next*` |
| [reader-api.md](reader-api.md) | Description of the Python API |

## Components at a glance

- `HWAccel` determines where the source frame is decoded.
- `PostProcessor` determines how processing and conversion are performed.
- `OutputFormat` determines the color representation of the output frames.
- `MemoryType` determines where the result is stored.

Pixel formats and color matrices span all three stages, so they are documented separately.
