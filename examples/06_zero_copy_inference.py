import argparse
import statistics
import time

import torch
import torchvision

from framepipe import HWAccel, MemoryType, OutputFormat, PostProcessor, Reader

weights = torchvision.models.ResNet18_Weights.DEFAULT
model = torchvision.models.resnet18(weights=weights).cuda().eval()

mean = torch.tensor([0.485, 0.456, 0.406], device="cuda").view(1, 3, 1, 1)
std = torch.tensor([0.229, 0.224, 0.225], device="cuda").view(1, 3, 1, 1)
categories = weights.meta["categories"]


def main(source):
    reader = Reader(
        name="reader_example06",
        source=source,
        hwaccel=HWAccel.CUDA,
        out_format=OutputFormat.RGB24,
        postprocessor=PostProcessor.CUDA,
        output_memory=MemoryType.GPU,
        out_w=224,
        out_h=224,
        realtime_file=False,
        auto_start=False,
        on_demand=True,
    )

    # Warmup before starting the decoder
    with torch.inference_mode():
        dummy = torch.randn(1, 3, 224, 224, device="cuda")
        for _ in range(10):
            model(dummy)
        torch.cuda.synchronize()

    # Processing video
    # NOTE: here we're using on_demand mode, and we’re using framepipe's reader
    #       differently - we read frame by frame strictly on command,
    #       without skipping a single frame due to long processing,
    #       but also without idling while waiting for the PTS interval
    #       embedded in the video file.
    #       I guess this is a best reading model for local video inferences
    processed = 0
    frame_times = []
    t0 = time.monotonic()

    try:
        reader.start()
        while reader.is_opened():
            t1 = time.monotonic()
            frame = reader.next_frame()

            if frame is None:
                time.sleep(0.001)
                continue

            with torch.inference_mode():
                torch.cuda.synchronize()

                x = torch.from_dlpack(frame.dlpack())

                x = x.permute(2, 0, 1).unsqueeze(0)
                x = x.float().div_(255.0)

                x = (x - mean) / std

                y = model(x)

                probs = torch.softmax(y, dim=1)
                confidence, class_id = probs.max(dim=1)

                torch.cuda.synchronize()
                frame_duration = time.monotonic() - t1

            frame_times.append(frame_duration)

            c_id = class_id.item()
            conf = confidence.item()
            class_name = categories[c_id]

            print(
                f"[ frame={frame.id:<3} ] -> {class_name:<25} ( {conf:>7.3%} ) time={frame_duration:.4f}"
            )

            processed += 1

    except KeyboardInterrupt:
        print(" KeyboardInterrupt")
    finally:
        t_f = time.monotonic() - t0
        print(f"\nTotal process time={t_f:.4f}", flush=True)
        reader.stop()

    print(f"Processed frames={processed}")

    if processed > 0:
        frame_times_ms = [t * 1000 for t in frame_times]
        quantiles = statistics.quantiles(frame_times_ms, n=100)

        print(f"p50 (Median) latency: {quantiles[49]:.1f}ms")
        print(f"p90 latency:          {quantiles[89]:.1f}ms")
        print(f"p95 latency:          {quantiles[94]:.1f}ms")
        print(f"p99 latency:          {quantiles[98]:.1f}ms")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=str, help="Path to videofile")

    args = parser.parse_args()
    main(source=args.source)
