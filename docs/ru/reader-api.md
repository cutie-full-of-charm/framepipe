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

## Модели доставки и тип результата

`Reader` разделяет как получать кадр (немедленно / только новый / ожидать новый / запросить следующий) и в каком виде получить (`Frame` или NumPy). Фоновое чтение реализует latest-drop, приоритетный для realtime ML/CV и edge-систем; `on_demand` позволяет покадрово обрабатывать файл. Детали, диаграмма и рекомендации — в [reading-models.md](reading-models.md).

## Методы получения кадров

| Метод | Возвращает | Когда использовать |
| --- | --- | --- |
| `read_frame()` | Последний `Frame` или `None` | Повторный доступ к последнему опубликованному кадру, CPU/GPU/NATIVE |
| `read_new_frame()` | Новый `Frame` или `None` | Не повторять кадр, но промежуточные могут быть пропущены |
| `read_latest_frame(timeout=1.0)` | `Frame` или `None` | Ожидать новый кадр не дольше указанного таймаута |
| `next_frame()` | `Frame` или `None` на EOF | Файл, только `on_demand=True`, один кадр по запросу |
| `read()` | NumPy ndarray или `None` | Только packed CPU (`RGB24`, `BGR24`, `GRAY8`) |
| `read_new()` | NumPy ndarray или `None` | То же, только новый кадр |
| `read_latest(timeout)` | NumPy ndarray или `None` | То же, с ожиданием нового кадра |
| `next()` | NumPy ndarray или `None` | On-demand + packed CPU; для GPU/NATIVE используйте `next_frame()` |
| `read_tensor()`, `read_new_tensor()` | PyTorch tensor / `None` | DLPack для CPU/CUDA packed; OpenCL отклоняется |
| `read_dlpack()`, `read_new_dlpack()` | `Frame` / `None` | Получить `Frame`; DLPack работает только у поддерживаемых packed типов |


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

`State`: `INIT`, `CONNECTING`, `STREAMING`, `DEGRADED`, `STOPPED`. Через `._on_state_change(name, old, new)` можно переопределить обработчик логирования состояний в Python-наследнике/обёртке; прямой публичный метод `set_state_callback()` есть у C++ класса, но не выведен как отдельный метод WrappedReader.

## Свойства

- `frame_pix_fmt`: pix_fmt последнего результата, а не обязательно исходного файла.
- `hwaccel_active`: имя реально активированного FFmpeg hwaccel после запуска сессии; до этого может быть `SOFTWARE`.
- `frames_processed`, `packets_processed`: счётчики (количество опубликованных кадров и обработанных пакетов).
- `last_pts_seconds`: PTS текущего кадра в секундах.
- `last_recv_time`: монотонное время последней публикации, не Unix timestamp.
- `state`, `name` -- текущее состояние и имя.

### Подорбнее

cм. [memory-and-frames.md](memory-and-frames.md), [architecture.md](architecture.md)
