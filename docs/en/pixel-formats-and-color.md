# Pixel formats, color matrices, and ranges

> Version at the time of writing: `0.4.2.dev4`.  
> Actual support is determined by the entire `HWAccel -> PostProcessor -> MemoryType -> OutputFormat` chain, not just by whether FFmpeg recognizes a format.  
> These tables will be expanded in future releases.

## What `OutputFormat` means

| `OutputFormat` | Result | Where available | Notes |
| --- | --- | --- | --- |
| `BGR24` | `bgr24`, 8 bit/channel, H×W×3 | CPU, CUDA, VAAPI→CPU, OpenCL | Default for Python `Reader`; convenient for OpenCV |
| `RGB24` | `rgb24`, 8 bit/channel, H×W×3 | CPU, CUDA, VAAPI→CPU, OpenCL | Convenient for models expecting RGB |
| `GRAY8` | `gray`, 8 bit, H×W | CPU, CUDA, VAAPI→CPU, OpenCL | CUDA uses Y without limited->full rescaling; OpenCL rescales it |
| `NATIVE` | Native representation for the current path | CPU, VAAPI, OpenCL; not the CUDA postprocessor | No resize for CPU and OpenCL; VAAPI GPU returns a hardware surface |

The listed `pix_fmt` names for packed formats match FFmpeg: `bgr24`, `rgb24`, `gray`. For `NATIVE`, inspect `frame.pix_fmt` rather than assuming `nv12` in advance.

## Input formats accepted by postprocessors

| `PostProcessor` | Formats explicitly accepted by the code | Actual limit |
| --- | --- | --- |
| `CPU` | Software AVFrame in a libswscale-supported format; NATIVE preserves the software pix_fmt | Cannot promise every FFmpeg pix_fmt; HW paths also require a supported download format |
| `CUDA` | `AV_PIX_FMT_CUDA` with `sw_format=AV_PIX_FMT_NV12` | Other sw_format values (P010, YUV444, etc.) are rejected |
| `VAAPI` | `AV_PIX_FMT_VAAPI` surface | No built-in whitelist of underlying sw_format; limits depend on `scale_vaapi`, the driver, and download |
| `OPENCL` | `AV_PIX_FMT_VAAPI` with `sw_format=AV_PIX_FMT_NV12` | Only 8-bit NV12; requires VAAPI->OpenCL mapping |

### `NATIVE` and `CpuPlanarFrame.planes()`

In `src/bindings.cpp`, the set of software formats for which plane extraction is implemented is explicitly limited:

| Pix fmt | `planes()` | Typical plane dimensions (chroma rounded up) |
| --- | --- | --- |
| `yuv420p` | Yes | `Y (H,W)`, `U (ceil(H/2),ceil(W/2))`, `V` likewise |
| `yuvj420p` | Yes | Same as `yuv420p`; FFmpeg's historical JPEG/full-range format |
| `nv12` | Yes | `Y (H,W)`, `UV (ceil(H/2), 2×ceil(W/2))` |
| `p010le`, `yuv420p10le`, `yuv422p`, `yuv444p`, etc. | Not implemented in `planes()` | Even if FFmpeg decodes a frame, the Python method may raise an error |

The code does not define a complete list of acceptable source pix_fmt values. The format list in the test-video generator (`tests/fixtures/video.py`) contains generation options, not a table of verified support.

## Color matrices: what each processor does

