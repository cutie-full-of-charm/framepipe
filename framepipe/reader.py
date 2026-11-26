#
# framepipe/reader.py
# SPDX-License-Identifier: MPL-2.0
#
from typing import Literal

import numpy as np

from framepipe import Frame

# import framepipe._framepipe as _framepipe
from ._framepipe import (
    HWAccel,
    MemoryType,
    OutputFormat,
    PostProcessor,
    Reader,
    State,
)


class ReaderStopped(RuntimeError):
    """Reader's impl is None."""


class WrappedReader:
    def __init__(
        self,
        source,
        name: str,
        verbose: bool = False,
        videostream_id: int = 0,
        reconnect_delay_sec: float = 1.0,
        degraded_timeout_sec: float = 3.0,
        av_container_options: dict | None = None,
        out_format: OutputFormat = OutputFormat.BGR24,
        hwaccel: HWAccel = HWAccel.SOFTWARE,
        postprocessor: PostProcessor = PostProcessor.CPU,
        output_memory: MemoryType = MemoryType.CPU,
        out_w: int = 0,
        out_h: int = 0,
        scale: float = 1.0,
        realtime_file: bool = False,
        auto_start: bool = True,
        on_demand: bool = False,
    ):
        self._verbose = verbose
        self._state_callback = self._on_state_change
        self._av_container_options = av_container_options or {}
        self._postprocessor = postprocessor
        self._output_memory = output_memory
        self._out_format = out_format

        self._impl = Reader(
            source,
            name,
            verbose=verbose,
            videostream_id=videostream_id,
            reconnect_delay_sec=reconnect_delay_sec,
            degraded_timeout_sec=degraded_timeout_sec,
            av_options=self._av_container_options,
            out_format=out_format,
            hwaccel=hwaccel,
            postprocessor=postprocessor,
            output_memory=output_memory,
            out_w=out_w,
            out_h=out_h,
            scale=scale,
            realtime_file=realtime_file,
            on_demand=on_demand,
        )

        self._impl.set_state_callback(self._state_callback)
        if auto_start:
            self._impl.start_thread()

    @property
    def _reader(self):
        impl = self._impl
        if impl is None:
            raise ReaderStopped("reader has been stopped")
        return impl

    # FIXME: брать настоящий output_memory из _impl?
    def _require_cpu_output(self) -> None:
        if self._output_memory != MemoryType.CPU:
            raise RuntimeError("NumPy requires CPU output memory")

    # FIXME: брать настоящий pxl_format из _impl?
    def _require_not_planar(self) -> None:
        if self._out_format not in (
            OutputFormat.BGR24,
            OutputFormat.RGB24,
            OutputFormat.GRAY8,
        ):
            raise RuntimeError("Method requires non-planar pixel format")

    def read(self) -> np.ndarray | None:
        self._require_cpu_output()
        self._require_not_planar()
        return self._reader.read()  # pyright: ignore

    def read_new(self) -> np.ndarray | None:
        self._require_cpu_output()
        self._require_not_planar()
        return self._reader.read_new()  # pyright: ignore

    def read_latest(self, timeout) -> np.ndarray | None:
        self._require_cpu_output()
        self._require_not_planar()
        return self._reader.read_latest(timeout=timeout)  # pyright: ignore

    def read_frame(self) -> Frame:
        return self._reader.read_frame()

    def read_new_frame(self) -> Frame | None:
        return self._reader.read_new_frame()

    def read_latest_frame(self, timeout: float = 1.0) -> Frame | None:
        return self._reader.read_latest_frame(timeout=timeout)

    def next(self) -> np.ndarray | None:
        return self._reader.next()

    def next_frame(self) -> Frame | None:
        return self._reader.next_frame()

    @staticmethod
    def _to_torch(frame):
        """Convert CPU/CUDA frames via DLPack; never reinterpret cl_mem as CUDA."""
        if frame is None:
            return None
        device_type, _device_id = frame.__dlpack_device__()
        if device_type == 4:  # DLPack kDLOpenCL
            raise RuntimeError(
                "OpenCL DLPack export is available, but standard PyTorch does "
                "not provide an OpenCL tensor backend. Use read_new_frame() "
                "with an OpenCL-capable consumer, or request CPU output. "
                "The separate integration test can probe torch.from_dlpack()."
            )
        import torch

        return torch.from_dlpack(frame)

    def read_tensor(self):
        return self._to_torch(self._reader.read_frame())

    def read_new_tensor(self):
        return self._to_torch(self._reader.read_new_frame())

    def read_dlpack(self) -> Frame:
        """Return the newest published Frame as a DLPack producer (CPU/CUDA/OpenCL)."""
        return self._reader.read_frame()

    def read_new_dlpack(self) -> Frame | None:
        """Return a new Frame as a DLPack producer; consumer owns its capsule."""
        return self._reader.read_new_frame()

    def grab(self) -> None:
        self._reader.grab()

    def retrieve(self) -> tuple[Literal[False], None] | tuple[Literal[True], np.ndarray]:
        return self._reader.retrieve()

    def release(self) -> None:
        self.stop()

    def is_opened(self) -> bool:
        return self._impl is not None and self._impl.is_opened()

    def is_alive(self) -> bool:
        return self._impl is not None and self._impl.is_alive()

    # FIXME: guard
    def start(self):
        self._reader.start_thread()

    def stop(self) -> None:
        impl = self._impl
        self._impl = None
        if impl is None:
            return
        impl.stop()
        self._state_callback = None

    # override callback with your logging here
    def _on_state_change(self, name: str, old: str, new: str):
        # print(f"[{name}]: {old} -> {new}")
        pass

    def get_last_exception(self) -> RuntimeError | None:
        return self._reader.get_last_exception()

    @property
    def warmuped(self) -> bool:
        return self._reader.frames_processed > 0

    @property
    def last_pts_seconds(self) -> float:
        return self._impl.last_pts_seconds if self._impl else 0.0

    @property
    def last_recv_time(self) -> float:
        return self._impl.last_recv_time if self._impl else 0.0

    @property
    def name(self) -> str:
        return self._impl.name if self._impl else "name: Null"

    @property
    def state(self):
        return self._impl.state if self._impl else State.STOPPED

    @property
    def frame_pix_fmt(self) -> str:
        return self._impl.frame_pix_fmt if self._impl else "none"

    @property
    def hwaccel_active(self) -> str:
        return self._impl.hwaccel_active if self._impl else "SOFTWARE"

    @property
    def _is_live(self):
        return self._impl._is_live if self._impl else False  # pyright: ignore

    @property
    def packets_processed(self) -> int:
        return self._impl.packets_processed if self._impl else -1

    @property
    def frames_processed(self) -> int:
        return self._impl.frames_processed if self._impl else -1

    def __enter__(self) -> WrappedReader:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()
