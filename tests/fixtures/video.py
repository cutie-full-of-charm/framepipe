import os
import sys
from fractions import Fraction

import av

import numpy as np
from loguru import logger
from matplotlib import font_manager
from PIL import Image, ImageDraw, ImageFont
from tqdm import tqdm

from .utils import VIDEO_CACHE_DIR

CODEC_PRESETS = {
    "h264": {
        "vcodec": "libx264",
        "ext": "mp4",
        "extra": ["-preset", "fast", "-crf", "18", "-pix_fmt", "yuv420p"],
        "desc": "H.264 / AVC (MP4)",
    },
    "h265": {
        "vcodec": "libx265",
        "ext": "mp4",
        "extra": ["-preset", "fast", "-crf", "20", "-pix_fmt", "yuv420p"],
        "desc": "H.265 / HEVC (MP4)",
    },
    "vp8": {
        "vcodec": "libvpx",
        "ext": "webm",
        "extra": ["-b:v", "8M", "-pix_fmt", "yuv420p"],
        "desc": "VP8 (WebM)",
    },
    "vp9": {
        "vcodec": "libvpx-vp9",
        "ext": "webm",
        "extra": ["-b:v", "0", "-crf", "30", "-pix_fmt", "yuv420p"],
        "desc": "VP9 (WebM)",
    },
    "av1": {
        "vcodec": "libsvtav1",
        "ext": "mp4",
        "extra": ["-preset", "6", "-crf", "30", "-pix_fmt", "yuv420p"],
        "desc": "AV1 / SVT-AV1 (MP4)",
    },
    "prores": {
        "vcodec": "prores_ks",
        "ext": "mov",
        "extra": ["-profile:v", "3", "-pix_fmt", "yuv422p10le"],
        "desc": "Apple ProRes 422 HQ (MOV)",
    },
    "mjpeg": {
        "vcodec": "mjpeg",
        "ext": "avi",
        "extra": ["-q:v", "2", "-pix_fmt", "yuvj420p"],
        "desc": "Motion JPEG (AVI)",
    },
}


def _make_font(size):
    try:
        prop = font_manager.FontProperties(weight="normal", style="normal")
        font_path = font_manager.findfont(prop)

        if font_path:
            try:
                return ImageFont.truetype(font_path, size)
            except Exception:
                pass

        return ImageFont.load_default()
    except Exception:
        return None


