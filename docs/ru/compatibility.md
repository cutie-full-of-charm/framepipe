# Совместимость декодеров, обработки, памяти и форматов

> Версия при написании: `0.4.2.dev4`.  

## Допустимые сочетания

| `HWAccel` | `PostProcessor` | `MemoryType` | `OutputFormat` | Результат (класс экземляра) | Уусловия / что учитывать |
| --- | --- | --- | --- | --- | --- |
| `SOFTWARE` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Конверсию выполнит FFmpeg swscale |
| `SOFTWARE` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Без resize, `planes()` доступен только для yuv420p/yuvj420p/nv12 |
| `CUDA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Здесь hardware frame будет скопирован на CPU через PCI-e шину |
| `CUDA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Без resize,  `planes()` доступен только для yuv420p/yuvj420p/nv12 |
| `CUDA` | `CUDA` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `CudaFrame` | Постпроцессор примет только CUDA NV12-поверхность, +др. ограничения |
| `VAAPI` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Скачивание из VRAM в CPU, затем swscale |
| `VAAPI` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Без resize, `planes()` только для yuv420p/yuvj420p/nv12 |
| `VAAPI` | `VAAPI` | `GPU` | NATIVE | `VaapiFrame` | Получим нативный VAAPI surface, постпроцессинг пропустится либо `scale_vaapi` |
| `VAAPI` | `VAAPI` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | VAAPI -> download в RAM -> swscale при необходимости |
| `VAAPI` | `VAAPI` | `CPU` | NATIVE | `CpuPlanarFrame` | `planes()` зависит от формата после download |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` / `OpenCLBufferFrame` | VAAPI NV12, поддержка VAAPI->OpenCL interop, сборка с OpenCL. |
| `VAAPI` | `OPENCL` | `CPU` / `GPU` | NATIVE | `CpuPlanarFrame` / `OpenCLImageFrame` | VAAPI NV12, без resize, interop |
| `D3D11VA` | `CPU` | `CPU` | BGR24, RGB24, GRAY8 | `CpuPackedFrame` | Windows, download + swscale; экспериментально |
| `D3D11VA` | `CPU` | `CPU` | NATIVE | `CpuPlanarFrame` | Windows, без resize; `planes()` только для известных pix_fmt |

## Недопустимые комбинации

Могли измениться, рекомендую проверять вручную на небольших тестовых скриптах/файлах.

- `PostProcessor.CPU` + `MemoryType.GPU`: запрещено.
- `PostProcessor.CUDA` с любым `HWAccel`, кроме `CUDA`: запрещено. `OutputFormat.NATIVE` с CUDA-постпроцессором: запрещено.
- `PostProcessor.VAAPI` с декодером, отличным от VAAPI: запрещено. VAAPI + `MemoryType.GPU` + не-`NATIVE`: запрещено.
- `PostProcessor.OPENCL` с декодером, отличным от VAAPI: запрещено + требует сборку с поддержкой OpenCL и формат NV12. `NATIVE` + изменение размера запрещено.
- `OutputFormat.NATIVE` + `PostProcessor.CPU` + изменение размера: запрещено.
- `HWAccel.D3D11VA` с постпроцессором, отличным от CPU, либо `MemoryType.GPU`: запрещено.
- В Win сборке VAAPI и OpenCL постпроцессоры явно отвергаются.
