# Decoder, processing, memory, and format compatibility

> Version at the time of writing: `0.4.2.dev4`.

## Supported combinations

| `HWAccel` | `PostProcessor` | `MemoryType` | `OutputFormat` | Result (instance class) | Conditions / caveats |
| --- | --- | --- | --- | --- | --- |
| `SOFTWARE` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Conversion is performed by FFmpeg swscale |
| `SOFTWARE` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resize; `planes()` is available only for yuv420p/yuvj420p/nv12 |
| `CUDA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Hardware frame is copied to the CPU over PCIe |
| `CUDA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resize; `planes()` is available only for yuv420p/yuvj420p/nv12 |
| `CUDA` | `CUDA` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `CudaFrame` | Postprocessor accepts only CUDA NV12 surfaces; other limitations apply |
| `VAAPI` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Download from VRAM to CPU, then swscale |
| `VAAPI` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | No resize; `planes()` only for yuv420p/yuvj420p/nv12 |
| `VAAPI` | `VAAPI` | `GPU` | NATIVE | `VaapiFrame` | Native VAAPI surface; postprocessing is skipped or `scale_vaapi` is used |
| `VAAPI` | `VAAPI` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | VAAPI -> download to RAM -> swscale if needed |
| `VAAPI` | `VAAPI` | `CPU` | NATIVE | `CpuPlanarFrame` | `planes()` depends on the format after download |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `OpenCLBufferFrame` | VAAPI NV12, VAAPI->OpenCL interop support, build with OpenCL |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | NATIVE | `CpuPlanarFrame` / `OpenCLImageFrame` | VAAPI NV12, no resize, interop |
| `D3D11VA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Windows, download + swscale; experimental |
| `D3D11VA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Windows, no resize; `planes()` only for known pix_fmt values |

## Unsupported combinations

These may have changed; I recommend checking them manually with small test scripts/files.

- `PostProcessor.CPU` + `MemoryType.GPU`: not supported.
- `PostProcessor.CUDA` with any `HWAccel` other than `CUDA`: not supported. `OutputFormat.NATIVE` with the CUDA postprocessor: not supported.
- `PostProcessor.VAAPI` with a decoder other than VAAPI: not supported. VAAPI + `MemoryType.GPU` + non-`NATIVE`: not supported.
- `PostProcessor.OPENCL` with a decoder other than VAAPI: not supported; also requires an OpenCL-enabled build and NV12 input. `NATIVE` + resize is not supported.
- `OutputFormat.NATIVE` + `PostProcessor.CPU` + resize: not supported.
- `HWAccel.D3D11VA` with a postprocessor other than CPU, or with `MemoryType.GPU`: not supported.
- The Windows build explicitly rejects VAAPI and OpenCL postprocessors.
