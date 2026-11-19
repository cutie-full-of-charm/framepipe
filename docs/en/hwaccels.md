# Decoding backends (`HWAccel`)

> Documented version: `0.4.2.dev4`. Decoding is implemented using FFmpeg `libav*`. Availability of individual codecs and hardware profiles depends on the FFmpeg build and GPU driver. **VAAPI has been the main testing focus; D3D11VA has barely been tested.**

`hwaccel` selects **where decoding happens**. It does not choose the pixel-conversion method or where the output is stored. The full set of choices is `hwaccel`, `postprocessor`, `output_memory`, and `out_format`.

See the [compatibility matrix](compatibility.md).

## `HWAccel.SOFTWARE`

- **Platform:** wherever the bundled FFmpeg build works.
- **Decoded frame:** software `AVFrame` with a decoder-selected pixel format (`yuv420p`, `nv12`, or another FFmpeg format).
- **Postprocessor:** `PostProcessor.CPU`.
- **Output memory:** `MemoryType.CPU`.
- **Output formats:** `BGR24`, `RGB24`, and `GRAY8` through swscale; `NATIVE` without resizing.
- **Color handling:** FFmpeg/libswscale conversion, without explicit color-matrix configuration in framepipe.

Typical path: `SOFTWARE → CPU → CPU`. This is the default Python `Reader` configuration; the default output format is BGR24.

## `HWAccel.VAAPI`

- **Platform:** Linux; VAAPI decoding is explicitly rejected on Windows.
- **Hardware pixel format:** `vaapi`. The underlying software format of a surface depends on the decoder/driver. Do not conflate it with `frame.pix_fmt == "vaapi"` for a GPU surface.
- **Device:** `av_hwdevice_ctx_create` currently uses the hard-coded path `/dev/dri/renderD128`, which might not exist on your system.
- **Driver lookup:** if `LIBVA_DRIVERS_PATH` is unset, the program searches `/usr/lib/x86_64-linux-gnu/dri`, `/usr/lib64/dri`, `/usr/lib/dri`, and `/usr/local/lib/dri`.
- **Postprocessors:** `CPU`, `VAAPI`, and `OPENCL`; OpenCL additionally requires NV12 and successful VAAPI-to-OpenCL interop.
- **Output memory:** CPU or GPU depending on postprocessing. VAAPI postprocessing with GPU output is allowed only for `NATIVE`.
- **Output formats:** `BGR24`, `RGB24`, `GRAY8`, `NATIVE`, subject to postprocessor restrictions.

VAAPI is currently the most exercised decoding backend. A GPU-to-GPU path can retain a `VaapiFrame(surface_id)`; CPU-to-NumPy paths download and convert the frame. With the necessary interop support, OpenCL can process NV12 without an intermediate CPU conversion.

The code calls `vaSyncSurface` before processing. Certain corrupted surfaces may be skipped without raising an exception or closing the session. After 25 consecutive decode errors, the implementation attempts session recovery. This is **not** a guarantee of recovery from every driver failure.

## `HWAccel.CUDA`

- **Platform:** requires compatible NVIDIA hardware, drivers, and CUDA runtime.
- **Hardware pixel format:** `cuda`; the underlying software format is determined by FFmpeg.
- **Postprocessors:** `CPU` (downloads hardware frames) or `CUDA` (GPU conversion/resizing through a `.cu` CUDA kernel).
- **Output memory:** RAM with the CPU postprocessor; RAM or GPU device memory (DLPack) with the CUDA postprocessor.
- **Formats:** the CPU path depends on swscale and hardware download capabilities. The CUDA postprocessor explicitly requires **8-bit NV12** and produces `BGR24`, `RGB24`, or `GRAY8`, but not `NATIVE`.
- **Color handling with CUDA postprocessing:** fixed BT.709 limited-range coefficients, ignoring the input's color-matrix and range metadata. Broader support may be added later.

The CUDA postprocessor is supplied as a separate dynamically loaded library, `_framepipe_cuda.so` (`.pyd` on Windows). `examples/06_zero_copy_inference.py` demonstrates GPU output and PyTorch DLPack interoperability.

Selecting `HWAccel.CUDA` does not mean every input stream can be decoded with CUDA. The selected FFmpeg decoder must provide an appropriate hardware configuration; otherwise initialization fails. There is currently **no automatic software fallback**. In `0.4.2.dev4`, an unsupported configuration may result in a background restart loop rather than a clear terminal configuration error; fixing this is the highest-priority issue.

## `HWAccel.D3D11VA` — highly experimental

- **Platform:** Windows only. The selection is rejected on Linux.
- **Hardware pixel format:** determined by the FFmpeg configuration, typically D3D11.
- **Postprocessor:** `PostProcessor.CPU` only.
- **Output memory:** `MemoryType.CPU` only.
- **Output formats:** `BGR24`, `RGB24`, `GRAY8`; `NATIVE` only without resizing, with further caveats.
- **Data path:** D3D11 hardware frame → `av_hwframe_transfer_data()` → CPU FFmpeg swscale or native output.

This backend has not been meaningfully validated across Windows systems.

## General restrictions

1. Hardware decoding is requested from a specific FFmpeg decoder. If `avcodec_get_hw_config` does not expose the desired device, initialization fails **without software fallback**. In `0.4.2.dev4`, this may manifest as a repeated background restart loop.
2. A software frame produced where a hardware pixel format was expected is treated as an error.
3. The `HWAccel` choice does not determine which color matrix is used for conversion; that depends on the `PostProcessor`. See [pixel-formats-and-color.md](pixel-formats-and-color.md).
4. A decoder/driver/format combination tested on one computer should not be assumed to work on another.

## Selection and diagnostics

```python
from framepipe import HWAccel, Reader

with Reader("video.mp4", name="hw-demo", hwaccel=HWAccel.VAAPI) as reader:
    while reader.is_opened():
        frame = reader.read_new_frame()
        if frame is not None:
            print(reader.hwaccel_active, frame.pix_fmt, frame.memory_type)
```

`hwaccel_active` reports the actually activated FFmpeg hardware backend after a session has opened. Check the FFmpeg/driver logs and `frame.pix_fmt` as well; do not rely solely on the requested `hwaccel` parameter. A Reader that never reaches `STREAMING` with a test file may have an unsupported or incorrect configuration (`0.4.2.dev4`).
