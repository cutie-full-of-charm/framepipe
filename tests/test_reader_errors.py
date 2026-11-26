import time

from framepipe import HWAccel, PostProcessor, Reader


def test_fatal_pipeline_error(videos):
    reader = Reader(
        source=videos["30fps_10s.mp4"],
        name="fatal-test",
        hwaccel=HWAccel.SOFTWARE,
        postprocessor=PostProcessor.CUDA,
    )

    try:
        deadline = time.monotonic() + 5.0

        while reader.is_opened():
            assert time.monotonic() < deadline, (
                "Reader did not terminate after fatal error"
            )
            reader.read_latest_frame(timeout=0.1)

        exc = reader.get_last_exception()

        assert exc is not None
        assert "CUDA" in str(exc) and "requires CUDA decoding" in str(exc)

        print(f"Captured exception: {exc}")

    finally:
        reader.stop()
