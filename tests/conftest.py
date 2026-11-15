import contextlib
import os
import shutil
import subprocess
import time

import pytest
from loguru import logger

from framepipe import Reader

from .fixtures.mediamtx import check_mediamtx, setup_mediamtx
from .fixtures.netem import netem_available, network_corrupt
from .fixtures.publisher import Publisher
from .fixtures.streaming import RTSP_LINK, mediamtx_reachable
from .fixtures.utils import (
    MEDIAMTX_DIR,
    MEDIAMTX_EXECUTABLE,
    VIDEO_CACHE_DIR,
)
from .fixtures.video import gen_videoset, test_videoset_exists


@pytest.fixture(params=[Reader], ids=lambda c: c.__name__)
def reader_cls(request):
    return request.param


@pytest.fixture(scope="session")
def run_mediamtx():
    mediamtx_exists = check_mediamtx()
    if not mediamtx_exists:
        if MEDIAMTX_DIR.exists():
            shutil.rmtree(MEDIAMTX_DIR)

        inst = setup_mediamtx()
        assert inst, True

    mediamtx_proc = subprocess.Popen(
        [str(MEDIAMTX_DIR / MEDIAMTX_EXECUTABLE)],
        stdout=subprocess.DEVNULL,
        cwd=MEDIAMTX_DIR,
    )

    for _ in range(50):
        ok, _ = mediamtx_reachable()
        if ok:
            break
        time.sleep(0.1)

    # отдаем процесс, чтобы сохранить возможность его закрыть, иначе будут
    # висеть 20 штук
    yield mediamtx_proc
    try:
        mediamtx_proc.terminate()
        mediamtx_proc.wait(timeout=5)
    except Exception:
        mediamtx_proc.kill()


@pytest.fixture(scope="session")
def require_netem():
    ok, ret = netem_available()
    if not ok:
        logger.error(f"Can't get netem: {ret}")
        pytest.skip("netem unavailable")


@pytest.fixture(scope="session", autouse=True)
def videos():
    if test_videoset_exists():
        logger.success("videoset found, generation skipped")
    else:
        logger.info("generating videoset")
        gen_videoset()

    videos = {
        filename: os.path.join(VIDEO_CACHE_DIR, filename)
        for filename in os.listdir(VIDEO_CACHE_DIR)
    }
    return videos


@pytest.fixture(scope="function")
def require_mediamtx(run_mediamtx):
    ok, ret = mediamtx_reachable()
    if not ok:
        logger.error(f"Can't reach mediamtx: {ret}")
        pytest.skip("MediaMTX unavailable")

    # отдаем процесс, если в каком-то тесте понадобится проверить ситуацию
    # из разряда, "а что будет если мы убьем mediamtx в середине теста"
    yield run_mediamtx


@pytest.fixture
def netem():
    ok, ret = netem_available()
    if not ok:
        pytest.skip(f"netem unavailable: {ret}")
    return network_corrupt


@pytest.fixture(scope="function")
def publish_vid(require_mediamtx):
    active_publishers = []

    def _publish(
        stream,
        src,
        ts_jitter_ms=0.0,
        real_rate=0.0,
        deliver_fps=0,
    ):
        rtsp_link = RTSP_LINK + "/" + stream
        pub = Publisher(
            src,
            rtsp_link,
            jitter_ms=ts_jitter_ms,
            real_rate=real_rate,
            deliver_fps=deliver_fps,
        )
        active_publishers.append(pub)
        return pub, rtsp_link

    yield _publish

    for publisher in active_publishers:
        with contextlib.suppress(ProcessLookupError):
            publisher.terminate()
            publisher.wait(timeout=3)
