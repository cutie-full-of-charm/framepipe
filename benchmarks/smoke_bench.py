import time
from pathlib import Path

import av

from framepipe import (
    HWAccel,
    OutputFormat,
    Reader,
)


def bench_framepipe(path: Path) -> tuple[int, float]:
    reader = Reader(
        source=str(path),
        name="benchmark",
        hwaccel=HWAccel.SOFTWARE,
        out_format=OutputFormat.NATIVE,
        on_demand=True,
    )

    count = 0
    start = time.perf_counter()

    try:
        while True:
            frame = reader.next_frame()
            if frame is None:
                break
            count += 1
    finally:
        reader.stop()

    elapsed = time.perf_counter() - start
    return count, elapsed

def bench_pyav(path: Path) -> tuple[int, float]:
    count = 0
    start = time.perf_counter()

    with av.open(str(path)) as container:
        for frame in container.decode(video=0):
            count += 1

    elapsed = time.perf_counter() - start
    return count, elapsed

if __name__ == "__main__":
    import sys

    path = Path(sys.argv[1])

    for name, fn in [
        ("PyAV", bench_pyav),
        ("framepipe", bench_framepipe),
    ]:
        count, elapsed = fn(path)
        print(
            f"{name}: {count} frames, "
            f"{elapsed:.3f}s, "
            f"{count / elapsed:.1f} FPS"
        )
