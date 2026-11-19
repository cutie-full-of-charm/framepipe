# Postprocessors (`PostProcessor`)

> Documented version: `0.4.2.dev4`. This page describes how frames are processed. Support for additional formats and color matrices is evolving. See the [compatibility matrix](compatibility.md).

## Overview

| Postprocessor | Required decoder | Input format | Output formats | Output location | Resizing | Color-matrix behavior |
| --- | --- | --- | --- | --- | --- | --- |
| `CPU` | SOFTWARE / CUDA / VAAPI / D3D11VA | Software `AVFrame`, directly or after hardware download | BGR24, RGB24, GRAY8, NATIVE | CPU | Yes, except NATIVE | FFmpeg swscale; no explicit matrix selection |
| `CUDA` | CUDA | CUDA surface, `sw_format=NV12` | BGR24, RGB24, GRAY8 | CPU or CUDA GPU | Bilinear algorithm in CUDA kernel | Fixed BT.709 limited for RGB/BGR |
| `VAAPI` | VAAPI | VAAPI surface | BGR24, RGB24, GRAY8, NATIVE | CPU; GPU only for NATIVE | `scale_vaapi` | VPP without explicit matrix configuration; swscale for CPU conversion |
| `OPENCL` | VAAPI | 8-bit NV12 VAAPI surface | BGR24, RGB24, GRAY8, NATIVE | CPU or OpenCL GPU | Yes for packed; no resizing for NATIVE | Supports matrix, range, and chroma-location metadata |

## `PostProcessor.CPU`

Implementation references: `src/post_processors/impls/cpu.cpp`, `src/post_processors/transfer.hpp`.

- For GPU decoding backends, the input surface is first downloaded to host memory using `av_hwframe_transfer_data()`.
- Packed output is created via `sws_getCachedContext()` and `sws_scale()` with `SWS_BILINEAR`, supporting `RGB24`, `BGR24`, `GRAY8`, and resizing.
- If both dimensions and pixel format already match, the implementation may pass through an `AVFrame` reference instead of performing an unnecessary conversion.
- `NATIVE` clones the original software `AVFrame` and exposes a `CpuPlanarFrame`, without resizing. Its `.planes()` method is implemented only for `yuv420p`, `yuvj420p`, and `nv12`.
- The code does not explicitly select YCbCr-to-RGB coefficients from `src->colorspace` or `color_range`; the resulting behavior depends on libswscale.

`MemoryType.CPU` can be used with each implemented decoder, provided hardware download and processing support the actual frame format.

## `PostProcessor.CUDA`

Implementation references: `src/post_processors/impls/cuda.cpp`, `src/gpu/cuda.cu`.

- Only supported with `HWAccel.CUDA`; input must be an `AV_PIX_FMT_CUDA` surface with `sw_format=AV_PIX_FMT_NV12`.
- The CUDA kernel performs bilinear resizing and, where needed, NV12-to-RGB/BGR conversion.
- `GRAY8` takes the Y plane. `RGB24` and `BGR24` use hard-coded **BT.709 limited-range** coefficients; the kernel ignores source color metadata in `0.4.2.dev4`.
- `MemoryType.GPU` returns a packed `uint8` `CudaFrame` (with DLPack support). `MemoryType.CPU` downloads the converted packed result to RAM.

**Color warning:** the CUDA conversion path cannot promise accurate color for BT.601/BT.2020 matrices, full-range video, or incompatible chroma layouts. If precise color is important, inspect source metadata and consider another processing path.

## `PostProcessor.VAAPI`

Implementation reference: `src/post_processors/impls/vaapi.cpp`.

- Requires `HWAccel.VAAPI` and a hardware VAAPI `AVFrame` on Linux.
- Without resizing, the surface passes through without a filter.
- With resizing, the FFmpeg graph is `buffer → scale_vaapi → buffersink`.
- **GPU output:** only `OutputFormat.NATIVE`; returns a `VaapiFrame` with `pix_fmt="vaapi"` and `surface_id`. This postprocessor does not convert to RGB while keeping the result as a VAAPI surface.
- **CPU output:** downloads the VAAPI surface. `NATIVE` retains the downloaded software pixel format; RGB/BGR/GRAY conversions use `sws_scale`.
- `scale_vaapi` is not configured with explicit color-matrix options. Download formats depend on FFmpeg and the driver.

VAAPI manages surfaces and resizing; `PostProcessor.OPENCL` is a separate route for GPU-side NV12-to-packed conversion with deliberate color-matrix selection.

## `PostProcessor.OPENCL`

Implementation references: `src/post_processors/impls/opencl.cpp`, `src/gpu/nv12_convert.cl`.

- Requires a Linux build with OpenCL, `HWAccel.VAAPI`, an 8-bit NV12 VAAPI surface, and working VAAPI-to-OpenCL interoperability.
- NV12 Y and UV planes are mapped as OpenCL images. For `BGR24`, `RGB24`, and `GRAY8`, the kernel converts and resizes them using bilinear sampling.
- Packed output with `MemoryType.GPU` yields `OpenCLBufferFrame` (`cl_mem`); with `MemoryType.CPU`, it yields `CpuPackedFrame`.
- `OutputFormat.NATIVE` is supported only at the original dimensions: as `OpenCLImageFrame` on GPU or `CpuPlanarFrame` with NV12 on CPU.
- Color support includes BT.709, BT.601 (BT.470BG/SMPTE 170M), BT.2020 NCL, SMPTE 240M, and FCC. `UNSPECIFIED` falls back to BT.601 with a warning; other unrecognized values raise an error. Full/limited range and several chroma locations are supported; see [pixel formats and color](pixel-formats-and-color.md).
- Packed OpenCL DLPack output advertises an OpenCL device (`device_type=4`). This is **not CUDA**, and the maintainer's `torch.from_dlpack()` test did not succeed in `0.4.2.dev4`.

## Where transfers and copies happen

| Path (decoder → postprocessor → output) | Memory operation |
| --- | --- |
| SOFTWARE → CPU → CPU RAM | CPU conversion, with a new buffer when needed |
| CUDA → CPU → CPU RAM | Hardware-to-host transfer before processing |
| CUDA → CUDA → GPU VRAM | CUDA kernel; final output stays in device memory |
| CUDA → CUDA → CPU RAM | CUDA kernel followed by GPU-to-CPU transfer |
| VAAPI → VAAPI → GPU NATIVE | VAAPI surface/pass-through or scaling; no mandatory CPU download |
| VAAPI → VAAPI → CPU | VAAPI processing followed by download and optional swscale |
| VAAPI → OPENCL → GPU | VAAPI-to-OpenCL mapping, then a kernel or direct plane exposure |
| VAAPI → OPENCL → CPU | VAAPI-to-OpenCL mapping followed by download of the result |

**Note:** "no CPU download" does not mean zero GPU allocations or device-side copies. The phrase **zero-copy** in example 06 primarily refers to passing an already prepared CUDA buffer to PyTorch using DLPack, not to the entire video decode pipeline.
