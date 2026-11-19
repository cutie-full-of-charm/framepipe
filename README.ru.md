# framepipe

**English:** [README.md](README.md) · [English documentation](docs/en/README.md)

**framepipe** — Python-библиотека для декодирования и подготовки видеокадров из файлов и сетевых потоков. Использует FFmpeg через C++-биндинг и предоставляет кадры в CPU-памяти либо поддерживаемые представления в GPU-памяти.

Изначально проект создавался для **realtime ML/CV и edge-инференса**, но стал более универсальным инструментом для видеопайплайнов. Самый проработанный сценарий по-прежнему — **read-latest-drop**: фоновое декодирование с публикацией последнего кадра без накопления очереди для отстающего потребителя. Для последовательной обработки файла есть `on_demand=True`.

> **Версия документации: `0.4.2.dev4` (development).** API Reader и поддерживаемые комбинации могут изменяться. Основной упор в разработке и тестировании делался на VAAPI; D3D11VA экспериментален и почти не проверялся. Готовые `.whl` пока планируются **только для manylinux**.

## Возможности

- Декодирование: `HWAccel.SOFTWARE`, `CUDA`, `VAAPI`, экспериментальный `D3D11VA`.
- Обработка: `PostProcessor.CPU`, `CUDA`, `VAAPI`, `OPENCL` — конверсия и масштабирование там, где они поддерживаются.
- Выход: `OutputFormat.BGR24`, `RGB24`, `GRAY8`, `NATIVE`; `MemoryType.CPU` / `GPU` в зависимости от пути обработки.
- NumPy для упакованных CPU-кадров; объекты `Frame`, DLPack и GPU-дескрипторы для поддерживаемых сценариев.
- Фоновое чтение последнего кадра и последовательное чтение файла по запросу.

**Совместимость определяется всей цепочкой: декодер → постпроцессор → формат → память.** Для GPU-сценариев сначала смотрите [матрицу совместимости](docs/ru/compatibility.md).

## Быстрый старт

Пакет доступен через PyPi
```sh
uv add framepipe
# or
pip install framepipe\
# или вы можете загрузить .whl со страницы релизов
```


Последовательное чтение файла с CPU/BGR-выводом по умолчанию:

```python
from framepipe import Reader

with Reader("video.mp4", name="demo", on_demand=True) as reader:
    while (image := reader.next()) is not None:
        print(image.shape)  # NumPy: H x W x 3
```

Для live-инференса `read_latest()` ожидает самый свежий доступный кадр без очереди предыдущих результатов:

```python
from framepipe import Reader

with Reader("rtsp://camera.example/live", name="camera") as reader:
    while reader.is_opened():
        image = reader.read_latest(timeout=1.0)
        if image is not None:
            print(image.shape)
```

Для GPU-вывода и `NATIVE` используйте методы с суффиксом `_frame`, например `read_latest_frame()` и `next_frame()`. Подробности: [модели чтения](docs/ru/reading-models.md) и [API Reader](docs/ru/reader-api.md).

## Документация

| Тема | Раздел |
| --- | --- |
| Устройство пайплайна | [Архитектура](docs/ru/architecture.md) |
| Допустимые сочетания компонентов | [Матрица совместимости](docs/ru/compatibility.md) |
| Декодирование | [HWAccel](docs/ru/hwaccels.md) |
| Конверсия и масштабирование | [Постпроцессоры](docs/ru/postprocessors.md) |
| Размещение и получение кадров | [MemoryType и Frame](docs/ru/memory-and-frames.md) |
| Форматы и цветовые матрицы | [Пиксельные форматы и цвет](docs/ru/pixel-formats-and-color.md) |
| `read` / `read_new` / `read_latest` / `on_demand` | [Модели чтения](docs/ru/reading-models.md) |
| Параметры и методы Reader | [API Reader](docs/ru/reader-api.md) |
| Текущая зрелость и ограничения | [Статус поддержки](docs/ru/support-status.md) |

Это **документация dev-версии**, а не гарантия работы на любой комбинации оборудования, кодека и драйвера. Поддержка форматов, корректность цветопередачи, тестирование платформ и API будут постепенно улучшаться.

Лицензия: MPL 2.0 (см. `LICENSE`).
