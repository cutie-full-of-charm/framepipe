from enum import Enum


class ReaderMods(Enum):
    pass

    # file -> realtime file (читаем с учетом pts)
    # file -> non realtime file (читаем как можно быстрее)
    # file -> on demand (читаем каждый следующий файл по команде)
