# Python API: `Reader`

> Версия при написании: `0.4.2.dev4`  
> Здесь описана python-обертка `framepipe.Reader` (`framepipe/reader.py`)

## Создание

```python
from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, Reader

reader = Reader(
    source="video.mp4",
    name="camera-01",
    out_format=OutputFormat.BGR24,
    hwaccel=HWAccel.SOFTWARE,
    postprocessor=PostProcessor.CPU,
    output_memory=MemoryType.CPU,
)
```

| Аргумент | По умолчанию | Назначение |
| --- | --- | --- |
| `source` | обязательный | Путь к видео или URL источника |
| `name` | обязательный | Имя читателя, используется в логах |
| `verbose` | `False` | Расширенное логирование |
| `videostream_id` | `0` | Порядковый номер видеопотока в контейнере (не индекс любого потока) |
| `reconnect_delay_sec` | `1.0` | Пауза перед новой сессией после ошибки в фоновом режиме |
| `degraded_timeout_sec` | `3.0` | Порог устаревания последнего кадра для `is_alive()` |
| `av_container_options` | `None` | Словарь опций FFmpeg demuxer / container; C++ аргумент называется `av_options` |
| `out_format` | `OutputFormat.BGR24` | BGR24, RGB24, GRAY8, NATIVE |
| `hwaccel` | `HWAccel.SOFTWARE` | Где декодировать |
| `postprocessor` | `PostProcessor.CPU` | Как обрабатывать |
| `output_memory` | `MemoryType.CPU` | Где размещать результат |
| `out_w`, `out_h` | `0`, `0` | Целевой размер при указании обоих положительных значений |
| `scale` | `1.0` | Множитель размеров, если не заданы обе координаты |
| `realtime_file` | `False` | Для файла выдерживать интервалы PTS (наивный 1 мс polling) |
| `auto_start` | `True` | Автоматически запустить рабочий поток |
| `on_demand` | `False` | Получать кадры файла по одному по вызову `next_frame()` |

Распознавание live-источника основано на префиксах URL: `rtsp://`, `rtmp://`, `rtmps://`, `udp://`, `srt://`, `mms://`. Для распознанных live-источников выставляется набор низколатентных FFmpeg-опций (`rtsp_transport=tcp`, `fflags=nobuffer`, `flags=low_delay`, `max_delay=0`, `analyzeduration=0`, `probesize=32`); пользовательские `av_container_options` имеют приоритет по одноимённым ключам.

## Методы получения кадров

`Reader` разделяет **как** получать кадр (немедленно / только новый / ждать новый / запросить следующий) и **в каком виде**: `numpy.ndarray` через `read*` либо объект `Frame` через методы с `_frame`.

- `read()` / `read_frame()`, `read_new()` / `read_new_frame()`, `read_latest(timeout)` / `read_latest_frame(timeout=1.0)` работают с последним опубликованным кадром.
- `next()` / `next_frame()` запрашивают следующий кадр в режиме `on_demand=True` (только файл).
- `read_tensor()` / `read_new_tensor()` используют PyTorch и DLPack; `read_dlpack()` / `read_new_dlpack()` возвращают `Frame`, а не готовый capsule.

Подробная таблица возвращаемых типов, блокировки, пропусков кадров и примеры — в [reading-models.md](reading-models.md). Доступные методы конкретных классов кадров — в [memory-and-frames.md](memory-and-frames.md).

## Жизненный цикл

```python
from framepipe import Reader

with Reader("video.mp4", name="demo") as reader:
    while reader.is_opened():
        frame = reader.read_new_frame()
        if frame is not None:
            print(frame.id)
# stop() вызывается через контекстный менеджер
```

- `start()` — явный запуск потока, полезен с `auto_start=False`; повторный запуск уже работающего потока вызывает ошибку.
- `stop()` / `release()` — остановка; повторный `stop()` у Python-обёртки безопасен. После `stop()` доступ к методам чтения через `._reader` вызывает `ReaderStopped`, а некоторые свойства возвращают значения-заглушки.
- `is_opened()` — читатель не остановлен и не находится в `STOPPED`. Это не обещание, что уже получен валидный кадр.
- `is_alive()` — был ли недавно опубликован кадр относительно `degraded_timeout_sec`.
- `warmuped` — опубликован хотя бы один кадр (`frames_processed > 0`).
- `get_last_exception()` — возвращает последнюю сохранённую терминальную ошибку (`RuntimeError` или `None`), не выбрасывая её сам. Через `WrappedReader` getter недоступен после `stop()`, поскольку обёртка очищает `_impl`.

`State`: `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, `STOPPED`. Через `._on_state_change(name, old, new)` можно переопределить обработчик логирования состояний в Python-наследнике/обёртке; прямой публичный метод `set_state_callback()` есть у C++ класса, но не выведен как отдельный метод WrappedReader.

## Свойства

- `frame_pix_fmt`: pix_fmt последнего результата, а не обязательно исходного файла.
- `hwaccel_active`: имя реально активированного FFmpeg hwaccel после запуска сессии; до этого может быть `SOFTWARE`.
- `frames_processed`, `packets_processed`: счётчики (количество опубликованных кадров и обработанных пакетов).
- `last_pts_seconds`: PTS текущего кадра в секундах.
- `last_recv_time`: монотонное время последней публикации, не Unix timestamp.
- `state`, `name` -- текущее состояние и имя.
