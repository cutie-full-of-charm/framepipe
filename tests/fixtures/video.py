import os
import sys
from fractions import Fraction
from pathlib import Path

import av
import numpy as np
from loguru import logger
from tqdm import tqdm

from .utils import VIDEO_CACHE_DIR

MATRICES = {
    "bt709": dict(
        kr=0.2126,
        kb=0.0722,
        cs=1,
        prim=1,
        trc=1,
        xprim="bt709",
        xtrc="bt709",
        xmat="bt709",
    ),
    "bt601": dict(
        kr=0.299,
        kb=0.114,
        cs=6,
        prim=6,
        trc=6,
        xprim="smpte170m",
        xtrc="smpte170m",
        xmat="smpte170m",
    ),
    "bt470bg": dict(
        kr=0.299,
        kb=0.114,
        cs=5,
        prim=5,
        trc=5,
        xprim="bt470bg",
        xtrc="bt470bg",
        xmat="bt470bg",
    ),
    "bt2020": dict(
        kr=0.2627,
        kb=0.0593,
        cs=9,
        prim=9,
        trc=14,
        xprim="bt2020",
        xtrc="bt2020-10",
        xmat="bt2020nc",
    ),
    "smpte240m": dict(
        kr=0.212,
        kb=0.087,
        cs=7,
        prim=7,
        trc=7,
        xprim="smpte240m",
        xtrc="smpte240m",
        xmat="smpte240m",
    ),
}

CODEC_PRESETS = {
    "h264": {
        "vcodec": "libx264",
        "ext": "mp4",
        "format": "mp4",
        "options": {"preset": "fast", "crf": "18"},
        "desc": "H.264 / AVC (MP4)",
    },
    "h265": {
        "vcodec": "libx265",
        "ext": "mp4",
        "format": "mp4",
        "options": {"preset": "fast", "crf": "20"},
        "desc": "H.265 / HEVC (MP4)",
    },
    "vp9": {
        "vcodec": "libvpx-vp9",
        "ext": "webm",
        "format": "webm",
        "options": {"b": "0", "crf": "30", "cpu-used": "4", "row-mt": "1"},
        "desc": "VP9 (WebM)",
    },
}

PIX_FMTS = (
    "yuv420p",
    "yuv422p",
    "yuv444p",
    "yuv420p10le",
    "yuv422p10le",
    "yuv444p10le",
)

RESOLUTIONS = ["640x360", "1280x720", "1920x1080", "3840x2160"]


def _chroma_mode(pix_fmt: str) -> str:
    for mode in ("444", "422"):
        if mode in pix_fmt:
            return mode
    return "420"


