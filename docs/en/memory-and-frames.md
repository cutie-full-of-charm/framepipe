# `MemoryType`, frame classes, and ways to access data

> Version at the time of writing: `0.4.2.dev4`.  
> `MemoryType` describes where the result lives, not the specific GPU API. The set of types and export methods will continue to evolve.

## `MemoryType.CPU` and `MemoryType.GPU`

- `MemoryType.CPU`: the frame is stored in RAM. For packed BGR/RGB/GRAY, you can get a NumPy array; for supported YUV/NV12 formats, you can access the planes. `planes()` returns a tuple of NumPy arrays.
- `MemoryType.GPU`: the frame belongs to a GPU context. Depending on the postprocessor, it may be CUDA device memory, a VAAPI surface, or OpenCL memory objects; these require an appropriate consumer.
- The Python `Reader` wrapper stores the requested `output_memory` in a plain instance field. Check `frame.memory_type` and `type(frame)` to determine the actual result.

## `Frame` types

| Class | Actual storage | Main access | Format / notes |
| --- | --- | --- | --- |
| `CpuPackedFrame` | CPU packed uint8 | `.array()`, `.numpy()`, `.dlpack()`, `__dlpack__` | `BGR24` / `RGB24`: `(H, W, 3)`; `GRAY8`: `(H, W)` |
| `CpuPlanarFrame` | CPU planes | `.planes()` | Only `yuv420p`, `yuvj420p`, `nv12` are supported |
| `CudaFrame` | CUDA device buffer | `.cuda_ptr`, `.cuda_context_handle`, `.dlpack()` | Packed uint8, RGB/BGR/GRAY |
| `VaapiFrame` | VAAPI hardware surface | `.surface_id` | GPU `NATIVE`, `pix_fmt="vaapi"` |
| `OpenCLBufferFrame` | OpenCL `cl_mem` buffer | `.opencl_buffer_handle`, `.opencl_context_handle`, `.dlpack()` | Packed uint8 |
| `OpenCLImageFrame` | Two OpenCL image planes | `.opencl_plane_handles`, `.opencl_context_handle` | NV12 `NATIVE`: Y and UV images |

Every type exposes the properties of the base `Frame`: `width`, `height`, `src_width`, `src_height`, `channels`, `pix_fmt`, `pts`, `ts`, `id`, `key`, `memory_type`. `pts` is expressed in the video stream's time base (not seconds); `ts` is the monotonic timestamp of frame publication (it does not come from RTSP metadata -- the decoding thread records it). `reader.last_pts_seconds` returns the PTS of the last stored frame in seconds.

### NumPy and dimensions

```python
from framepipe import CpuPackedFrame, CpuPlanarFrame

frame = reader.read_new_frame()
if isinstance(frame, CpuPackedFrame):
    arr = frame.array()  # uint8 NumPy array; may have stride/padding
elif isinstance(frame, CpuPlanarFrame):
    planes = frame.planes()  # only yuv420p / yuvj420p / nv12
```

`CpuPackedFrame.array()` creates a NumPy view with the correct row stride and keeps the memory owner alive for as long as the view exists. `planes()` returns views of the planes, not ready-to-use RGB. Y/UV shapes for NV12 and Y/U/V shapes for YUV420P are listed in [pixel-formats-and-color.md](pixel-formats-and-color.md).

Limitation: `NATIVE` may refer to other FFmpeg pix_fmt values (including 10-bit formats). The object will be a `CpuPlanarFrame`, but `.planes()` will raise an error if the format is not in the implemented list. `NATIVE` does not mean universal access to the planes of every possible format.

### CUDA / PyTorch / DLPack

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

You can also call `torch.from_dlpack(frame.dlpack())`.  
DLPack is available for packed CPU frames as well, but it does not turn a CPU frame into a GPU frame. The CUDA kernel in the current version (`0.4.2.dev4`) is synchronized before the result is returned, and DLPack transfer does not require an intermediate NumPy/CPU buffer.

`OpenCLBufferFrame` advertises DLPack OpenCL device_type=4, but standard PyTorch *does not provide an OpenCL tensor backend*. The Python `read_tensor()` method explicitly rejects OpenCL to avoid confusing `cl_mem` with a CUDA pointer.

### OpenCL and VAAPI handles

`opencl_buffer_handle`, `opencl_context_handle`, `opencl_plane_handles`, `surface_id`, `cuda_ptr` are low-level handles. They only make sense within the corresponding API and while the `Frame` holding the required resources remains alive. Do not pass them to another GPU runtime without a separate, correct interop operation (`0.4.2.dev4` does not support such operations). `VaapiFrame` has neither `.array()` nor `.dlpack()`.

## Accessing frames through `Reader`

Methods ending in `_frame` return a `Frame` object (including for `NATIVE` and GPU), while NumPy variants require packed CPU output. `read_dlpack()` and `read_new_dlpack()` return a `Frame` as a DLPack producer, not a ready-made capsule; `read_tensor()` / `read_new_tensor()` use PyTorch and explicitly reject OpenCL. `next()` and `retrieve()` are also intended for packed CPU output.
