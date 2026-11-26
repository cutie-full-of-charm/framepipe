# Архитектура и жизненный цикл кадра

> Версия при написании: `0.4.2.dev4`, интерфейсы и ограничения могут измениться.

## Пайплайн

В общем случае, рабочий пайплайн выглядит так:

```text
файл / RTSP / другой источник, распознаваемый FFmpeg
  │
  V
FFmpeg demux + video decoder -- HWAccel (SOFTWARE / CUDA / VAAPI / D3D11VA)
  │
  V
InputTransfer: RefTransfer, или трансфер hardware VRAM -> CPU RAM
  │
  V
IFrameProcessor: CPU / CUDA / VAAPI / OpenCL
  │                преобразование формата и/или изменение размера
  V
OutputTransfer: без копирования / CUDA->CPU / VAAPI->CPU
  │
  V
Frame: CpuPacked / CpuPlanar / Cuda / Vaapi / OpenCLBuffer / OpenCLImage
  │
  V
Reader: последний кадр / новый кадр / запрос кадра
```

Важные моменты:

1. `hwaccel` -- декодирование. Даже если оно выполняется на GPU, кадр может скачан на хост (грубо говоря, в RAM из VRAM).
2. `postprocessor` -- обработка. Например, VAAPI->OpenCL работает с VAAPI-декодированием и OpenCL-конвертацией.
3. `output_memory` -- размещение после обработки. RAM, CUDA, VAAPI surface, OpenCL GPU buffer -- разные варианты хранения.
4. `out_format` -- формат результата. `NATIVE` сохраняет нативное представление в рамках пайплайна. Фактический итоговый формат пайплайна можно получить через `frame.pix_fmt`

Матрица допустимых комбинаций: [compatibility.md](compatibility.md).

## Режимы чтения

Базовый, изначально задуманный и проработанный режим -- **latest-drop**: producer работает в отдельном потоке, не зависит от python-обработчика, и каждый новый кадр перезаписывает предыдущий. В общем, остальные режимы были как бы приделаны к этому, какие-то более удачно, какие-то менее удачно. Подробности — в отдельном разделе.

Диаграмма слота, различия `read*`/`next*`, `realtime_file` и общая позиция чтения — в [reading-models.md](reading-models.md). Здесь важно только то, что модель получения кадра не зависит от выбора `PostProcessor` и `MemoryType`.

## Ошибки и состояния

`State`: `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, `STOPPED`. Состояние `STREAMING` устанавливается после получения ключевого кадра. Подробнее о жизненном цикле и доступе к последней терминальной ошибке -- в [reader-api.md](reader-api.md).
