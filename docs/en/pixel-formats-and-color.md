# Pixel formats, color matrices, and ranges

> Documented version: `0.4.2.dev4`. Actual support is determined by the complete `HWAccel → PostProcessor → MemoryType → OutputFormat` chain, not merely by whether FFmpeg recognizes a format. These tables are expected to expand.

## Three different concepts

1. **Codec and decoded pixel format.** H.264, HEVC, VP9, and other codecs are decoded by FFmpeg to pixel formats such as `yuv420p`, `nv12`, or `p010le`. A hardware decoder may use `AV_PIX_FMT_VAAPI` or `AV_PIX_FMT_CUDA` with an underlying `sw_format=NV12`.
2. **Output `OutputFormat`.** The requested result (`BGR24`, `RGB24`, `GRAY8`, or `NATIVE`). The actual `frame.pix_fmt` depends on the processing path.
3. **Color matrix and related metadata.** The coefficients for YCbCr-to-RGB conversion (BT.601, BT.709, etc.), luma range (`limited`/`full`), and chroma sample location. None can be reliably inferred from the name `nv12` or the codec alone.

The matrix (`AVFrame.colorspace`) is not the same as color primaries (`color_primaries`) or transfer characteristics (`color_trc`). Supporting BT.2020 NCL matrix coefficients in OpenCL does **not** amount to a complete HDR, tonemapping, or color-primary conversion pipeline.

## `OutputFormat` values

| `OutputFormat` | Result | Available paths | Notes |
| --- | --- | --- | --- |
| `BGR24` | `bgr24`, 8-bit/channel, H × W × 3 | CPU, CUDA, VAAPI→CPU, OpenCL | Default Python Reader output; convenient for OpenCV |
| `RGB24` | `rgb24`, 8-bit/channel, H × W × 3 | CPU, CUDA, VAAPI→CPU, OpenCL | Useful for ML models expecting RGB |
| `GRAY8` | `gray`, 8-bit, H × W | CPU, CUDA, VAAPI→CPU, OpenCL | CUDA uses raw Y without limited→full normalization; OpenCL does normalize |
| `NATIVE` | Native representation of the selected processing path | CPU, VAAPI, OpenCL; **not** the CUDA postprocessor | No resizing with CPU/OpenCL; VAAPI GPU output retains a hardware surface |

The `pix_fmt` strings for packed results correspond to FFmpeg names (`bgr24`, `rgb24`, `gray`). With `NATIVE`, inspect `frame.pix_fmt` instead of assuming it will be `nv12`.

## Input formats accepted by postprocessors

| `PostProcessor` | Explicitly accepted input | Actual boundary |
| --- | --- | --- |
| `CPU` | Software AVFrame in a format accepted by libswscale; `NATIVE` preserves the software pixel format | Not every FFmpeg format is guaranteed; hardware sources also require successful download |
| `CUDA` | `AV_PIX_FMT_CUDA` with `sw_format=AV_PIX_FMT_NV12` | Other software formats (P010, YUV444, etc.) are rejected |
| `VAAPI` | `AV_PIX_FMT_VAAPI` surface | No built-in whitelist of underlying software formats; `scale_vaapi`, driver, and download support apply |
| `OPENCL` | `AV_PIX_FMT_VAAPI` with `sw_format=AV_PIX_FMT_NV12` | 8-bit NV12 only, with VAAPI→OpenCL mapping |

### `NATIVE` and `CpuPlanarFrame.planes()`

In `src/bindings.cpp`, the software formats with implemented Python plane extraction are explicitly limited to:

| Pixel format | `planes()` | Typical plane dimensions (rounded up for chroma) |
| --- | --- | --- |
| `yuv420p` | Yes | `Y (H,W)`, `U (ceil(H/2),ceil(W/2))`, `V` likewise |
| `yuvj420p` | Yes | Like `yuv420p`; legacy FFmpeg JPEG/full-range format |
| `nv12` | Yes | `Y (H,W)`, `UV (ceil(H/2), 2×ceil(W/2))` |
| `p010le`, `yuv420p10le`, `yuv422p`, `yuv444p`, etc. | Not implemented | Decoding may succeed, but the Python `.planes()` method can still fail |

The code does not define a comprehensive list of supported input pixel formats. The formats listed by the video test fixture generator (`tests/fixtures/video.py`) are generation options, **not** evidence of tested decoder support.

## Color matrices: behavior by postprocessor

