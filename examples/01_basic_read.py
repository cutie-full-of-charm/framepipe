import argparse
import time

from framepipe import OutputFormat, Reader


def main(source) -> None:
    reader = Reader(source=source, name="reader_example01")
    consumed = 0
    t0 = time.monotonic()
    try:
        while reader.is_opened():
            frame = reader.read_new_frame()
            if frame is None:
                continue

            consumed += 1

            if consumed == 1:
                if reader._out_format == OutputFormat.NATIVE:
                    planes = frame.planes()
                    print(
                        f"Received frame: OutputFormat.Native, planes_shape={[p.shape for p in planes]}, pix_fmt={frame.pix_fmt}"
                    )
                elif frame.pix_fmt != "gray":
                    h, w, c = frame.array().shape
                    print(
                        f"First frame: {w}x{h}x{c} dtype={frame.array().dtype} fmt={reader.frame_pix_fmt}"
                    )
                else:
                    h, w = frame.array().shape
                    print(
                        f"First frame: {w}x{h} dtype={frame.array().dtype} fmt={reader.frame_pix_fmt}"
                    )

            if consumed % 30 == 0:
                print(
                    f"consumed={consumed} decoded={reader.frames_processed} "
                    f"pts={reader.last_pts_seconds:.3f}s state={reader.state}"
                )

    except KeyboardInterrupt:
        print(" KeyboardInterrupt")
    finally:
        delta_time = time.monotonic() - t0
        print(
            f"Finished: consumed={consumed} decoded={reader.frames_processed} "
            f"packets={reader.packets_processed} in {delta_time:.3f}s, "
            f"pts time is {reader.last_pts_seconds}s, state={reader.state}",
            flush=True,
        )
        reader.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=str, help="Path to videofile")

    args = parser.parse_args()
    main(source=args.source)
