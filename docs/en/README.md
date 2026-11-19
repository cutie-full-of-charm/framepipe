# framepipe documentation

> API version: **`0.4.2.dev4` (development)**. These pages document the current implementation and known limitations. Additional formats and backends are planned.

| File | Contents |
| --- | --- |
| [architecture.md](architecture.md) | Why decoding, postprocessing, and output memory are separate choices |
| [compatibility.md](compatibility.md) | Allowed combinations and output formats |
| [hwaccels.md](hwaccels.md) | `HWAccel.SOFTWARE / CUDA / VAAPI / D3D11VA` and decoding restrictions |
| [postprocessors.md](postprocessors.md) | `PostProcessor.CPU / CUDA / VAAPI / OPENCL`, resizing, and pixel conversion |
| [memory-and-frames.md](memory-and-frames.md) | `MemoryType`, frame subclasses, NumPy, DLPack, kDLOpenCL, and VAAPI surfaces |
| [pixel-formats-and-color.md](pixel-formats-and-color.md) | Pixel formats, color matrices, color spaces, and range handling |
| [reading-models.md](reading-models.md) | Latest-drop, `read` / `read_new` / `read_latest`, `realtime_file`, `on_demand`, and `next*` |
| [reader-api.md](reader-api.md) | Python Reader API |
| [support-status.md](support-status.md) | Implementation confidence, testing status, and priorities; contributions welcome |

## Components at a glance

- `HWAccel` selects where the source frame is decoded.
- `PostProcessor` selects how the frame is processed and converted.
- `MemoryType` selects where the result resides.

Pixel formats and color matrices cut across all three layers, so they have their own page.

**Terminology:**

- **Implemented** means the corresponding path appears to be implemented in the source code.
- **Allowed** means the code does not reject the combination up front; runtime failure is still possible.
- **Tested** means it worked in the maintainer's test environment; that does not guarantee it will work on yours.

[Back to the project README](../../README.md) · [Русская документация](../ru/README.md)
