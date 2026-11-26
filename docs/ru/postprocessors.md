# Постпроцессоры (`PostProcessor`)

> Версия при написании: `0.4.2.dev4`  
> Здесь описан путь обработки кадра. Реализация развивается, в том числе по части цветовых матриц и форматов.  
> Общая таблица: [compatibility.md](compatibility.md).

Варианты `HWAccel` / `PostProcessor` / `MemoryType` / `OutputFormat` сведены в [compatibility.md](compatibility.md). Ниже — что на самом деле делает каждый постпроцессор, включая ограничения входа и места копирования.

## `PostProcessor.CPU`

Ref файлы реализации: `src/post_processors/impls/cpu.cpp`, `src/post_processors/transfer.hpp`.

- Для GPU-декодеров входная поверхность сначала скачивается в CPU через `av_hwframe_transfer_data()`.
- Для упакованного результата используются `sws_getCachedContext()` и `sws_scale()` с `SWS_BILINEAR`: доступны `RGB24`, `BGR24`, `GRAY8`, а также изменение размера.
- Если размеры и pix_fmt уже совпадают, выполняется passthrough через ссылку на AVFrame вместо лишней конверсии.
- `NATIVE` клонирует исходный software AVFrame и предоставляет `CpuPlanarFrame`, масштабирование в таком случае запрещено. При этом `planes()` реализован только для `yuv420p`, `yuvj420p`, `nv12`.
- Отсутствует код явного выбора коэффициентов YCbCr->RGB по `src->colorspace`/`color_range`, точное преобразование зависит от реализации libswscale.

Сочетания: `MemoryType.CPU` сочетается с любым декодером из реализованных, при условии успешного скачивания/обработки его формата.

## `PostProcessor.CUDA`

Ref файлы реализации: `src/post_processors/impls/cuda.cpp`, `src/gpu/cuda.cu`.

- Допустим только с `HWAccel.CUDA`, вход `AV_PIX_FMT_CUDA` +  `sw_format=AV_PIX_FMT_NV12`.
- В CUDA-kernel выполняются bilinear resize и, если требуется, преобразование NV12->RGB/BGR.
- `GRAY8` берётся из plane Y; `RGB24`/`BGR24` используют фиксированную BT.709 limited, метаданные матрицы и диапазона kernel не учитывает. Формула приведена в [pixel-formats-and-color.md](pixel-formats-and-color.md).
- `MemoryType.GPU` -> `CudaFrame` с packed `uint8` (и DLPack), `MemoryType.CPU` -> загрузка конвертированного packed результата в RAM.

## `PostProcessor.VAAPI`

Ref файлы реализации: `src/post_processors/impls/vaapi.cpp`.

- Допустим только с `HWAccel.VAAPI` и hardware VAAPI AVFrame (Linux).
- Если изменение размера не требуется -- passthrough без фильтра.
- Если размер меняется, будет использоваться граф FFmpeg `buffer -> scale_vaapi -> buffersink`
- GPU-выход: только `OutputFormat.NATIVE`, возвращается `VaapiFrame`, `pix_fmt="vaapi"`, доступен `surface_id`, конвертации в RGB у этого постпроцессора нет.
- CPU-выход: download из VAAPI, `NATIVE` оставляет скачанный software pix_fmt, а RGB/BGR/GRAY выполняются через `sws_scale`
- `scale_vaapi` не конфигурируется параметрами цветовой матрицы, формат поверхностей при download зависит от FFmpeg/драйвера. 

Разделение VAAPI и OpenCL: VAAPI отвечает за поверхность и resize, отдельный `PostProcessor.OPENCL` нужен для GPU-конвертации NV12->packed с осознанным выбором матрицы.

## `PostProcessor.OPENCL`

Ref файлы реализации: `src/post_processors/impls/opencl.cpp`, `src/gpu/nv12_convert.cl`.

- Нужны Linux-сборка с найденным OpenCL, декодер `HWAccel.VAAPI`, входная VAAPI-поверхность NV12 (8-bit) и успешная операция VAAPI->OpenCL interop.
- NV12-плоскости отображаются как OpenCL Y/UV images, в случае `BGR24`/`RGB24`/`GRAY8` kernel выполняет bilinear resize и конвертацию.
- `MemoryType.GPU` + packed -> `OpenCLBufferFrame` (`cl_mem`), `MemoryType.CPU` + packed → `CpuPackedFrame`.
- `OutputFormat.NATIVE` только при исходных размерах: на GPU это `OpenCLImageFrame`, на CPU -- `CpuPlanarFrame` с NV12.
- Цвет: учитываются метаданные матрицы, диапазона и положения chroma; неизвестные значения имеют отдельные правила fallback или вызывают ошибку. Точные варианты и значения по умолчанию — в [pixel-formats-and-color.md](pixel-formats-and-color.md).
- Packed OpenCL DLPack маркируется устройством OpenCL (device_type=4). Это не CUDA, `torch.from_dlpack()` в версии `0.4.2.dev4` у меня не сработал.

## Где происходит копирование

| Путь (декод -> постпроцессор -> размещение CPU/GPU) | Операция с памятью |
| --- | --- |
| CPU -> CPU постпроцессор -> CPU RAM | CPU-конверсия, при необходимости новый буфер |
| CUDA -> CPU постпроцессор -> CPU RAM | HW -> CPU перед обработкой |
| CUDA -> CUDA постпроцессор -> GPU VRAM | CUDA kernel, результат остаётся в CUDA GPU, не покидает видеокарту вообще |
| CUDA -> CUDA постпроцессор -> CPU RAM | CUDA kernel, затем GPU -> CPU hw_download |
| VAAPI -> VAAPI постпроцессор -> GPU NATIVE | VAAPI surface/scale, без обязательной CPU-выгрузки |
| VAAPI -> VAAPI постпроцессор -> CPU | VAAPI surface/scale, затем HW -> CPU и при необходимости swscale |
| VAAPI -> OPENCL постпроцессор -> GPU | VAAPI -> OpenCL mapping, затем OpenCL kernel или выдача плоскостей |
| VAAPI -> OPENCL постпроцессор -> CPU | VAAPI -> OpenCL mapping и hw_download результата |
