import argparse
import time

from framepipe import CpuPackedFrame, CpuPlanarFrame, Reader


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
                if isinstance(frame, CpuPlanarFrame):
                    planes = frame.planes()
                    print(
                        f"First frame: CpuPlanarFrame (native), "
                        f"planes_shape={[p.shape for p in planes]}, "
                        f"pix_fmt={frame.pix_fmt}"
                    )

                elif isinstance(frame, CpuPackedFrame):
                    array = frame.array()

                    if array.ndim == 3:
                        h, w, c = array.shape
                        print(
                            f"First frame: packed (rgb24/bgr24) {w}x{h}x{c} "
                            f"dtype={array.dtype} "
                            f"fmt={reader.frame_pix_fmt}"
                        )
                    elif array.ndim == 2:
                        h, w = array.shape
                        print(
                            f"First frame: packed gray8 {w}x{h} "
                            f"dtype={array.dtype} "
                            f"fmt={reader.frame_pix_fmt}"
                        )
                    else:
                        raise RuntimeError(
                            "Somehow received frame with ndims != 2 or 3"
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
