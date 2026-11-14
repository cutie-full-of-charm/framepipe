import argparse
import sys
import time

from framepipe import OutputFormat, Reader, State


# NOTE: getting frames in their native format
def grab_native(source):
    reader = Reader(
        source, name=f"reader_example03", out_format=OutputFormat.NATIVE
    )

    deadline = time.monotonic() + 3.0
    while reader.state != State.STREAMING:
        if time.monotonic() > deadline:
            raise RuntimeError("Can't get frame in time")
        time.sleep(0.001)

    frame = reader.read_new_frame()
    reader.stop()
    if frame is not None:
        return frame

    return None


def grab_one(fmt: OutputFormat, source):
    reader = Reader(source, name=f"reader_example03", out_format=fmt)

    deadline = time.monotonic() + 3.0
    while reader.state != State.STREAMING:
        if time.monotonic() > deadline:
            raise RuntimeError("Can't get frame in time")
        time.sleep(0.001)

    frame = reader.read_new_frame()
    reader.stop()
    if frame is not None:
        return frame.array(), frame.pix_fmt

    return None, "none"


def main(source) -> None:
    for fmt in (OutputFormat.BGR24, OutputFormat.RGB24, OutputFormat.GRAY8):
        frame, pix = grab_one(fmt, source)
        if frame is None:
            print(f"{fmt}: frame not received")
            continue
        print(
            f"Received frame: {fmt}, shape={frame.shape}, dtype={frame.dtype}, pix_fmt={pix}"
        )

    # NATIVE format
    native_frame = grab_native(source)
    if native_frame is None:
        print(f"{fmt}: frame not received")
        return

    planes = native_frame.planes()
    print(
        f"Received frame: OutputFormat.Native, planes_shape={[p.shape for p in planes]}, pix_fmt={native_frame.pix_fmt}"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=str, help="Path to videofile")

    args = parser.parse_args()
    main(source=args.source)
