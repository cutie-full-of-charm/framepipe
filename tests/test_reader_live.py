import os
import platform
import signal
import time

import pytest
from loguru import logger

from framepipe import State

from .fixtures.streaming import wait_stream_ready
from .fixtures.utils import (
    reader_context,
    wait_for_frame,
    wait_for_state,
)


def assert_live(reader, timeout=2.0):
    assert reader.is_alive()
    assert reader._is_live
    assert reader.state == State.STREAMING

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if reader.read_new() is not None:
            return
        time.sleep(0.01)
    pytest.fail(f"no new frame within {timeout}s after recovery")


def test_is_live(publish_vid, videos, reader_cls):
    label = reader_cls.__name__
    stream_link = f"stream_{label}"

    proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
    wait_stream_ready(stream_link)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        wait_for_frame(reader, 1)

        assert reader._is_live is True, "RTSP-source must be True here"
        assert reader.is_alive()


# FIXME: FAILED tests/test_reader_live.py::test_drop_recover[WrappedSyncReader-4-10] - assert 0.9333333333333333 > 1.0
def test_drop_recover(publish_vid, videos, reader_cls):

    label = reader_cls.__name__
    stream_link = f"stream_{label}"

    proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
    wait_stream_ready(stream_link)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        last_pts_before_drop = reader.last_pts_seconds
        last_recv_before_drop = reader.last_recv_time
        last_frames_before_drop = reader.frames_processed

        proc.terminate()
        logger.info("Process terminated, means camera drop")

        proc.wait(timeout=3)
        time.sleep(2.0)

        proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
        wait_stream_ready(stream_link)

        start = time.time()
        logger.info("Process started, means camera returned")

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert reader.frames_processed > last_frames_before_drop
        assert reader.last_pts_seconds > last_pts_before_drop
        assert reader.last_recv_time > last_recv_before_drop

        assert_live(reader)


# freeze, подразумневается, что сокет отркыт, но получаемых кадров нет
# FIXME: !!!
#       FAILED tests/test_reader_live.py::test_freeze_recover[WrappedReader] - assert 0.9333333333333333 == 0.9
#       ERROR tests/test_reader_live.py::test_freeze_recover[WrappedReader] - TimeoutError: RTSP publisher did not exit in time
def test_freeze_recover(publish_vid, videos, reader_cls):
    if platform.system() != "Linux":
        pytest.skip(f"OS is not Linux, OS: {platform.system()}")

    label = reader_cls.__name__
    stream_link = f"stream_{label}"

    proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
    wait_stream_ready(stream_link)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        last_pts_before_freeze = reader.last_pts_seconds
        last_recv_before_freeze = reader.last_recv_time
        last_frames_before_freeze = reader.frames_processed

        os.kill(
            proc.pid, signal.SIGSTOP
        )  # FIXME: это достаточно хорошая эмуляция фриза?
        time.sleep(2.0)

        assert reader.read_new() is not None
        assert reader.is_alive() in (True, False)  # NOT NONE
        assert reader.last_pts_seconds == last_pts_before_freeze  # ??
        assert reader.last_recv_time == last_recv_before_freeze  # ??

        start = time.time()
        os.kill(proc.pid, signal.SIGCONT)

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert reader.frames_processed > last_frames_before_freeze
        assert reader.last_pts_seconds > last_pts_before_freeze
        assert reader.last_recv_time > last_recv_before_freeze

        assert_live(reader)


# flap означает мигание потока, проверка на зомби процессы
def test_flap_recover(publish_vid, videos, reader_cls):
    label = reader_cls.__name__
    stream_link = f"stream_{label}"

    proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
    wait_stream_ready(stream_link)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        for _ in range(3):
            proc.terminate()
            proc.wait(timeout=3)
            time.sleep(0.5)  # поток отключен половину секунды
            proc, link = publish_vid(stream_link, videos["30fps_10s.mp4"])
            time.sleep(1.0)  # потом включен секунду

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# PyAV перекодирует видео под поток, добавив PTS-джиттер
def test_ts_jitter(publish_vid, videos, reader_cls):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"], ts_jitter_ms=0.08)
    wait_stream_ready(stream)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        wait_for_frame(reader, 1)

        last = -1.0
        regressions = 0
        for _ in range(60):
            if reader.read_new() is not None:
                pts = reader.last_pts_seconds
                assert pts is not None
                if pts < last - 0.001:
                    regressions += 1
                last = max(last, pts)
            time.sleep(0.02)

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# камера отдает кадры медленнее реального времени? не уверен, что тут получится в итоге
def test_slow_camera(publish_vid, videos, reader_cls):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"], real_rate=0.7)
    wait_stream_ready(stream)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# FIXME: перепроверить перекодирование PyAV-потока
def test_camera_delivers_low_fps(publish_vid, videos, reader_cls):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"], deliver_fps=15)
    wait_stream_ready(stream)

    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# @pytest.mark.netem
def test_netem_no_dup_frames(publish_vid, videos, reader_cls, netem):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"])
    wait_stream_ready(stream)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        wait_for_frame(reader, 1)

        with netem(delay_ms=50, duplicate_packets_percent=5):
            seen_pts = []
            for _ in range(40):
                if reader.read_new() is not None:
                    pts = reader.last_pts_seconds
                    assert pts not in seen_pts, (
                        f"netem frame duplicate: PTS {pts}"
                    )
                    seen_pts.append(pts)
                time.sleep(0.05)

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# @pytest.mark.netem
def test_jitter_no_false_drop(publish_vid, videos, reader_cls, netem):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"])
    wait_stream_ready(stream)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")
        wait_for_frame(reader, 1)

        with netem(delay_ms=200, jitter_ms=80):
            wait_for_frame(reader, reader.frames_processed + 5, timeout=3)

            assert reader.state == State.STREAMING, (
                "Reader's state != STREAMING "
            )

            prev = -1.0
            for _ in range(30):
                if reader.read_new() is not None:
                    pts = reader.last_pts_seconds
                    assert pts >= prev, f"pts error: {pts} < {prev}"
                    prev = pts
                time.sleep(0.05)

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert_live(reader)


# @pytest.mark.netem
def test_packet_loss(publish_vid, videos, reader_cls, netem):
    label = reader_cls.__name__
    stream = f"stream_{label}"

    proc, link = publish_vid(stream, videos["30fps_10s.mp4"])
    wait_stream_ready(stream)

    start = time.time()
    with reader_context(reader_cls, source=link, name=label) as reader:
        wait_for_state(reader, State.STREAMING)
        logger.info(f"{label} streaming: {time.time() - start:.3f}")

        wait_for_frame(reader, 1)

        last_pts_before_loss = reader.last_pts_seconds
        last_recv_before_loss = reader.last_recv_time
        last_frames_before_loss = reader.frames_processed

        with netem(
            delay_ms=50, loss_packets_percent=5, reorder_packets_percent=10
        ):
            wait_for_frame(reader, 5)
            assert reader.read_new() is not None
            assert reader.is_alive()

        wait_for_state(reader, State.STREAMING)
        wait_for_frame(reader, reader.frames_processed + 16)

        assert reader.frames_processed > last_frames_before_loss
        assert reader.last_pts_seconds > last_pts_before_loss
        assert reader.last_recv_time > last_recv_before_loss

        assert_live(reader)


# TODO: тесты синхронизации с дефектами сети
