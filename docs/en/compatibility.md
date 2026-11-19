# Decoder, postprocessor, memory, and format compatibility

> Documented version: `0.4.2.dev4`.  
> A ✓ indicates an implemented code path **subject to further requirements**, not a guarantee that it works on every device.

## Allowed combinations

| `HWAccel` | `PostProcessor` | `MemoryType` | `OutputFormat` | Result class | Conditions / considerations |
| --- | --- | --- | --- | --- | --- |
| `SOFTWARE` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | FFmpeg swscale handles conversion |
| `SOFTWARE` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resizing; `planes()` only supports yuv420p/yuvj420p/nv12 |
| `CUDA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Hardware frames are transferred to CPU over PCIe or the available system interconnect |
| `CUDA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resizing; `planes()` only supports yuv420p/yuvj420p/nv12 |
| `CUDA` | `CUDA` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `CudaFrame` | Only CUDA NV12 surfaces, plus other restrictions |
| `VAAPI` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Hardware-to-host transfer followed by swscale |
| `VAAPI` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resizing; `planes()` supports only the listed formats |
| `VAAPI` | `VAAPI` | `GPU` | NATIVE | `VaapiFrame` | Native VAAPI surface; pass-through or `scale_vaapi` |
| `VAAPI` | `VAAPI` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | VAAPI download to RAM, then swscale if needed |
| `VAAPI` | `VAAPI` | `CPU` | NATIVE | `CpuPlanarFrame` | `planes()` support depends on the format after download |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `OpenCLBufferFrame` | VAAPI NV12, working VAAPI-to-OpenCL interop, OpenCL-enabled build; see [note](#performance-note) |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | NATIVE | `CpuPlanarFrame` / `OpenCLImageFrame` | VAAPI NV12, no resizing, working interop |
| `D3D11VA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Windows, download + swscale; experimental |
| `D3D11VA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Windows, no resizing; `planes()` only for supported formats |

### Performance note

In an informal run, the VAAPI-to-OpenCL path appeared to perform approximately **1.5× better** than an FFmpeg CLI path using `scale_vaapi`. This observation has **not** yet been captured in a reproducible benchmark and should not be treated as a general performance claim.

## Disallowed combinations

These restrictions may change. Verify a path with small test files on the intended hardware.

- `PostProcessor.CPU` + `MemoryType.GPU` is rejected.
- `PostProcessor.CUDA` requires `HWAccel.CUDA`; it does not accept `OutputFormat.NATIVE`.
- `PostProcessor.VAAPI` requires VAAPI decoding. VAAPI postprocessing with GPU output requires `NATIVE`.
- `PostProcessor.OPENCL` requires VAAPI decoding, an OpenCL-enabled build, and NV12 input. `NATIVE` output cannot be resized.
- `OutputFormat.NATIVE` + `PostProcessor.CPU` does not allow resizing.
- `HWAccel.D3D11VA` only works with CPU postprocessing and CPU output.
- The Windows build explicitly rejects VAAPI and OpenCL postprocessors.

## How to interpret input pixel-format support

This table describes data paths and **output** formats, not every supported input codec. The FFmpeg decoder and GPU driver determine whether an H.264, HEVC, or other stream can use a given hardware decoder. CUDA and OpenCL postprocessors explicitly require NV12; the VAAPI postprocessor does not maintain a universal whitelist of accepted underlying `sw_format` values. The CPU path is limited to formats supported by libswscale and, when relevant, hardware-frame download.

Support for a pixel format is not the same as support for its color matrix: see [pixel-formats-and-color.md](pixel-formats-and-color.md). For individual backends, consult [hwaccels.md](hwaccels.md) and [postprocessors.md](postprocessors.md).
