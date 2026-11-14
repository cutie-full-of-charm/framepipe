import argparse
import time

import cv2

from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, Reader

_MAP_FORMAT = {
    "rgb24": OutputFormat.RGB24,
    "bgr24": OutputFormat.BGR24,
    "gray": OutputFormat.GRAY8,
}
_MAP_HW = {
    "software": HWAccel.SOFTWARE,
    "cuda": HWAccel.CUDA,
}
_MAP_POSTPROCESSOR = {"cpu": PostProcessor.CPU, "cuda": PostProcessor.CUDA}


def main(source, scale, hwaccel, format, postprocessor) -> None:

    fmt = _MAP_FORMAT[format]
    decode_hw = _MAP_HW[hwaccel]
    frame_postprocessor = _MAP_POSTPROCESSOR[postprocessor]
    frame_output_memory = MemoryType.CPU

    # NOTE: if you have selected hwaccel=cuda and postprocessor=cuda,
    #       the color conversion, which in this case will be nv12 -> rgb/bgr/gray,
    #       as well as resize, will also be performed on the GPU,
    #       which is a tiny load for it (and can be very significant for the CPU).
    #       Keep in mind that in order to display a frame on the screen,
    #       it must pass through the PCI‑E bus;
    #       if you only perform color conversion, skipping scale,
    #       you are transmitting more data through it
    #       than if you were transmitting frames in the OutputFormat.NATIVE
    reader = Reader(
        source=source,
        name="reader_example05",
        out_format=fmt,
        hwaccel=decode_hw,
        postprocessor=frame_postprocessor,
        output_memory=frame_output_memory,
        # you can also provide the ecxact final size here.
        # SWS_BILINEAR is used for any resize
        # out_w=640,
        # out_h=640,
        scale=scale,
        realtime_file=True,
    )

    win_name = "example05"
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
    parser.add_argument("hwaccel", choices=["software", "cuda"])
    parser.add_argument("postprocessor", choices=["cpu", "cuda"])
    parser.add_argument("format", choices=["rgb24", "bgr24", "gray"])
    parser.add_argument("scale", type=float)

    args = parser.parse_args()
    main(
        source=args.source,
        hwaccel=args.hwaccel,
        scale=args.scale,
        format=args.format,
        postprocessor=args.postprocessor,
    )