| Matrix / parameter | `CPU` | `CUDA` | `VAAPI` | `OPENCL` |
| --- | --- | --- | --- | --- |
| BT.601 (`BT470BG` / `SMPTE170M`) | No explicit selection; FFmpeg swscale | No -- conversion is hardcoded to BT.709 | No explicit selection; driver/VPP + swscale on CPU download | Yes |
| BT.709 | No explicit selection; FFmpeg swscale | Yes, fixed | No explicit selection | Yes |
| BT.2020 NCL | No explicit selection | No | No explicit selection | Yes, without HDR tone mapping |
| SMPTE 240M | No explicit selection | No | No explicit selection | Yes |
| FCC | No explicit selection | No | No explicit selection | Yes |
| Unknown / missing tag | FFmpeg default behavior | Still BT.709 limited | FFmpeg/driver behavior | `UNSPECIFIED` -> BT.601 fallback; other unsupported values -> error |
| Full/limited range | Not set explicitly in swscale code | RGB/BGR: limited; tag ignored | Not set explicitly; depends on the path | Yes, via `color_range`; unknown -> limited |
| Chroma location | Not set explicitly | Fixed NV12 sampling geometry | Not set explicitly | Yes, via `chroma_location`; unknown -> left |

“No explicit selection” does not mean the resulting colors are guaranteed to be either correct or incorrect: the outcome depends on FFmpeg, its defaults, and the driver. The current framepipe code simply does not pass these conversion parameters manually. Strict colorimetric verification requires tests against reference pixels.

### `PostProcessor.CUDA`: actual coefficients

The CUDA kernel in `src/gpu/cuda.cu` converts NV12 to RGB/BGR approximately using the following formula (Y/U/V are 8-bit values):

```text
C = 1.16438356 × (Y - 16)
D = U - 128
E = V - 128
R = C + 1.79274107 × E
G = C - 0.21324861 × D - 0.53290933 × E
B = C + 2.11240179 × D
```

This corresponds to BT.709 limited (TV). The `src->colorspace`, `src->color_range`, and `src->chroma_location` metadata are not used to select coefficients. For `GRAY8`, the scaled Y component is written *as is*; the output may differ from OpenCL grayscale for limited-range video.

### `PostProcessor.OPENCL`: exact rules

From `src/post_processors/impls/opencl.cpp` and `src/gpu/nv12_convert.cl`:

| `AVFrame.colorspace` | OpenCL handling |
| --- | --- |
| `AVCOL_SPC_BT709` | BT.709: `Kr=0.2126`, `Kb=0.0722` |
| `AVCOL_SPC_BT470BG`, `AVCOL_SPC_SMPTE170M` | BT.601: `Kr=0.299`, `Kb=0.114` |
| `AVCOL_SPC_BT2020_NCL` | BT.2020 NCL: `Kr=0.2627`, `Kb=0.0593` |
| `AVCOL_SPC_SMPTE240M` | SMPTE 240M: `Kr=0.212`, `Kb=0.087` |
| `AVCOL_SPC_FCC` | FCC: `Kr=0.30`, `Kb=0.11` |
| `AVCOL_SPC_UNSPECIFIED` | Warning, fallback to BT.601 |
| Other value | “unsupported color matrix” error |

`color_range`: `AVCOL_RANGE_JPEG` -> full; `AVCOL_RANGE_MPEG` -> limited; other values -> warning + limited. Supported `chroma_location` values: `LEFT`, `CENTER`, `TOPLEFT`, `TOP`, `BOTTOMLEFT`, `BOTTOM`; others -> warning + `LEFT`. For `GRAY8`, OpenCL also accounts for full/limited range when converting Y.

These rules apply to OpenCL packed conversion. With `OutputFormat.NATIVE`, no matrix is applied: NV12 is returned as planes/images.

## Resizing

- If both `out_w > 0` and `out_h > 0` are supplied, those exact dimensions are used and `scale` is ignored.
- Otherwise, when `scale != 1.0`, dimensions are calculated as `int(src_width * scale)` and `int(src_height * scale)`; the resulting values must be positive.
- CPU: `SWS_BILINEAR` for packed conversion.
- CUDA and OpenCL: bilinear resize in the GPU kernel for packed output.
- VAAPI: `scale_vaapi` if the size changes.
- CPU/OPENCL `NATIVE`: resizing is forbidden; the CUDA postprocessor does not support `NATIVE`.