def render_frame(
    current_time: float,
    width: int,
    height: int,
):

    img = Image.new("RGB", (width, height), (245, 245, 250))
    draw = ImageDraw.Draw(img)

    mins = int(current_time) // 60
    secs = int(current_time) % 60
    ms = int((current_time % 1) * 1000)
    time_str = f"{mins:02d}:{secs:02d}.{ms:03d}"

    font_size = max(40, width // 8)
    font = _make_font(font_size)

    cx, cy = width // 2, height // 2

    shadow_offset = max(2, width // 400)
    draw.text(
        (cx + shadow_offset, cy + shadow_offset),
        time_str,
        font=font,
        fill=(180, 180, 200),
        anchor="mm",
    )
    draw.text((cx, cy), time_str, font=font, fill=(20, 20, 30), anchor="mm")

    label_font = _make_font(max(12, width // 80))
    draw.text(
        (width - 10, height - 10),
        f"t={current_time:.4f}s",
        font=label_font,
        fill=(100, 100, 120),
        anchor="rb",
    )

    return np.array(img)


def generate_video(
    output=None, codec="h264", duration=10, resolution="800x600", fps=30
):
    codec = codec.lower()
    if codec not in CODEC_PRESETS:
        print(f"Error: codec '{codec}' not supported.")
        print(f"Available: {', '.join(CODEC_PRESETS.keys())}")
        sys.exit(1)

    preset = CODEC_PRESETS[codec]
    output = (
        output
        if output is not None
        else f"timer_{duration}s_{fps}fps_{codec}.{preset['ext']}"
    )

    total_frames = int(duration * fps)
    w, h = map(int, resolution.split("x"))

    # print(f"  Output      : {output}")
    # print(f"  Codec       : {preset['desc']}")
    # print(f"  Resolution  : {w}×{h}")
    # print(f"  FPS         : {fps}")
    # print(f"  Duration    : {duration} secs")
    # print(f"  Frames      : {total_frames}")

    # PyAV использует библиотеки FFmpeg напрямую, без запуска ffmpeg CLI.
    options = {
        "h264": {"preset": "fast", "crf": "18"},
        "h265": {"preset": "fast", "crf": "20"},
        "vp8": {"b": "8M"},
        "vp9": {"b": "0", "crf": "30"},
        "av1": {"preset": "6", "crf": "30"},
        "prores": {"profile": "3"},
        "mjpeg": {"qscale": "2"},
    }[codec]
    formats = {
        "prores": "yuv422p10le",
        "mjpeg": "yuvj420p",
    }
    try:
        with av.open(str(output), mode="w") as container:
            stream = container.add_stream(preset["vcodec"], rate=fps, options=options)
            stream.width = w
            stream.height = h
            stream.pix_fmt = formats.get(codec, "yuv420p")
            stream.gop_size = fps
            with tqdm(total=total_frames, unit="frames", unit_scale=True) as pb:
                for i in range(total_frames):
                    pixels = render_frame(i / fps, w, h)
                    frame = av.VideoFrame.from_ndarray(pixels, format="rgb24")
                    frame.pts = i
                    frame.time_base = Fraction(1, fps)
                    for packet in stream.encode(frame):
                        container.mux(packet)
                    pb.update(1)
            for packet in stream.encode(None):
                container.mux(packet)
    except Exception as exc:
        raise RuntimeError(f"PyAV video generation failed: {output}") from exc

    size_mb = os.path.getsize(output) / 1_048_576
    logger.success(f"Video generated: {output} ({size_mb:.2f} mb)\n")


def make_offset_copy(src, dst, offset):
    """Remux without re-encoding and shift stream timestamps by *offset* seconds."""
    with av.open(str(src)) as source, av.open(str(dst), mode="w") as target:
        mapping = {}
        for stream in source.streams:
            # PyAV's stream-template API copies codec parameters for stream copy.
            mapping[stream.index] = target.add_stream_from_template(stream)
        for packet in source.demux():
            if packet.dts is None and packet.pts is None:
                continue  # demuxer flush packet
            output_stream = mapping[packet.stream.index]
            tb = packet.time_base or packet.stream.time_base
            delta = round(Fraction(str(offset)) / tb)
            if packet.pts is not None:
                packet.pts += delta
            if packet.dts is not None:
                packet.dts += delta
            packet.stream = output_stream
            target.mux(packet)
    return dst


def probe_fps(path):
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        rate = stream.average_rate or stream.base_rate or stream.guessed_rate
        return float(rate) if rate is not None else 0.0


# FIXME: добавить также видео, где GOP между i-frame будет не 30 кадров (~1сек.)
# а 2/4 секунды, как с этим справится ридер?


# videoset:
# 1) 30fps 10sec        30fps_10s
# 2) 60fps 10sec        60fps_10s
# 3) 25fps 10sec        25fps_10s
# 4) 30fps 30sec        30fps_30s
# 5) 30fps 5 sec        30fps_5s
# 6) 1 + offset 2 sec   30fps_10s_offset_2s
# 7) 1 + offset 4 sec   30fps_10s_offset_4s
# 8) 2 + offset 2 sec   60fps_10s_offset_2s
# 9) 4 + offset 10sec   30fps_30s_offset_4s
def gen_videoset():
    if not VIDEO_CACHE_DIR.exists():
        VIDEO_CACHE_DIR.mkdir(parents=True, exist_ok=True)

    # 1) 30fps 10sec        30fps_10s
    vid_path = VIDEO_CACHE_DIR / "30fps_10s.mp4"
    if not vid_path.exists():
        generate_video(vid_path, fps=30, duration=10)

    # 2) 60fps 10sec        60fps_10s
    vid_path = VIDEO_CACHE_DIR / "60fps_10s.mp4"
    if not vid_path.exists():
        generate_video(vid_path, fps=60, duration=10)

    # 3) 25fps 10sec        25fps_10s
    vid_path = VIDEO_CACHE_DIR / "25fps_10s.mp4"
    if not vid_path.exists():
        generate_video(vid_path, fps=25, duration=10)

    # 4) 30fps 30sec        30fps_30s
    vid_path = VIDEO_CACHE_DIR / "30fps_30s.mp4"
    if not vid_path.exists():
        generate_video(vid_path, fps=30, duration=30)

    # 5) 30fps 5 sec        30fps_5s
    vid_path = VIDEO_CACHE_DIR / "30fps_5s.mp4"
    if not vid_path.exists():
        generate_video(vid_path, fps=30, duration=5)

    # 6) 1 + offset 2 sec   30fps_10s_offset_2s
    src_path = VIDEO_CACHE_DIR / "30fps_10s.mp4"
    dest_path = VIDEO_CACHE_DIR / "30fps_10s_offset_2s.mp4"
    offset = 2
    if not dest_path.exists():
        make_offset_copy(src_path, dest_path, offset)

    # 7) 1 + offset 4 sec   30fps_10s_offset_4s
    src_path = VIDEO_CACHE_DIR / "30fps_10s.mp4"
    dest_path = VIDEO_CACHE_DIR / "30fps_10s_offset_4s.mp4"
    offset = 4
    if not dest_path.exists():
        make_offset_copy(src_path, dest_path, offset)

    # 8) 2 + offset 2 sec   60fps_10s_offset_2s
    src_path = VIDEO_CACHE_DIR / "60fps_10s.mp4"
    dest_path = VIDEO_CACHE_DIR / "60fps_10s_offset_2s.mp4"
    offset = 2
    if not dest_path.exists():
        make_offset_copy(src_path, dest_path, offset)

    # 9) 4 + offset 10sec   30fps_30s_offset_10s
    src_path = VIDEO_CACHE_DIR / "30fps_30s.mp4"
    dest_path = VIDEO_CACHE_DIR / "30fps_30s_offset_10s.mp4"
    offset = 10
    if not dest_path.exists():
        make_offset_copy(src_path, dest_path, 10)


def videoset_exists():
    if not VIDEO_CACHE_DIR.exists():
        return False
    return True
