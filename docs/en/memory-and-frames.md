# `MemoryType`, frame classes, and accessing their data

> Documented version: `0.4.2.dev4`. `MemoryType` describes the **location** of the result, not the GPU API used to access it. Supported representations and export mechanisms will evolve.

## `MemoryType.CPU` and `MemoryType.GPU`

- `MemoryType.CPU`: the result is stored in host RAM. Packed BGR/RGB/GRAY frames can be accessed as NumPy arrays; supported YUV/NV12 frames expose planes as a tuple of NumPy arrays through `.planes()`.
- `MemoryType.GPU`: the result belongs to a GPU context. Depending on the postprocessor, this means CUDA device memory, a VAAPI surface, or OpenCL memory objects. Each requires a compatible consumer.
- The Python `Reader` wrapper stores the requested `output_memory` on the Reader object. To inspect the actual result, use `frame.memory_type` and `type(frame)`.

## `Frame` types

| Class | Storage | Main accessors | Formats and details |
| --- | --- | --- | --- |
| `CpuPackedFrame` | Packed CPU `uint8` | `.array()`, `.numpy()`, `.dlpack()`, `__dlpack__` | `BGR24` / `RGB24`: `(H, W, 3)`; `GRAY8`: `(H, W)` |
| `CpuPlanarFrame` | CPU image planes | `.planes()` | Explicit support for `yuv420p`, `yuvj420p`, and `nv12` only |
| `CudaFrame` | CUDA device buffer | `.cuda_ptr`, `.cuda_context_handle`, `.dlpack()` | Packed `uint8`, RGB/BGR/GRAY |
| `VaapiFrame` | VAAPI hardware surface | `.surface_id` | GPU `NATIVE`, `pix_fmt="vaapi"` |
| `OpenCLBufferFrame` | OpenCL `cl_mem` buffer | `.opencl_buffer_handle`, `.opencl_context_handle`, `.dlpack()` | Packed `uint8` |
| `OpenCLImageFrame` | Two OpenCL image planes | `.opencl_plane_handles`, `.opencl_context_handle` | NV12 `NATIVE`: Y and UV images |

All frame types provide the base `Frame` properties: `width`, `height`, `src_width`, `src_height`, `channels`, `pix_fmt`, `pts`, `ts`, `id`, `key`, and `memory_type`. `pts` is expressed in the video stream's time base, **not seconds**. `ts` is a monotonic timestamp recorded when the decoder thread publishes the frame; it is not an RTSP metadata timestamp. `reader.last_pts_seconds` exposes the PTS of the last stored frame in seconds.

### NumPy arrays and dimensions

```python
from framepipe import CpuPackedFrame, CpuPlanarFrame

frame = reader.read_new_frame()
if isinstance(frame, CpuPackedFrame):
    arr = frame.array()  # NumPy uint8; may have a row stride or padding
elif isinstance(frame, CpuPlanarFrame):
    planes = frame.planes()  # yuv420p / yuvj420p / nv12 only
```

`CpuPackedFrame.array()` creates a NumPy view with the correct row stride and keeps the underlying memory owner alive while the view exists. For `nv12`, `.planes()` returns Y `(H, W)` and UV `(ceil(H/2), 2*ceil(W/2))`. For `yuv420p` and `yuvj420p`, it returns Y `(H, W)` and separate U/V planes `(ceil(H/2), ceil(W/2))`. These are views of the existing planes; producing RGB requires an additional color conversion (`swscale`, VAAPI, etc.).

**Limitation:** `NATIVE` can also represent other FFmpeg pixel formats, including 10-bit formats. Such a result may still be a `CpuPlanarFrame`, but `.planes()` raises an error unless its pixel format is among the explicitly implemented cases. `NATIVE` does not imply Python plane access for every FFmpeg format.

### CUDA, PyTorch, and DLPack

```python
from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, Reader
import torch

with Reader(
    "video.mp4", name="cuda-dlpack", hwaccel=HWAccel.CUDA,
    postprocessor=PostProcessor.CUDA, output_memory=MemoryType.GPU,
    out_format=OutputFormat.RGB24,
) as reader:
    while reader.is_opened():
        frame = reader.read_new_frame()
        if frame is not None:
            tensor = torch.from_dlpack(frame)  # CUDA uint8 HWC
            print(tensor.shape, tensor.device)
```

You can also call `torch.from_dlpack(frame.dlpack())`. DLPack is available for supported packed CPU results as well, but it does not move them to GPU memory. The CUDA kernel in `0.4.2.dev4` synchronizes before publishing the result; using DLPack avoids an intermediate NumPy or CPU buffer.

`OpenCLBufferFrame` advertises DLPack's OpenCL device type (`kDLOpenCL = 4`), but standard PyTorch does **not** provide an OpenCL tensor backend. The Python `read_tensor()` method explicitly rejects OpenCL rather than mistaking a `cl_mem` handle for a CUDA pointer.

### OpenCL and VAAPI handles

`opencl_buffer_handle`, `opencl_context_handle`, `opencl_plane_handles`, `surface_id`, and `cuda_ptr` are low-level handles. They are meaningful only within the corresponding GPU API and while the owning `Frame` is alive. Do not pass them to an unrelated GPU runtime without a valid interoperation mechanism; `0.4.2.dev4` does not expose such a generic mechanism. `VaapiFrame` has no `.array()` or `.dlpack()` method.

## Reading frames through `Reader`

- `read_frame()`, `read_new_frame()`, `read_latest_frame(timeout)`, and `next_frame()` return `Frame` instances and support native/GPU results.
- `read()`, `read_new()`, and `read_latest(timeout)` return `numpy.ndarray` only when the wrapper is configured for `MemoryType.CPU` and `BGR24`/`RGB24`/`GRAY8`; otherwise they raise `RuntimeError`.
- `next()` and `retrieve()` delegate NumPy conversion to C++; use them with packed CPU output. For other frame types, prefer `next_frame()` or the `read_*_frame()` variants.
- `read_dlpack()` and `read_new_dlpack()` return a **Frame**, which can act as a DLPack producer for supported packed CPU/CUDA/OpenCL buffer types. They do not return a ready-made DLPack capsule. Export is not implemented for planar frames, OpenCL images, or VAAPI surfaces.
- `read_tensor()` and `read_new_tensor()` use PyTorch conversion for the supported CPU/CUDA cases and explicitly reject OpenCL.

For reading semantics and `on_demand`, see [Reader API](reader-api.md).
