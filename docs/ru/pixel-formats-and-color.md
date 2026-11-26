# Пиксельные форматы, цветовые матрицы и диапазоны

> Версия при написании: `0.4.2.dev4`  
> Реальная поддержка определяется всей цепочкой `HWAccel -> PostProcessor -> MemoryType -> OutputFormat`, а не только наличием формата в FFmpeg.  
> В дальнейшем таблицы расширятся.

## Что значит `OutputFormat`

| `OutputFormat` | Результат | Где доступен | Особенности |
| --- | --- | --- | --- |
| `BGR24` | `bgr24`, 8 bit/channel, H×W×3 | CPU, CUDA, VAAPI→CPU, OpenCL | По умолчанию в Python `Reader`; удобен для OpenCV |
| `RGB24` | `rgb24`, 8 bit/channel, H×W×3 | CPU, CUDA, VAAPI→CPU, OpenCL | Удобен для моделей, ожидающих RGB |
| `GRAY8` | `gray`, 8 bit, H×W | CPU, CUDA, VAAPI→CPU, OpenCL | CUDA берёт Y без limited->full перенормировки; OpenCL перенормирует |
| `NATIVE` | Нативное представление для текущего маршрута | CPU, VAAPI, OpenCL; не CUDA-постпроцессор | Без resize у CPU и OpenCL; VAAPI GPU возвращает hardware surface |

Указанные имена `pix_fmt` для packed-форматов соответствуют FFMPEG: `bgr24`, `rgb24`, `gray`. Для NATIVE смотрите `frame.pix_fmt`, а не предполагайте заранее `nv12`.

## Форматы на входе постпроцессоров

| `PostProcessor` | Форматы, которые явно принимает код | Реальная граница |
| --- | --- | --- |
| `CPU` | Software AVFrame с форматом, поддерживаемым libswscale; NATIVE сохраняет software pix_fmt | Нельзя обещать любой FFmpeg pix_fmt; на HW-пути download должен быть поддержан |
| `CUDA` | `AV_PIX_FMT_CUDA` с `sw_format=AV_PIX_FMT_NV12` | Другие sw_format (P010, YUV444 и т. д.) отвергаются |
| `VAAPI` | `AV_PIX_FMT_VAAPI` surface | Нет встроенного whitelist underlying sw_format; ограничения зависят от `scale_vaapi`, драйвера и скачивания |
| `OPENCL` | `AV_PIX_FMT_VAAPI` с `sw_format=AV_PIX_FMT_NV12` | Только 8-bit NV12; требуется VAAPI->OpenCL mapping |

### `NATIVE` и `CpuPlanarFrame.planes()`

В `src/bindings.cpp` набор программных форматов, для которых реализовано извлечение плоскостей, ограничен явно:

| Pix fmt | `planes()` | Типичные размеры плоскостей (с округлением вверх для chroma) |
| --- | --- | --- |
| `yuv420p` | Да | `Y (H,W)`, `U (ceil(H/2),ceil(W/2))`, `V` аналогично |
| `yuvj420p` | Да | Как `yuv420p`; исторический JPEG/full-range тип FFmpeg |
| `nv12` | Да | `Y (H,W)`, `UV (ceil(H/2), 2×ceil(W/2))` |
| `p010le`, `yuv420p10le`, `yuv422p`, `yuv444p`, и т. д. | Не реализовано в `planes()` | Даже если FFmpeg декодировал кадр, Python-метод может бросить ошибку |

Полный список допустимых pix_fmt источника не зафиксирован в коде. Список форматов в генераторе тестовых видео (`tests/fixtures/video.py`) -- варианты генерации, а не таблица проверенной поддержки.

## Цветовые матрицы: что делает каждый процессор

