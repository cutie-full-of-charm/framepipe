# Postprocessors (`PostProcessor`)

> Version at the time of writing: `0.4.2.dev4`.  
> This page describes the frame processing path. The implementation is still evolving, including its handling of color matrices and formats.  
> General table: [compatibility.md](compatibility.md).

The combinations of `HWAccel` / `PostProcessor` / `MemoryType` / `OutputFormat` are listed in [compatibility.md](compatibility.md). Below is what each postprocessor actually does, including input limitations and where copies take place.

## `PostProcessor.CPU`

Implementation files: `src/post_processors/impls/cpu.cpp`, `src/post_processors/transfer.hpp`.

- With GPU decoders, the input surface is first downloaded to the CPU using `av_hwframe_transfer_data()`.
- For packed output, `sws_getCachedContext()` and `sws_scale()` with `SWS_BILINEAR` are used: `RGB24`, `BGR24`, `GRAY8`, and resizing are supported.
- If the dimensions and pix_fmt already match, the frame is passed through by reference to the AVFrame instead of performing an unnecessary conversion.
- `NATIVE` clones the source software AVFrame and exposes a `CpuPlanarFrame`; resizing is forbidden in this case. However, `planes()` is implemented only for `yuv420p`, `yuvj420p`, and `nv12`.
- The code does not explicitly select YCbCr->RGB coefficients from `src->colorspace`/`color_range`; the exact conversion depends on libswscale's implementation.

Combinations: `MemoryType.CPU` can be used with any implemented decoder, provided that its format can be successfully downloaded and processed.

## `PostProcessor.CUDA`

Implementation files: `src/post_processors/impls/cuda.cpp`, `src/gpu/cuda.cu`.

- Only supported with `HWAccel.CUDA`; input must be `AV_PIX_FMT_CUDA` with `sw_format=AV_PIX_FMT_NV12`.
- A CUDA kernel performs bilinear resizing and, if necessary, NV12->RGB/BGR conversion.
- `GRAY8` comes from the Y plane; `RGB24`/`BGR24` use fixed BT.709 limited. The kernel ignores matrix and range metadata. The formula is given in [pixel-formats-and-color.md](pixel-formats-and-color.md).
- `MemoryType.GPU` -> `CudaFrame` containing packed `uint8` (with DLPack); `MemoryType.CPU` -> download of the converted packed result into RAM.

## `PostProcessor.VAAPI`

Implementation file: `src/post_processors/impls/vaapi.cpp`.

- Only supported with `HWAccel.VAAPI` and a hardware VAAPI AVFrame (Linux).
- If no resize is required, the surface is passed through without a filter.
- If the size changes, the FFmpeg graph `buffer -> scale_vaapi -> buffersink` is used.
- GPU output: only `OutputFormat.NATIVE`; returns a `VaapiFrame` with `pix_fmt="vaapi"` and an accessible `surface_id`. This postprocessor does not convert to RGB.
- CPU output: the frame is downloaded from VAAPI; `NATIVE` keeps the downloaded software pix_fmt, while RGB/BGR/GRAY are produced via `sws_scale`.
- `scale_vaapi` is not configured with color-matrix parameters. The downloaded surface format depends on FFmpeg/the driver.

VAAPI and OpenCL serve different purposes: VAAPI handles surfaces and resizing; the separate `PostProcessor.OPENCL` performs NV12->packed conversion on the GPU with an explicit choice of color matrix.

## `PostProcessor.OPENCL`

Implementation files: `src/post_processors/impls/opencl.cpp`, `src/gpu/nv12_convert.cl`.

- Requires a Linux build with OpenCL detected, a `HWAccel.VAAPI` decoder, an NV12 (8-bit) VAAPI input surface, and successful VAAPI->OpenCL interop.
- NV12 planes are mapped as OpenCL Y/UV images; for `BGR24`/`RGB24`/`GRAY8`, the kernel performs bilinear resize and conversion.
- `MemoryType.GPU` + packed -> `OpenCLBufferFrame` (`cl_mem`); `MemoryType.CPU` + packed -> `CpuPackedFrame`.
- `OutputFormat.NATIVE` is supported only at source dimensions: on GPU this is an `OpenCLImageFrame`, on CPU a `CpuPlanarFrame` with NV12.
- Color: matrix, range, and chroma-location metadata are taken into account; unknown values have specific fallback rules or cause an error. Exact variants and defaults are documented in [pixel-formats-and-color.md](pixel-formats-and-color.md).
- Packed OpenCL DLPack identifies the device as OpenCL (device_type=4). It is not CUDA; `torch.from_dlpack()` did not work for me in `0.4.2.dev4`.

## Where copies happen

| Path (decode -> postprocessor -> CPU/GPU placement) | Memory operation |
| --- | --- |
| CPU -> CPU postprocessor -> CPU RAM | CPU conversion, a new buffer if needed |
| CUDA -> CPU postprocessor -> CPU RAM | HW -> CPU before processing |
| CUDA -> CUDA postprocessor -> GPU VRAM | CUDA kernel; result stays on the CUDA GPU and never leaves the graphics card |
| CUDA -> CUDA postprocessor -> CPU RAM | CUDA kernel, then GPU -> CPU download |
| VAAPI -> VAAPI postprocessor -> GPU NATIVE | VAAPI surface/scale, no mandatory CPU download |
| VAAPI -> VAAPI postprocessor -> CPU | VAAPI surface/scale, then HW -> CPU and swscale if needed |
| VAAPI -> OPENCL postprocessor -> GPU | VAAPI -> OpenCL mapping, then OpenCL kernel or plane export |
| VAAPI -> OPENCL postprocessor -> CPU | VAAPI -> OpenCL mapping and result download |