def make_bars(w: int, bh: int):
    cols = np.array(
        [
            [1, 1, 1],
            [1, 1, 0],
            [0, 1, 1],
            [0, 1, 0],
            [1, 0, 1],
            [1, 0, 0],
            [0, 0, 1],
            [0, 0, 0],
        ],
        np.float32,
    )
    idx = np.minimum(np.arange(w) * len(cols) // w, len(cols) - 1)
    return np.broadcast_to(cols[idx][None], (bh, w, 3)).copy()


def _encoder_options(codec: str, matrix: str, full: bool, gop: int, bframes):
    m = MATRICES[matrix]
    options = {**CODEC_PRESETS[codec]["options"], "g": str(gop)}
    if bframes is not None:
        options["bf"] = str(bframes)
    if codec == "h264":
        options["x264-params"] = (
            f"scenecut=0:colorprim={m['xprim']}:transfer={m['xtrc']}:"
            f"colormatrix={m['xmat']}:range={'pc' if full else 'tv'}"
        )
    elif codec == "h265":
        options["x265-params"] = (
            f"scenecut=0:log-level=error:colorprim={m['xprim']}:transfer={m['xtrc']}:"
            f"colormatrix={m['xmat']}:range={'full' if full else 'limited'}"
        )
    return options


def _set_color_tags(codec_context, matrix: str, full: bool):
    m = MATRICES[matrix]
    vals = {
        "colorspace": m["cs"],
        "color_primaries": m["prim"],
        "color_trc": m["trc"],
        "color_range": 2 if full else 1,
    }
    for name, val in vals.items():
        try:
            setattr(codec_context, name, val)
        except Exception:
            logger.warning(f"can't set codec_context.{name}")


def render_frame(i: int, xs, ys, rng, noise: float, bars):
    """Движущийся цветной градиент + яркостный гауссов шум -> float32 RGB [0..1], (h, w, 3)."""
    h, w = len(ys), len(xs)
    ph = (i * 0.01) % 1.0
    img = np.empty((h, w, 3), np.float32)
    img[..., 0] = ((xs + ph) % 1.0)[None, :]
    img[..., 1] = ys[:, None]
    img[..., 2] = (
        0.5 * (1.0 - ((xs + 2 * ph) % 1.0))[None, :] + 0.5 * (1.0 - ys)[:, None]
    )
    if noise > 0:
        img += rng.standard_normal((h, w, 1), dtype=np.float32) * (
            noise / 255.0
        )
    np.clip(img, 0.0, 1.0, out=img)
    if bars is not None:
        img[: bars.shape[0]] = bars
    return img


def rgb_to_yuv(
    rgb, kr: float, kb: float, full: bool, bits: int, chroma: str = "420"
):
    """RGB float -> (Y, U, V)"""

    def _subsample(c, mode: str):
        if mode == "444":
            return c
        if c.shape[1] & 1:
            c = np.hstack([c, c[:, -1:]])
        if mode == "422":
            return c.reshape(c.shape[0], c.shape[1] // 2, 2).mean(2)
        if c.shape[0] & 1:
            c = np.vstack([c, c[-1:]])
        return c.reshape(c.shape[0] // 2, 2, c.shape[1] // 2, 2).mean((1, 3))

    kg = 1.0 - kr - kb
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = kr * r + kg * g + kb * b
    cb = _subsample((b - y) / (2.0 * (1.0 - kb)), chroma)
    cr = _subsample((r - y) / (2.0 * (1.0 - kr)), chroma)
    s = float(1 << (bits - 8))
    maxv = float((1 << bits) - 1)
    if full:
        yq, cbq, crq = y * maxv, 128 * s + cb * maxv, 128 * s + cr * maxv
    else:
        yq = (16 + 219 * y) * s
        cbq = (128 + 224 * cb) * s
        crq = (128 + 224 * cr) * s
    dt = np.uint8 if bits == 8 else np.uint16
    return tuple(
        np.clip(np.rint(p), 0, maxv).astype(dt) for p in (yq, cbq, crq)
    )


def yuv_frame(planes, w: int, h: int, pix_fmt: str):
    frame = av.VideoFrame(w, h, pix_fmt)
    for plane, arr in zip(frame.planes, planes):
        stride = plane.line_size // arr.itemsize
        pad = np.zeros((plane.height, stride), arr.dtype)
        pad[: arr.shape[0], : arr.shape[1]] = arr
        plane.update(pad)  # pyright: ignore
    return frame


def probe_color(path):
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        frame = next(container.decode(stream))
        return {
            "pix_fmt": frame.format.name,
            "colorspace": str(getattr(frame, "colorspace", None)),
            "color_range": str(getattr(frame, "color_range", None)),
        }


def generate_video(
    output,
    codec="h264",
    duration=10,
    resolution="1920x1080",
    fps=30,
    matrix="bt709",
    color_range="tv",
    pix_fmt="yuv420p",
    gop=None,
    bframes=None,
    noise=8.0,
    seed=0,
    progress=True,
):
    """градиент + шум, закодированный с матрицей matrix и range tv | pc"""
    codec = codec.lower()
    if codec not in CODEC_PRESETS:
        raise ValueError(
            f"codec '{codec}' not supported: {', '.join(CODEC_PRESETS)}"
        )
    if matrix not in MATRICES:
        raise ValueError(
            f"matrix '{matrix}' not supported: {', '.join(MATRICES)}"
        )
    if color_range not in ("tv", "pc"):
        raise ValueError("color_range must be 'tv' or 'pc'")
    if pix_fmt not in PIX_FMTS:
        raise ValueError(
            f"pix_fmt '{pix_fmt}' not supported: {', '.join(PIX_FMTS)}"
        )

    preset = CODEC_PRESETS[codec]
    output = Path(output)
    w, h = map(int, resolution.split("x"))
    if w % 2 or h % 2:
        raise ValueError(f"{resolution}: размеры должны быть чётными")

    full = color_range == "pc"
    bits = 10 if "10" in pix_fmt else 8
    chroma = _chroma_mode(pix_fmt)
    gop = gop or fps
    total_frames = int(duration * fps)
    m = MATRICES[matrix]

    # фиксированный seed
    rng = np.random.default_rng(seed)
    xs = np.linspace(0, 1, w, dtype=np.float32)
    ys = np.linspace(0, 1, h, dtype=np.float32)
    bars = make_bars(w, max(2, (h // 8) // 2 * 2))

    tmp = output.with_name(output.name + ".part")
    try:
        with av.open(str(tmp), mode="w", format=preset["format"]) as container:
            stream = container.add_stream(
                preset["vcodec"],
                rate=fps,
                options=_encoder_options(codec, matrix, full, gop, bframes),
            )
            stream.width = w
            stream.height = h
            stream.pix_fmt = pix_fmt
            stream.gop_size = gop
            _set_color_tags(stream.codec_context, matrix, full)
            with tqdm(
                total=total_frames,
                unit="frames",
                unit_scale=True,
                disable=not progress,
            ) as pb:
                for i in range(total_frames):
                    rgb = render_frame(i, xs, ys, rng, noise, bars)
                    planes = rgb_to_yuv(
                        rgb,
                        m["kr"],  # pyright: ignore
                        m["kb"],  # pyright: ignore
                        full,
                        bits,
                        chroma,
                    )
                    frame = yuv_frame(planes, w, h, pix_fmt)
                    frame.pts = i
                    frame.time_base = Fraction(1, fps)
                    for packet in stream.encode(frame):
                        container.mux(packet)
                    pb.update(1)
            for packet in stream.encode(None):
                container.mux(packet)
        os.replace(tmp, output)
    except Exception as exc:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"PyAV video generation failed: {output}") from exc

    size_mb = output.stat().st_size / 1_048_576
    logger.success(
        f"Video generated: {output} ({size_mb:.2f} mb) {probe_color(output)}"
    )


# FIXME: добавить также видео, где GOP между i-frame будет не 30 кадров (~1сек.)
# а 2/4 секунды, как с этим справится ридер?


# videoset:
TEST_VIDEOSET = []
BENCH_VIDEOSET = []

# FIXME: сделать явное разделение на тестовый/бенч видеосеты по аргументу
def gen_videoset():
    VIDEO_CACHE_DIR.mkdir(parents=True, exist_ok=True)

    videos = []

    def add(filename, **kwargs):
        path = VIDEO_CACHE_DIR / filename
        if not path.exists():
            generate_video(path, **kwargs)
        videos.append(path)

    # NOTE & BENCH: закоменченные видео размечены под бенчмарки,
    #               под текущие (0.4.2.dev2) тесты достаточно части C
    #               (на самом деле 1 видео из части C)

    # A) Разрешения и кодеки (8 видео)
    # add("h264_360p.mp4", resolution="640x360")
    # add("h264_720p.mp4", resolution="1280x720")
    # add("h264_1080p.mp4", resolution="1920x1080")
    add("h264_2160p.mp4", resolution="3840x2160")

    # add("h265_360p.mp4", codec="h265", resolution="640x360")
    # add("h265_720p.mp4", codec="h265", resolution="1280x720")
    # add("h265_1080p.mp4", codec="h265", resolution="1920x1080")
    add("h265_2160p.mp4", codec="h265", resolution="3840x2160")

    # B) GOP и B-frames (7 видео)
    # add("h264_gop1.mp4", gop=1)
    # add("h264_gop60.mp4", gop=60)
    # add("h264_gop120.mp4", gop=120)

    # add("h265_gop1.mp4", codec="h265", gop=1)
    # add("h265_gop60.mp4", codec="h265", gop=60)
    # add("h265_gop120.mp4", codec="h265", gop=120)

    # add("h264_bf0.mp4", bframes=0)

    # C) FPS и длительность (5 видео)
    add("25fps_10s.mp4", fps=25, duration=10, resolution="800x600")
    add("60fps_10s.mp4", fps=60, duration=10, resolution="800x600")
    add("30fps_10s.mp4", fps=30, duration=10, resolution="800x600")
    add("30fps_30s.mp4", fps=30, duration=30, resolution="800x600")
    add("30fps_5s.mp4", fps=30, duration=5, resolution="800x600")

    # D) Цветовые матрицы и range (6 видео)
    # add("bt601_tv.mp4", matrix="bt601")
    # add("bt470bg_tv.mp4", matrix="bt470bg")
    # add("bt2020_tv.mp4", matrix="bt2020")

    # add("bt709_pc.mp4", matrix="bt709", color_range="pc")
    # add("bt601_pc.mp4", matrix="bt601", color_range="pc")
    # add("bt2020_pc.mp4", matrix="bt2020", color_range="pc")

    # E) Форматы пикселей (4 видео)
    # add("h264_422.mp4", pix_fmt="yuv422p")
    # add("h264_444.mp4", pix_fmt="yuv444p")

    # add("h265_10bit.mp4", codec="h265", pix_fmt="yuv420p10le")
    # add(
    #     "h265_10bit_bt2020.mp4",
    #     codec="h265",
    #     pix_fmt="yuv420p10le",
    #     matrix="bt2020",
    # )

    # F) Уровень шума (2 видео)
    # add("noise0.mp4", noise=0)
    # add("noise24.mp4", noise=24)

    # G) VP9 (2 видео)
    # add("vp9_720p.webm", codec="vp9", resolution="1280x720")
    # add("vp9_1080p.webm", codec="vp9", resolution="1920x1080")

    logger.success(f"Videoset ready: {len(videos)} videos in {VIDEO_CACHE_DIR}")
    return videos


def test_videoset_exists():
    if not VIDEO_CACHE_DIR.exists():
        return False
    # TESTFIXME: не забывать расширять проверку по мере расширения тестов
    if not Path(VIDEO_CACHE_DIR / "30fps_10s.mp4").exists():
        return False
    if not Path(VIDEO_CACHE_DIR / "h265_2160p.mp4").exists():
        return False
    if not Path(VIDEO_CACHE_DIR / "h264_2160p.mp4").exists():
        return False
    return True
