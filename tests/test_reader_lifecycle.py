import subprocess
import sys
import threading
import time

import pytest

from framepipe import Frame, OutputFormat, State
from framepipe._framepipe import CpuPackedFrame, CpuPlanarFrame

from .fixtures.utils import reader_context, wait_for_state


def test_file_eof_is_not_error_and_keeps_final_frame(videos, reader_cls):
    # No calls to read_latest_frame until all frames have been decoded.
    # This checks that EOF does not discard the final unread frame.
    with reader_context(
        reader_cls,
        source=videos["30fps_10s.mp4"],
        name="eof-final-frame",
    ) as reader:
        wait_for_state(reader, State.STOPPED, timeout=25.0)
        assert reader.frames_processed == 300
        assert reader.get_last_exception() is None

        final_frame = reader.read_latest_frame(timeout=0.0)
        assert isinstance(final_frame, Frame)
        assert final_frame.id == reader.frames_processed
        assert reader.read_latest_frame(timeout=0.0) is None
        assert not reader.is_opened()


def test_latest_frame_returns_metadata(videos, reader_cls):
    with reader_context(
        reader_cls,
        source=videos["30fps_10s.mp4"],
        name="latest-frame-metadata",
        realtime_file=True,
    ) as reader:
        frame = reader.read_latest_frame(timeout=3.0)
        assert isinstance(frame, Frame)
        assert frame.id > 0
        assert frame.width == 800
        assert frame.height == 600
        assert frame.pts >= 0
        assert frame.ts > 0


def test_latest_frame_times_out_without_requests(videos, reader_cls):
    # In on_demand mode, latest-frame reads do not themselves request decoding.
    with reader_context(
        reader_cls,
        source=videos["30fps_10s.mp4"],
        name="latest-no-requests",
        on_demand=True,
    ) as reader:
        started = time.monotonic()
        assert reader.read_latest_frame(timeout=0.12) is None
        elapsed = time.monotonic() - started
        assert 0.08 <= elapsed < 2.0
        assert reader.frames_processed == 0
        assert reader.get_last_exception() is None


def test_latest_frame_wait_releases_gil(videos, reader_cls):
    # A worker Python thread must execute while C++ waits for a frame.
    with reader_context(
        reader_cls,
        source=videos["30fps_10s.mp4"],
        name="latest-gil-release",
        on_demand=True,
    ) as reader:
        worker_ran = threading.Event()
        worker_finished_at = []

        def worker():
            time.sleep(0.05)
            worker_finished_at.append(time.monotonic())
            worker_ran.set()

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        began = time.monotonic()
        result = reader.read_latest_frame(timeout=0.30)
        ended = time.monotonic()
        thread.join(timeout=1.0)

        assert result is None
        assert worker_ran.is_set()
        assert worker_finished_at and worker_finished_at[0] < ended
        assert ended - began >= 0.20


def test_latest_frame_wakes_on_stop_in_subprocess(videos):
    # stop/join and condition-variable bugs can hang pytest itself.
    # Isolate this test and enforce a hard timeout outside the test process.
    code = r'''
import sys
import threading
import time
from framepipe import Reader

reader = Reader(source=sys.argv[1], name="stop-wake", on_demand=True)
entered = threading.Event()
result = []
impl = reader._reader

def wait_for_frame():
    entered.set()
    result.append(impl.read_latest_frame(timeout=7.0))

worker = threading.Thread(target=wait_for_frame, daemon=True)
worker.start()
assert entered.wait(1.0)
time.sleep(0.2)
started = time.monotonic()
reader.stop()
worker.join(timeout=1.5)
assert not worker.is_alive(), "read_latest_frame did not wake on stop"
assert result == [None]
assert time.monotonic() - started < 2.5
'''
    try:
        result = subprocess.run(
            [sys.executable, "-P", "-c", code, str(videos["30fps_10s.mp4"])],
            capture_output=True,
            text=True,
            timeout=9.0,
            check=False,
        )
    except subprocess.TimeoutExpired:
        pytest.fail("Reader stop() or read_latest_frame() deadlocked")
    assert result.returncode == 0, (result.stdout + "\n" + result.stderr)


def test_on_demand_frame_and_latest_share_cursor_in_subprocess(videos):
    # next_frame() exercises an unfinished on-demand lifecycle. Keep it in
    # a child process so a decoder deadlock cannot hang the full pytest run.
    code = r'''
import sys
from framepipe import Reader
reader = Reader(source=sys.argv[1], name="demand-cursor", on_demand=True)
try:
    first = reader.next_frame()
    assert first is not None
    latest = reader.read_latest_frame(timeout=0.0)
    assert latest is not None and latest.id == first.id
    assert reader.read_latest_frame(timeout=0.0) is None
    assert reader.read_new_frame() is None  # shared last-read ID
finally:
    reader.stop()
'''
    try:
        result = subprocess.run(
            [sys.executable, "-P", "-c", code, str(videos["30fps_10s.mp4"])],
            capture_output=True,
            text=True,
            timeout=9.0,
            check=False,
        )
    except subprocess.TimeoutExpired:
        pytest.fail("on_demand next_frame/stop deadlocked")
    assert result.returncode == 0, (result.stdout + "\n" + result.stderr)


def test_cpu_frame_subtypes_match_output(videos, reader_cls):
    path = videos["30fps_10s.mp4"]
    with reader_context(
        reader_cls,
        source=path,
        name="cpu-packed",
        out_format=OutputFormat.BGR24,
    ) as reader:
        frame = reader.read_latest_frame(timeout=3.0)
        assert isinstance(frame, CpuPackedFrame)
        array = frame.array()
        assert array.dtype.name == "uint8"
        assert array.shape == (600, 800, 3)

    with reader_context(
        reader_cls,
        source=path,
        name="cpu-planar",
        out_format=OutputFormat.NATIVE,
    ) as reader:
        frame = reader.read_latest_frame(timeout=3.0)
        assert isinstance(frame, CpuPlanarFrame)
        planes = frame.planes()
        assert len(planes) in (2, 3)
        assert all(plane.size > 0 for plane in planes)
        # NumPy convenience methods intentionally reject native planar.
        with pytest.raises(RuntimeError, match="non-planar"):
            reader.read()
