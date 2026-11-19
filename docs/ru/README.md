# Документация framepipe

> Версия описываемого API: `0.4.2.dev4` (dev). Документация фиксирует текущий код и известные ограничения. Новые форматы и бэкенды планируются.


| Файл | Содержание |
| --- | --- |
| [architecture.md](architecture.md) | Почему параметры декодирования, постпроцессинга и памяти независимы |
| [compatibility.md](compatibility.md) | Таблица допустимых комбинаций с форматами выхода |
| [hwaccels.md](hwaccels.md) | `HWAccel.SOFTWARE / CUDA / VAAPI / D3D11VA` и ограничения декодирования |
| [postprocessors.md](postprocessors.md) | `PostProcessor.CPU / CUDA / VAAPI / OPENCL`: что делают, как масштабируют и конвертируют цветовые форматы |
| [memory-and-frames.md](memory-and-frames.md) | `MemoryType`, конкретные классы кадров, numpy, DLPack, kDLOpenCL, vaapi_surfaces |
| [pixel-formats-and-color.md](pixel-formats-and-color.md) | Все про цветовые матрицы, colorspaces, etc. |
| [reading-models.md](reading-models.md) | Модель чтения: latest-drop, read/read_new/read_latest, `realtime_file`, `on_demand` и `next*` |
| [reader-api.md](reader-api.md) | Описаниее python API |
| [support-status.md](support-status.md) | Уровни уверенности в реализации, тесты, TODO, feel free to conrtibute btw |

## Коротко про компоненты

 - `HWAccel` определяет, где декодируется исходный кадр
 - `PostProcessor` определяет, как выполняются обработка и преобразование
 - `MemoryType` определяет, где будет лежать результат  

Пиксельные форматы и цветовые матрицы пересекают все три слоя, поэтому описаны отдельно.

NOTE: 
- "реализовано" -- соответствующая часть вроде как реализована в исходниках  
- "допустимо" -- код не отвергает сочетание параметров заранее, но что-то все равно может пойти не так  
- "проверено" -- подтверждено моии прогонами, но может не работать у вас
