# Decoding backends (`HWAccel`)

> Version at the time of writing: `0.4.2.dev4`.  
> Decoding is implemented with FFmpeg `libav*`; the availability of a particular codec and hardware profile depends on the FFmpeg build and driver.  
> Most of the testing effort has focused on VAAPI.  
> D3D11VA has barely been tested.

`hwaccel` selects only where decoding happens, not how pixels are converted or where the result is stored. The complete parameter set is `hwaccel`, `postprocessor`, `output_memory`, `out_format`.  
Compatibility matrix: [compatibility.md](compatibility.md).

## `HWAccel.SOFTWARE`

- Platform: wherever the FFmpeg build in use can run.
- Decoded frame format: software `AVFrame`; the decoder chooses the format (`yuv420p`, `nv12`, and other FFmpeg formats).
- Postprocessing: `PostProcessor.CPU`.
- Output memory: `MemoryType.CPU`.
- Output formats: `BGR24`/`RGB24`/`GRAY8` via swscale, or `NATIVE` without resizing.
- Color: CPU conversion through FFmpeg/libswscale, without explicit color-matrix configuration.

Typical path: `SOFTWARE → CPU → CPU`. This is the default Python `Reader` configuration (with BGR24 as the default Python output format).

## `HWAccel.VAAPI`

- Platform: Linux; VAAPI decoding is explicitly prohibited on Windows.
- Hardware pix_fmt: `vaapi`. The actual underlying software format of the surface is determined by the decoder/driver; it should not be confused with `frame.pix_fmt == "vaapi"` on a GPU surface.
- Requirement: `av_hwdevice_ctx_create` currently uses the fixed path `/dev/dri/renderD128`, which may not work on your system.
- Driver: when `LIBVA_DRIVERS_PATH` is unset, the program tries to find drivers in `/usr/lib/x86_64-linux-gnu/dri`, `/usr/lib64/dri`, `/usr/lib/dri`, `/usr/local/lib/dri`.
- Postprocessing: `CPU`, `VAAPI`, `OPENCL` (the last one only with VAAPI NV12 and successful interop).
- Output memory: CPU or GPU depending on the postprocessor; `VAAPI + GPU` is supported only with `NATIVE`.
- Output formats: `BGR24`/`RGB24`/`GRAY8`/`NATIVE`, subject to postprocessor-specific limitations.

VAAPI is the most thoroughly developed backend in the current implementation. For GPU->GPU, you can keep the surface as a `VaapiFrame(surface_id)`; for CPU->NumPy, download and conversion are available. OpenCL can process NV12 without an intermediate CPU conversion if cross-backend memory mapping is supported.

The code performs `vaSyncSurface` before processing. Individual corrupted surfaces may be skipped without throwing an exception or closing the session; after 25 decoding errors, the session attempts recovery. This does not imply recovery from every driver error.

## `HWAccel.CUDA`

- Platform: requires a CUDA-capable configuration, an NVIDIA driver, and the CUDA runtime.
- Hardware pix_fmt: `cuda`; the underlying software format is determined by FFmpeg.
- Postprocessing: `CPU` (download hardware frame) or `CUDA` (GPU conversion/resize using a .cu CUDA kernel).
- Output memory: RAM for the CPU postprocessor; RAM or GPU (DLPack) for the CUDA postprocessor.
- Formats: the CPU path depends on swscale and download support. The CUDA postprocessor explicitly requires 8-bit NV12 and outputs `BGR24`, `RGB24`, `GRAY8` (but not `NATIVE`).
- Color in the CUDA postprocessor: fixed BT.709 limited. See [pixel-formats-and-color.md](pixel-formats-and-color.md) for details and limitations.

The CUDA postprocessor is contained in a separate dynamic library, `_framepipe_cuda.so` (`.pyd` on Windows), loaded on demand. The [06_zero_copy_inference](../../examples/06_zero_copy_inference.py) example uses GPU output and DLPack with PyTorch.

Selecting `HWAccel.CUDA` does not mean that any given stream can necessarily be decoded using CUDA: before starting, the code checks whether FFmpeg exposes a hardware configuration for the required codec. If not, initialization fails; there is no software fallback.

## `HWAccel.D3D11VA` -- very rough and experimental

- Platform: the `D3D11VA` branch selects D3D11 only on Windows; the selection is rejected on Linux.
- Hardware pix_fmt: determined by the FFmpeg configuration, usually D3D11.
- Postprocessing: only `PostProcessor.CPU`.
- Output memory: only `MemoryType.CPU`.
- Output formats: `BGR24`, `RGB24`, `GRAY8`. `NATIVE` only without resizing, and even then with caveats.
- Path: D3D11 hardware frame -> `av_hwframe_transfer_data()` -> CPU FFmpeg swscale / native.

## General limitations

1. If a hardware decoder returns a software frame instead of the expected hardware pix_fmt, this is treated as an error.
2. Choosing an `HWAccel` does not determine the color conversion matrix: see [pixel-formats-and-color.md](pixel-formats-and-color.md).
3. A decoder/driver/format combination tested on one machine should not automatically be considered tested on another.

## Selection and diagnostics example

```python
from framepipe import HWAccel, Reader

with Reader("video.mp4", name="hw-demo", hwaccel=HWAccel.VAAPI) as reader:
    while reader.is_opened():
        frame = reader.read_new_frame()
        if frame is not None:
            print(reader.hwaccel_active, frame.pix_fmt, frame.memory_type)
```

`hwaccel_active` reports the name of the active FFmpeg hardware device after the session has opened; also check the logs and driver/FFmpeg logs alongside `frame.pix_fmt`, rather than relying on `hwaccel_requested` alone. If the reader cannot reach `STREAMING` on a test file, the configuration is very likely unsupported or incorrect (`0.4.2.dev4`).
