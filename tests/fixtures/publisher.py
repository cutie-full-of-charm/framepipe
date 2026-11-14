"""RTSP test publisher implemented with PyAV, without an ffmpeg executable."""

import multiprocessing
import random
import time
from fractions import Fraction

import av


def _wait_until(deadline):
    remaining = deadline - time.monotonic()
    if remaining > 0:
        time.sleep(remaining)


def _publish_transcoded(src, url, jitter_ms, real_rate, deliver_fps):
    # Encodes every loop anew; timing starts at zero for the source, but
    # outgoing timestamps stay monotonic across loop boundaries.
    with av.open(url, 'w', format='rtsp', options={'rtsp_transport': 'tcp'}) as dest:
        with av.open(str(src)) as sample:
            input_rate = sample.streams.video[0].average_rate
        fps = Fraction(deliver_fps) if deliver_fps else (input_rate or Fraction(30))
        stream = dest.add_stream('libx264', rate=fps, options={
            'preset': 'ultrafast', 'tune': 'zerolatency',
        })
        stream.pix_fmt = 'yuv420p'
        stream.gop_size = 30
        # Set dimensions before the RTSP output is initialized.
        with av.open(str(src)) as sample:
            input_stream = sample.streams.video[0]
            stream.width = input_stream.codec_context.width
            stream.height = input_stream.codec_context.height

        frame_index = 0
        start = time.monotonic()
        # Delivering fewer frames is equivalent to FFmpeg's fps filter;
        # real_rate instead slows the source clock without changing PTS.
        output_time_base = Fraction(1, 90000)
        stream.time_base = output_time_base
        stream.codec_context.time_base = output_time_base
        last_sample_index = -1
        source_loop = 0
        while True:
            with av.open(str(src)) as source:
                video = source.streams.video[0]
                source_rate = video.average_rate or Fraction(30)
                source_step = Fraction(source_rate.denominator, source_rate.numerator)
                local_index = -1
                for local_index, frame in enumerate(source.decode(video)):
                    source_time = source_loop + Fraction(local_index) * source_step
                    # For lower output FPS, drop frames until the next output tick.
                    if deliver_fps:
                        sample_index = int(source_time * fps)
                        if sample_index <= last_sample_index:
                            continue
                        last_sample_index = sample_index
                    nominal_seconds = Fraction(frame_index, 1) / fps
                    speed = real_rate if real_rate > 0 else 1.0
                    _wait_until(start + float(nominal_seconds) / speed)
                    frame.pts = round(nominal_seconds / output_time_base)
                    frame.time_base = output_time_base
                    if jitter_ms:
                        # Keep the jitter relative to PTS in milliseconds.
                        frame.pts += round(random.random() * jitter_ms / 1000 / float(output_time_base))
                    for packet in stream.encode(frame):
                        dest.mux(packet)
                    frame_index += 1
                source_loop += Fraction(local_index + 1) * source_step


def _publish_copy(src, url):
    """Loop the input while copying video packets and pacing by DTS."""
    loop_offset = Fraction(0)
    start = time.monotonic()
    with av.open(str(src)) as sample:
        video = sample.streams.video[0]
        with av.open(url, mode='w', format='rtsp', options={'rtsp_transport': 'tcp'}) as dest:
            out_stream = dest.add_stream_from_template(video)
            while True:
                last_time = Fraction(0)
                first_this_loop = None
                with av.open(str(src)) as source:
                    input_video = source.streams.video[0]
                    for packet in source.demux(input_video):
                        if packet.dts is None:
                            continue
                        tb = packet.time_base or input_video.time_base
                        if first_this_loop is None:
                            first_this_loop = packet.dts * tb
                        relative_dts = packet.dts * tb - first_this_loop
                        # Shift both DTS and PTS by the same amount.
                        shift = round((loop_offset - first_this_loop) / tb)
                        _wait_until(start + float(loop_offset + relative_dts))
                        packet.dts += shift
                        if packet.pts is not None:
                            packet.pts += shift
                        packet.stream = out_stream
                        dest.mux(packet)
                        last_time = max(last_time, relative_dts)
                    frame_rate = input_video.average_rate or Fraction(30)
                    step = Fraction(frame_rate.denominator, frame_rate.numerator)
                    loop_offset += last_time + step


def _publish_worker(src, url, jitter_ms, real_rate, deliver_fps):
    if any((jitter_ms, real_rate, deliver_fps)):
        _publish_transcoded(src, url, jitter_ms, real_rate, deliver_fps)
    else:
        _publish_copy(src, url)


class Publisher:
    """Minimal Popen-compatible interface used in the reader tests."""

    def __init__(self, src, url, jitter_ms=0.0, real_rate=0.0, deliver_fps=0):
        self._process = multiprocessing.Process(
            target=_publish_worker,
            args=(str(src), url, jitter_ms, real_rate, deliver_fps),
            daemon=True,
        )
        self._process.start()

    @property
    def pid(self):
        return self._process.pid

    def poll(self):
        return None if self._process.is_alive() else self._process.exitcode

    def terminate(self):
        if self._process.is_alive():
            self._process.terminate()

    def kill(self):
        if self._process.is_alive():
            self._process.kill()

    def wait(self, timeout=None):
        self._process.join(timeout)
        if self._process.is_alive():
            raise TimeoutError('RTSP publisher did not exit in time')
        return self._process.exitcode
