import time

import pytest

from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, State

from .fixtures.utils import reader_context


def wait_for_error(reader, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        exc = reader.get_last_exception()
        if exc is not None and reader.state == State.STOPPED:
            return exc
        time.sleep(0.01)
    pytest.fail(f"terminal error not published: state={reader.state}")


def test_missing_file_is_terminal(tmp_path, reader_cls):
    with reader_context(
        reader_cls, source=str(tmp_path / "missing.mp4"), name="missing-file"
    ) as reader:
        exc = wait_for_error(reader)
        assert isinstance(exc, RuntimeError)
        assert not reader.is_opened()


def test_invalid_live_on_demand_rejected(reader_cls):
    with pytest.raises(RuntimeError, match="on_demand"):
        reader_cls(
            source="rtsp://127.0.0.1:8554/example",
            name="invalid-live-demand",
            on_demand=True,
            auto_start=False,
        )


def test_invalid_realtime_on_demand_rejected(videos, reader_cls):
    with pytest.raises(RuntimeError, match="on_demand"):
        reader_cls(
            source=videos["30fps_10s.mp4"],
            name="invalid-realtime-demand",
            on_demand=True,
            realtime_file=True,
            auto_start=False,
        )


def test_invalid_vaapi_gpu_format_rejected(videos, reader_cls):
    with pytest.raises(RuntimeError, match="NATIVE"):
        reader_cls(
            source=videos["30fps_10s.mp4"],
            name="invalid-vaapi-output-format",
            output_memory=MemoryType.GPU,
            postprocessor=PostProcessor.VAAPI,
            out_format=OutputFormat.BGR24,
            auto_start=False,
        )


@pytest.mark.parametrize("timeout", [-0.1, float("nan"), float("inf")])
def test_invalid_latest_timeout(videos, reader_cls, timeout):
    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"],
        name="invalid-latest-timeout", auto_start=False,
    ) as reader:
        with pytest.raises(RuntimeError, match="timeout"):
            reader.read_latest_frame(timeout=timeout)