| Матрица / параметр | `CPU` | `CUDA` | `VAAPI` | `OPENCL` |
| --- | --- | --- | --- | --- |
| BT.601 (`BT470BG` / `SMPTE170M`) | Нет явного выбора; FFmpeg swscale | Нет — конверсия жёстко BT.709 | Нет явного выбора, драйвер/VPP + swscale при CPU download | Да |
| BT.709 | Нет явного выбора; FFMPEG swscale | Да, фиксированно | Нет явного выбора | Да |
| BT.2020 NCL | Нет явного выбора | Нет | Нет явного выбора | Да, без HDR-тонмаппинга |
| SMPTE 240M | Нет явного выбора | Нет | Нет явного выбора | Да |
| FCC | Нет явного выбора | Нет | Нет явного выбора | Да |
| Неизвестная / отсутствующая метка | Поведение FFmpeg defaults | Всё равно BT.709 limited | Поведение FFmpeg/драйвера | `UNSPECIFIED` -> fallback BT.601; остальные неподдержанные значения -> ошибка |
| Диапазон full/limited | Не задаётся явно в коде swscale | RGB/BGR: limited; метка игнорируется | Не задаётся явно; зависит от тракта | Да, по `color_range`; неизвестно -> limited |
| Chroma location | Не задаётся явно | Фиксированная геометрия сэмплирования NV12 | Не задаётся явно | Да, по `chroma_location`; неизвестно -> left |

«Нет явного выбора» не означает, что цвет гарантированно правильный или неправильный: результат определяется библиотекой FFmpeg, её настройками по умолчанию и драйвером. Текущий код framepipe просто не передаёт вручную эти параметры конвертации. Для строгой цветометрической проверки нужны тесты с эталонными пикселями.

### `PostProcessor.CUDA`: фактические коэффициенты

CUDA kernel в `src/gpu/cuda.cu` вычисляет RGB/BGR из NV12 примерно по следующей формуле (значения Y/U/V — 8-битные):

```text
C = 1.16438356 × (Y - 16)
D = U - 128
E = V - 128
R = C + 1.79274107 × E
G = C - 0.21324861 × D - 0.53290933 × E
B = C + 2.11240179 × D
```

Это соответствует BT.709, limited (TV). Метаданные `src->colorspace`, `src->color_range`, `src->chroma_location` при выборе коэффициентов не используются. Для `GRAY8` записывается масштабированная Y-компонента *как есть*; результат может отличаться от OpenCL grayscale для limited-range видео.

### `PostProcessor.OPENCL`: точные правила

Из `src/post_processors/impls/opencl.cpp` и `src/gpu/nv12_convert.cl`:

| `AVFrame.colorspace` | Обработка OpenCL |
| --- | --- |
| `AVCOL_SPC_BT709` | BT.709: `Kr=0.2126`, `Kb=0.0722` |
| `AVCOL_SPC_BT470BG`, `AVCOL_SPC_SMPTE170M` | BT.601: `Kr=0.299`, `Kb=0.114` |
| `AVCOL_SPC_BT2020_NCL` | BT.2020 NCL: `Kr=0.2627`, `Kb=0.0593` |
| `AVCOL_SPC_SMPTE240M` | SMPTE 240M: `Kr=0.212`, `Kb=0.087` |
| `AVCOL_SPC_FCC` | FCC: `Kr=0.30`, `Kb=0.11` |
| `AVCOL_SPC_UNSPECIFIED` | Предупреждение, fallback BT.601 |
| Другое значение | Ошибка «unsupported color matrix» |

`color_range`: `AVCOL_RANGE_JPEG` -> full; `AVCOL_RANGE_MPEG` -> limited; остальные значения -> предупреждение + limited. Поддерживаемые `chroma_location`: `LEFT`, `CENTER`, `TOPLEFT`, `TOP`, `BOTTOMLEFT`, `BOTTOM`; остальные -> предупреждение + `LEFT`. Для `GRAY8` OpenCL также учитывает full/limited при преобразовании Y.

Эти правила относятся к OpenCL packed-конвертации. При `OutputFormat.NATIVE` матрица не применяется: выдаётся NV12 как набор плоскостей/изображений.

## Масштабирование

- Если указаны оба `out_w > 0` и `out_h > 0`, используется точный размер, а `scale` не применяется.
- Иначе при `scale != 1.0` размер вычисляется как `int(src_width * scale)` и `int(src_height * scale)`; значения должны получиться положительными.
- CPU: `SWS_BILINEAR` для packed-конвертации.
- CUDA и OpenCL: bilinear в GPU kernel для packed.
- VAAPI: `scale_vaapi`, если размер изменился.
- CPU/OPENCL `NATIVE`: изменение размера запрещено; CUDA-постпроцессор `NATIVE` не поддерживает.
