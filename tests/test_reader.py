import time

import pytest
from loguru import logger
from numpy import ndarray, uint8

from framepipe import State

from .fixtures.utils import (
    reader_context,
    wait_for_frame,
    wait_for_state,
)


def test_reads_file(videos, reader_cls):

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_state(reader, State.STREAMING)

        result = reader.read_new()

        assert reader.state == State.STREAMING
        assert reader.packets_processed != 0
        assert reader.frames_processed != 0
        assert reader.is_alive() is True
        assert reader._is_live is False
        assert isinstance(result, ndarray)


def test_reads_correct_frame_shape(videos, reader_cls):
    expected_frame = ndarray((600, 800, 3), dtype=uint8)

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_state(reader, State.STREAMING)

        actual_frame = reader.read_new()

        assert actual_frame.dtype == expected_frame.dtype
        assert actual_frame.shape == expected_frame.shape


# FIXME: здесь обращаюсь к полю класса (буфера), а не непосредственно к pts
# последнего кадра, возможно стоит, добавить debug метод, который будет
# возвращать tuple(frame, pts)
# UPD: теперь есть FrameEntry, возможно стоит использовать его?
#
# FIXME: datarace
# UPDATE: datarace fixed i guess.
def test_read_new_reads_no_dups(videos, reader_cls):

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_frame(reader, 1)

        prev_pts = -1.0
        frame_counter = 0
        deadline = time.monotonic() + 3.0
        while frame_counter < 20 and time.monotonic() < deadline:
            frame = reader.read_new()
            if frame is None:
                continue

            pts = reader.last_pts_seconds

            assert pts > prev_pts, f"ERROR: pts:{pts} <= prev:{prev_pts}?"
            prev_pts = pts
            frame_counter += 1
        assert frame_counter > 1


# FIXME: нужен _debug метод, возвращающий вместе с кадром также его PTS
# вероятно какой-то закрытый api-метод, читающий кадр и данные кадра атомарно,
# который я смогу тянуть для тестов, и из него должен произрастать урезанный
# публинчный api-метод?
def test_monotonic_pts_grow(videos, reader_cls):
    frames_count = 60

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_state(reader, State.STREAMING)

        prev = -1.0
        for _ in range(frames_count):
            if reader.read_new() is not None:
                pts = reader.last_pts_seconds
                assert pts >= prev, f"pts error: {pts} < {prev}"
                prev = pts
            time.sleep(0.05)


def test_monotonic_frames_and_packets_grow(videos, reader_cls):

    grow_time_secs = 3.0

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_state(reader, State.STREAMING)

        prev_packets = -1
        prev_frames = -1

        deadline = time.monotonic() + grow_time_secs
        while time.monotonic() <= deadline:
            if reader.packets_processed != prev_packets:
                assert reader.packets_processed > prev_packets, ""
            if reader.frames_processed != prev_frames:
                assert reader.frames_processed > prev_frames, ""

            prev_packets = reader.packets_processed
            prev_frames = reader.frames_processed
            time.sleep(0.001)


# FIXME: не работает вообще??
# FIXME: здесь impl становится None после stop(), как это тестировать
# тестировать что бросается ошибка? хм..
@pytest.mark.xfail(reason="doesnt work in any way?")
def test_stop_releases_resources(videos, reader_cls):

    reader = reader_cls(
        source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    )
    wait_for_frame(reader, 1)

    reader.stop()

    deadline = time.monotonic() + 3.0
    while reader.is_alive() is True:
        if time.monotonic() >= deadline:
            logger.critical(
                f"reader({reader_cls.__name__}) .stop() is alive timeout"
            )
            break
        time.sleep(0.001)

    assert reader.is_alive() is False
    assert reader.read_new() is None


def test_file_reader_not_live(videos, reader_cls):
    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        wait_for_state(reader, State.STREAMING)

        assert reader._is_live is False


def test_reader_double_stop(videos, reader_cls):

    reader = reader_cls(
        source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    )
    wait_for_frame(reader, 1)

    reader.stop()
    reader.stop()


# FIXME: FAILED tests/test_reader.py::test_reader_warmuped[WrappedSyncReader-10-10] - Failed: State != streaming but we have frames
def test_reader_warmuped(videos, reader_cls):

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        deadline = time.monotonic() + 3.0

        while time.monotonic() <= deadline:
            if reader.state == State.INIT:
                assert reader.warmuped is False
            if reader.state == State.CONNECTING:
                assert reader.warmuped is False
            if reader.state == State.STREAMING and reader.frames_processed == 0:
                assert reader.warmuped is False

            if reader.state != State.STREAMING and reader.frames_processed > 0:
                pytest.fail("State != streaming but we have frames")

            if reader.state != State.STREAMING and reader.warmuped:
                pytest.fail("State != streaming but warmuped")

            if reader.state == State.STREAMING and reader.frames_processed > 0:
                assert reader.warmuped, True
                break

            time.sleep(0.001)


def test_read_latest_blocks_until_frame(videos, reader_cls):

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        t0 = time.monotonic()
        frame = reader.read_latest(timeout=5.0)
        assert frame is not None
        assert time.monotonic() - t0 < 5.0  # вернулся раньше таймаута


def test_read_latest_wakes_on_stop(videos, reader_cls):

    with reader_context(
        reader_cls, source=videos["30fps_10s.mp4"], name=reader_cls.__name__
    ) as reader:
        reader.read_latest(timeout=2.0)

        import threading

        threading.Timer(0.2, reader.stop).start()
        t0 = time.monotonic()
        reader.read_latest(timeout=10.0)
        assert time.monotonic() - t0 < 2.0


def test_custom_ffmpeg_args_stored(videos, reader_cls):
    opts = {"rtsp_transport": "udp", "max_delay": "500000"}

    with reader_context(
        reader_cls,
        source=videos["60fps_10s.mp4"],
        av_container_options=opts,
        name=reader_cls.__name__,
    ) as reader:
        assert reader._av_container_options == opts
        reader.stop()


def test_custom_args_override_defaults(videos, reader_cls):

    custom = {"probesize": "5000000"}
    with reader_context(
        reader_cls,
        source="rtsp://fake",
        av_container_options=custom,
        name=reader_cls.__name__,
    ) as reader:
        assert reader._av_container_options == custom
        assert "probesize" in reader._av_container_options
        assert reader._av_container_options["probesize"] == "5000000"
        reader.stop()
