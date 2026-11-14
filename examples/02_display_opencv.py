import argparse
import time

import cv2

from framepipe import Reader


def main(source) -> None:
    # by default realtime_file is False and video will be decoded as fast as possible,
    # otherwise, decoder will wait for the interframe interval (pts) of the video file.
    # NOTE: realtime_file implementation is absolutely naive (1ms polling),
    #       it may cause issues in some cases
    reader = Reader(
        source=source,
        name="reader_example02",
        realtime_file=True,
    )

    win_name = "example02"
    t0 = time.monotonic()
    try:
        while reader.is_opened():
            frame = reader.read_new()
            if frame is None:
                continue

            cv2.imshow(win_name, frame)
            key = cv2.waitKey(1) & 0xFF
            if key in (27, ord("q")):
                break

    except KeyboardInterrupt:
        print(" KeyboardInterrupt")
    finally:
        delta_time = time.monotonic() - t0
        print(f"Finished in {delta_time:.3f}s", flush=True)
        cv2.destroyAllWindows()
        reader.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=str, help="Path to videofile")

    args = parser.parse_args()
    main(source=args.source)
