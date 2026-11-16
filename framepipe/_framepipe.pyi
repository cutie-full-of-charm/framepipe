from collections.abc import Mapping
import enum
from typing import Annotated, Any

import numpy
from numpy.typing import NDArray


from typing import Literal

class State(enum.Enum):
    INIT = 0

    CONNECTING = 1

    STREAMING = 2

    DEGRADED = 3

    STOPPED = 4

class OutputFormat(enum.Enum):
    BGR24 = 0

    RGB24 = 1

    GRAY8 = 2

    NATIVE = 3

class HWAccel(enum.Enum):
    SOFTWARE = 0

    CUDA = 1

    VAAPI = 2

    D3D11VA = 3

class PostProcessor(enum.Enum):
    CPU = 0

    CUDA = 1

    VAAPI = 2

    OPENCL = 3

class MemoryType(enum.Enum):
    CPU = 0

    GPU = 1

class Frame:
    @property
    def width(self) -> int: ...

    @property
    def height(self) -> int: ...

    @property
    def src_width(self) -> int: ...

    @property
    def src_height(self) -> int: ...

    @property
    def channels(self) -> int: ...

    @property
    def pts(self) -> int: ...

    @property
    def ts(self) -> float: ...

    @property
    def id(self) -> int: ...

    @property
    def key(self) -> bool: ...

    @property
    def memory_type(self) -> MemoryType: ...

    @property
    def pix_fmt(self) -> str: ...

    def __repr__(self) -> str: ...

class CpuPackedFrame(Frame):
    def array(self) -> NDArray[numpy.uint8]: ...

    def numpy(self) -> NDArray[numpy.uint8]: ...

    def dlpack(self) -> Annotated[Any, dict(dtype='uint8')]: ...

    def __dlpack_device__(self) -> tuple: ...

    def __dlpack__(self, stream: object | None = None) -> object: ...

class CpuPlanarFrame(Frame):
    def planes(self) -> tuple: ...

class CudaFrame(Frame):
    @property
    def cuda_ptr(self) -> int: ...

    @property
    def cuda_context_handle(self) -> int: ...

    def dlpack(self) -> Annotated[Any, dict(dtype='uint8')]: ...

    def __dlpack_device__(self) -> tuple: ...

    def __dlpack__(self, stream: object | None = None) -> object: ...

class OpenCLBufferFrame(Frame):
    @property
    def opencl_buffer_handle(self) -> int: ...

    @property
    def opencl_context_handle(self) -> int: ...

    def dlpack(self) -> Annotated[Any, dict(dtype='uint8')]: ...

    def __dlpack_device__(self) -> tuple: ...

    def __dlpack__(self, stream: object | None = None) -> object: ...

class OpenCLImageFrame(Frame):
    @property
    def opencl_context_handle(self) -> int: ...

    @property
    def opencl_plane_handles(self) -> tuple: ...

class VaapiFrame(Frame):
    @property
    def surface_id(self) -> int: ...

class Reader:
    def __init__(self, source: str, name: str, verbose: bool = False, videostream_id: int = 0, reconnect_delay_sec: float = 1.0, degraded_timeout_sec: float = 3.0, av_options: Mapping[str, str] = {}, out_format: OutputFormat = OutputFormat.NATIVE, hwaccel: HWAccel = HWAccel.SOFTWARE, realtime_file: bool = False, out_w: int = 0, out_h: int = 0, scale: float = 1.0, postprocessor: PostProcessor = PostProcessor.CPU, output_memory: MemoryType = MemoryType.CPU, on_demand: bool = False) -> None: ...

    def read(self) -> numpy.ndarray | None: ...

    def read_new(self) -> numpy.ndarray | None: ...

    def read_latest(self, timeout: float = 1.0) -> numpy.ndarray | None: ...

    def next(self) -> numpy.ndarray | None: ...

    def read_frame(self) -> Frame: ...

    def read_new_frame(self) -> Frame | None: ...

    def read_latest_frame(self, timeout: float = 1.0) -> Frame | None: ...

    def next_frame(self) -> Frame | None: ...

    def grab(self) -> None: ...

    def retrieve(self) -> tuple[Literal[False], None] | tuple[Literal[True], numpy.ndarray]: ...

    def start_thread(self) -> None: ...

    def is_opened(self) -> bool: ...

    def is_alive(self) -> bool: ...

    def stop(self) -> None: ...

    def set_state_callback(self, cb: object | None) -> None: ...

    @property
    def name(self) -> str: ...

    @property
    def source(self) -> str: ...

    @property
    def out_format(self) -> OutputFormat: ...

    @property
    def hwaccel_requested(self) -> HWAccel: ...

    @property
    def hwaccel_active(self) -> str: ...

    @property
    def frame_pix_fmt(self) -> str: ...

    @property
    def state(self) -> State: ...

    @property
    def last_pts_seconds(self) -> float: ...

    @property
    def last_recv_time(self) -> float: ...

    @property
    def frames_processed(self) -> int: ...

    @property
    def packets_processed(self) -> int: ...

    def __str__(self) -> str: ...
