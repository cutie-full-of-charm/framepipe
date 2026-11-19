# Постпроцессоры (`PostProcessor`)

> Версия при написании: `0.4.2.dev4`  
> Здесь описан путь обработки кадра. Реализация развивается, в том числе по части цветовых матриц и форматов.  
> Общая таблица: [compatibility.md](compatibility.md).

## Таблица

| Постпроцессор | Требуемое декодирование | Формат на входе | Выходные форматы | Куда выводит | Масштабирование | Цветовая матрица |
| --- | --- | --- | --- | --- | --- | --- |
| `CPU` | SOFTWARE / CUDA / VAAPI / D3D11VA | Software AVFrame, полученный напрямую или после HW download | BGR24, RGB24, GRAY8, NATIVE | CPU | Да, если формат не NATIVE | FFmpeg swscale, явная настройка матрицы отсутствует |
| `CUDA` | CUDA | CUDA surface, sw_format=NV12 | BGR24, RGB24, GRAY8 | CPU или CUDA GPU | в CUDA kernel захардожен bilinear алгоритм | фиксированная, только BT.709 limited для RGB/BGR |
| `VAAPI` | VAAPI | VAAPI surface | BGR24, RGB24, GRAY8, NATIVE | CPU, для GPU только NATIVE | через `scale_vaapi` | VPP без настройки матрицы, CPU-конверсия через swscale |
| `OPENCL` | VAAPI | VAAPI surface, sw_format=NV12, 8-bit | BGR24, RGB24, GRAY8, NATIVE | CPU или OpenCL GPU | Да для packed, NATIVE без resize | Есть поддержка метаданных матрицы + range + chroma |

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
- `GRAY8` берётся из plane Y, `RGB24`/`BGR24` используют захардкоженные коэффициенты BT.709, limited-range, поддержка других будет добавлена в дальнейшем (`0.4.2.dev4`). Метаданные kernel не учитывает.
- `MemoryType.GPU` -> `CudaFrame` с packed `uint8` (и DLPack), `MemoryType.CPU` -> загрузка конвертированного packed результата в RAM.

Внимание: для других цветовых матриц (BT.601/BT.2020), full-range и другого расположения chroma нельзя обещать корректный цвет после конвертации cuda kernel'ом (`0.4.2.dev4`). Если важна корректная передача цвета, проверьте входные метаданные, выберите другой путь.

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
- Цвет: поддерживается почти все, что может понадобиться: матрицы BT.709, BT.601 (BT.470BG/SMPTE 170M), BT.2020 NCL, SMPTE 240M, FCC, при `UNSPECIFIED` выбирается BT.601 и пишется предупреждение, другие неизвестные значения вызывают ошибку. Поддержаны full/limited range и набор расположений chroma, значения по умолчанию описаны в [цветах](pixel-formats-and-color.md).
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

NOTE: "Без CPU-выгрузки" не означает отсутствие любых операций выделения/копирования на самом GPU, словосочетание zero-copy в примере 06 относится прежде всего к передаче готового CUDA-буфера в PyTorch через DLPack, а не ко всему декодированию видео