| Matrix / parameter | `CPU` | `CUDA` | `VAAPI` | `OPENCL` |
| --- | --- | --- | --- | --- |
| BT.601 (`BT470BG` / `SMPTE170M`) | No explicit selection; FFmpeg swscale | No: conversion is fixed to BT.709 | No explicit selection; driver/VPP and swscale for CPU output | Yes |
| BT.709 | No explicit selection; FFmpeg swscale | Yes, fixed | No explicit selection | Yes |
| BT.2020 NCL | No explicit selection | No | No explicit selection | Yes, without HDR tonemapping |
| SMPTE 240M | No explicit selection | No | No explicit selection | Yes |
| FCC | No explicit selection | No | No explicit selection | Yes |
| Unknown / absent metadata | FFmpeg defaults | Still BT.709 limited | FFmpeg/driver defaults | `UNSPECIFIED` → BT.601 fallback; other unsupported values raise an error |
| Full/limited range | Not explicitly configured in framepipe's swscale path | RGB/BGR assumes limited; input metadata ignored | Not explicitly set; path-dependent | Yes, from `color_range`; unknown → limited |
| Chroma location | Not explicitly set | Fixed NV12 sampling geometry | Not explicitly set | Yes, from `chroma_location`; unknown → left |

**No explicit selection** is not a claim that color will always be correct or incorrect. Behavior depends on FFmpeg, its defaults, and the driver. Framepipe currently does not set these conversion parameters explicitly. Accurate colorimetric verification requires tests against reference pixels.

### `PostProcessor.CUDA`: actual coefficients

The CUDA kernel in `src/gpu/cuda.cu` converts NV12 to RGB/BGR using approximately these equations for 8-bit Y/U/V:

```text
C = 1.16438356 × (Y - 16)
D = U - 128
E = V - 128
R = C + 1.79274107 × E
G = C - 0.21324861 × D - 0.53290933 × E
B = C + 2.11240179 × D
```

These coefficients correspond to **BT.709 limited (TV) range**. `src->colorspace`, `src->color_range`, and `src->chroma_location` do not select the conversion coefficients. `GRAY8` writes the resized Y component as-is, so its values can differ from the OpenCL grayscale result for limited-range video.

### `PostProcessor.OPENCL`: exact rules

From `src/post_processors/impls/opencl.cpp` and `src/gpu/nv12_convert.cl`:

| `AVFrame.colorspace` | OpenCL handling |
| --- | --- |
| `AVCOL_SPC_BT709` | BT.709: `Kr=0.2126`, `Kb=0.0722` |
| `AVCOL_SPC_BT470BG`, `AVCOL_SPC_SMPTE170M` | BT.601: `Kr=0.299`, `Kb=0.114` |
| `AVCOL_SPC_BT2020_NCL` | BT.2020 NCL: `Kr=0.2627`, `Kb=0.0593` |
| `AVCOL_SPC_SMPTE240M` | SMPTE 240M: `Kr=0.212`, `Kb=0.087` |
| `AVCOL_SPC_FCC` | FCC: `Kr=0.30`, `Kb=0.11` |
| `AVCOL_SPC_UNSPECIFIED` | Warning and BT.601 fallback |
| Any other value | “unsupported color matrix” error |

For `color_range`, `AVCOL_RANGE_JPEG` means full; `AVCOL_RANGE_MPEG` means limited; other values produce a warning and default to limited. Supported `chroma_location` values: `LEFT`, `CENTER`, `TOPLEFT`, `TOP`, `BOTTOMLEFT`, and `BOTTOM`; any other value produces a warning and defaults to `LEFT`. OpenCL `GRAY8` conversion also accounts for full/limited range when converting Y.

These rules apply to OpenCL's **packed-output conversion**. With `OutputFormat.NATIVE`, no color matrix is applied: the result remains NV12 as planes/images.

## Resizing

- If both `out_w > 0` and `out_h > 0` are provided, they determine the exact output size; `scale` is ignored.
- Otherwise, when `scale != 1.0`, dimensions are calculated as `int(src_width * scale)` and `int(src_height * scale)`. Both must be positive.
- CPU uses `SWS_BILINEAR` for packed conversion.
- CUDA and OpenCL use bilinear resizing in the GPU kernel for packed output.
- VAAPI uses `scale_vaapi` when dimensions change.
- CPU/OpenCL `NATIVE` output cannot be resized; the CUDA postprocessor does not support `NATIVE`.

For a specific path and resulting frame class, see the [compatibility matrix](compatibility.md) and [memory types](memory-and-frames.md).
