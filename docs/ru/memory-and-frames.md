# `MemoryType`, классы кадров и способы доступа

> Версия при написании: `0.4.2.dev4`  
> `MemoryType` описывает расположение результата, но не конкретный GPU API, набор типов и способов экспорта будет развиваться.

## `MemoryType.CPU` и `MemoryType.GPU`

- `MemoryType.CPU`: кадр хранится в RAM. для packed BGR/RGB/GRAY можно получить numpy-массив, для поддерживаемых YUV/NV12 -- плоскости, `planes()` отдаст кортеж numpy массивов.
- `MemoryType.GPU`: кадр принадлежит GPU-контексту. В зависимости от постпроцессора это CUDA device memory, VAAPI surface или OpenCL memory objects, требуют соответствующего потребителя.
- В python-обертке `Reader` запрошенный `output_memory` хранится простым полем объекта, фактический результат смотрите через `frame.memory_type` и `type(frame)`.

## Типы `Frame`

| Класс | Реальное хранение | Основной доступ | Формат/особенности |
| --- | --- | --- | --- |
| `CpuPackedFrame` | CPU packed uint8 | `.array()`, `.numpy()`, `.dlpack()`, `__dlpack__` | `BGR24` / `RGB24`: `(H, W, 3)`; `GRAY8`: `(H, W)` |
| `CpuPlanarFrame` | CPU planes | `.planes()` | поддержаны только `yuv420p`, `yuvj420p`, `nv12` |
| `CudaFrame` | CUDA device buffer | `.cuda_ptr`, `.cuda_context_handle`, `.dlpack()` | packed uint8, RGB/BGR/GRAY |
| `VaapiFrame` | VAAPI hardware surface | `.surface_id` | GPU `NATIVE`, `pix_fmt="vaapi"` |
| `OpenCLBufferFrame` | OpenCL `cl_mem` buffer | `.opencl_buffer_handle`, `.opencl_context_handle`, `.dlpack()` | packed uint8 |
| `OpenCLImageFrame` | два OpenCL image plane | `.opencl_plane_handles`, `.opencl_context_handle` | NV12 `NATIVE`: Y и UV images |

Для каждого типа доступны свойства базового `Frame`: `width`, `height`, `src_width`, `src_height`, `channels`, `pix_fmt`, `pts`, `ts`, `id`, `key`, `memory_type`. `pts` -- значение во временной базе видеопотока (не секунды), `ts` -- monotonic timestamp публикации кадра (берется не из rtsp метаданных, а фиксируется потоком декодирования). `reader.last_pts_seconds` возвращает PTS последнего хранимого кадра в секундах.

### numpy и размеры

```python
from framepipe import CpuPackedFrame, CpuPlanarFrame

frame = reader.read_new_frame()
if isinstance(frame, CpuPackedFrame):
    arr = frame.array()  # numpy uint8, может иметь stride/padding
elif isinstance(frame, CpuPlanarFrame):
    planes = frame.planes()  # только yuv420p / yuvj420p / nv12
```

`CpuPackedFrame.array()` строит numpy-view с корректным stride строки и удерживает владельца памяти, пока view жив. `planes()` возвращает представления плоскостей, а не готовый RGB: формы Y/UV для NV12 и Y/U/V для YUV420P указаны в [pixel-formats-and-color.md](pixel-formats-and-color.md).

Ограничение: `NATIVE` может обозначать и другие FFMPEG pix_fmt (10-битные), объект будет `CpuPlanarFrame`, но `.planes()` выдаст ошибку, если формат не из списка реализованных, `NATIVE` это не уникальный доступ к плоскостям любого формата.

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

Можно вызвать также `torch.from_dlpack(frame.dlpack())`  
Для CPU packed DLPack также доступен, но не превращает CPU-кадр в GPU-кадр. CUDA kernel текущей версии (`0.4.2.dev4`) синхронизируется перед выдачей результата, передача через DLPack не требует промежуточного numpy/CPU буфера.

`OpenCLBufferFrame` объявляет DLPack OpenCL device_type=4, но стандартный PyTorch *не предоставляет OpenCL tensor backend. Python метод `read_tensor()` специально отвергает OpenCL, чтобы не перепутать `cl_mem` и указатель CUDA.

### OpenCL и VAAPI дескрипторы

`opencl_buffer_handle`, `opencl_context_handle`, `opencl_plane_handles`, `surface_id`, `cuda_ptr` -- низкоуровневые дескрипторы, они имеют смысл только внутри соответствующего API и пока жив `Frame`, удерживающий нужные ресурсы, не передавайте их в чужой GPU runtime без отдельной корректной interop-операции (`0.4.2.dev4` таких не поддерживает), у `VaapiFrame` нет `.array()` или `.dlpack()`

## Доступ к кадру через `Reader`

Методы с `_frame` возвращают объект `Frame` (в том числе для `NATIVE` и GPU), а NumPy-варианты требуют packed CPU. `read_dlpack()` и `read_new_dlpack()` возвращают `Frame` как DLPack producer, а не готовый capsule; `read_tensor()` / `read_new_tensor()` используют PyTorch и явно отклоняют OpenCL. `next()` и `retrieve()` также рассчитаны на packed CPU.
