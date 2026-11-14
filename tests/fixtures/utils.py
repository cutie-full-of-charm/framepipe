import platform
import time
from contextlib import contextmanager
from pathlib import Path

import pytest

from framepipe import State

ROOT = Path(__file__).parent.parent.parent
TESTS = ROOT / "tests"
FIXTURES = TESTS / "fixtures"
VIDEO_CACHE_DIR = FIXTURES / "video_cache"
MEDIAMTX_DIR = Path(FIXTURES / "mediamtx")
MEDIAMTX_EXECUTABLE = (
    "mediamtx.exe" if platform.system() == "Windows" else "mediamtx"
)


def wait_for_state(reader_inst, state: State, timeout=10.0):
    deadline = time.monotonic() + timeout

    while reader_inst.state != state:
        if time.monotonic() > deadline:
            pytest.fail(
                f"Can't initiate streaming in time, timeout: {timeout}s"
            )

        time.sleep(0.001)


def wait_for_frame(reader_inst, frame_num, timeout=10.0):
    deadline = time.monotonic() + timeout

    while True:
        if reader_inst.frames_processed >= frame_num:
            break
        if time.monotonic() > deadline:
            pytest.fail(f"Can't read frame in time, timeout: {timeout}s")

        time.sleep(0.001)


def collect_frames(reader_inst, count, timeout=10.0):
    deadline = time.monotonic() + timeout
    frames = []
    while time.monotonic() < deadline:
        if len(frames) >= count:
            break

        frame = reader_inst.read_new()
        if frame is not None:
            frames.append(frame)
        else:
            time.sleep(0.001)
    return frames


@contextmanager
def reader_context(cls, *args, **kw):
    obj = cls(*args, **kw)
    try:
        yield obj
    finally:
        obj.stop()
