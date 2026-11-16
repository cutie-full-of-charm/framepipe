// src/bindings.cpp
#include "_base.hpp"
#include "frame.hpp"
#include "reader.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unordered_map.h>
#include <nanobind/stl/vector.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <sys/types.h>
}

#if defined(_WIN32) || defined(_WIN64)
using uint = std::uint32_t;
#endif

namespace nb = nanobind;
using namespace nb::literals;

using FrameArray = nb::ndarray<nb::numpy, uint8_t>;
using FrameDLPack = nb::ndarray<nb::array_api, uint8_t>;

// The storage type, not a combination of nullable pointers, determines the API.
static FrameArray frame_to_numpy(std::shared_ptr<Frame> f) {
    if (!f)
        throw std::runtime_error("Null frame");
    auto *cpu = std::get_if<CpuPackedStorage>(&f->storage);
    if (!cpu || !cpu->owner || !cpu->data || cpu->row_stride <= 0)
        throw std::runtime_error("CpuPackedFrame required for array()");

    auto *ptr = new std::shared_ptr<Frame>(f);
    nb::capsule owner(ptr, [](void *p) noexcept { delete static_cast<std::shared_ptr<Frame> *>(p); });
    const int64_t strides[3] = {static_cast<int64_t>(cpu->row_stride), static_cast<int64_t>(f->channels), 1};
    return FrameArray(cpu->data, f->ndim, f->dims, owner, strides);
}

static int frame_dlpack_device(const Frame &frame) {
    if (std::holds_alternative<CpuPackedStorage>(frame.storage))
        return nb::device::cpu::value;
    if (std::holds_alternative<OpenCLBufferStorage>(frame.storage))
        return 4; // DLPack kDLOpenCL: data is cl_mem, not a mapped pointer
    if (std::holds_alternative<CudaStorage>(frame.storage))
        return nb::device::cuda::value;
    throw std::runtime_error("Frame storage is not a packed DLPack tensor");
}

static FrameDLPack frame_to_dlpack(std::shared_ptr<Frame> f) {
    if (!f || f->ndim == 0)
        throw std::runtime_error("Packed tensor required for DLPack export");
    void *ptr = nullptr;
    if (auto *cpu = std::get_if<CpuPackedStorage>(&f->storage)) {
        if (!cpu->owner)
            throw std::runtime_error("CPU frame has no owner");
        ptr = cpu->data;
    } else if (auto *cuda = std::get_if<CudaStorage>(&f->storage)) {
        if (!cuda->owner)
            throw std::runtime_error("CUDA frame has no owner");
        ptr = cuda->data;
    } else if (auto *cl = std::get_if<OpenCLBufferStorage>(&f->storage)) {
        if (!cl->owner)
            throw std::runtime_error("OpenCL frame has no owner");
        ptr = cl->buffer;
    }
    if (!ptr)
        throw std::runtime_error("Frame has no packed buffer");

    const int device_type = frame_dlpack_device(*f);
    auto *holder = new std::shared_ptr<Frame>(std::move(f));
    nb::capsule owner(holder, [](void *p) noexcept { delete static_cast<std::shared_ptr<Frame> *>(p); });
    const Frame &frame = **holder;
    // CPU AVFrames can have padded rows: DLPack strides are in elements
    // (uint8_t => bytes), so never advertise a padded frame as contiguous.
    int64_t cpu_strides[3] = {};
    const int64_t *strides = nullptr;
    if (const auto *cpu = std::get_if<CpuPackedStorage>(&frame.storage)) {
        cpu_strides[0] = cpu->row_stride;
        cpu_strides[1] = frame.channels == 1 ? 1 : frame.channels;
        cpu_strides[2] = 1;
        strides = cpu_strides;
    }
    return FrameDLPack(ptr, static_cast<size_t>(frame.ndim), frame.dims, owner, strides, nb::dtype<uint8_t>(),
                       device_type, 0);
}

static nb::tuple frame_planes(std::shared_ptr<Frame> f) {
    if (!f)
        throw std::runtime_error("Null frame");
    auto *cpu = std::get_if<CpuPlanarStorage>(&f->storage);
    if (!cpu || !cpu->owner)
        throw std::runtime_error("CpuPlanarFrame required for planes()");
    if (f->pix_fmt != AV_PIX_FMT_YUV420P && f->pix_fmt != AV_PIX_FMT_YUVJ420P && f->pix_fmt != AV_PIX_FMT_NV12)
        throw std::runtime_error("Unsupported pixel format for planes()");

    auto *holder = new std::shared_ptr<Frame>(f);
    nb::capsule owner(holder, [](void *p) noexcept { delete static_cast<std::shared_ptr<Frame> *>(p); });
    const size_t w = static_cast<size_t>(f->width);
    const size_t h = static_cast<size_t>(f->height);
    const size_t cw = (w + 1) / 2;
    const size_t ch = (h + 1) / 2;

    auto plane = [&](int index, size_t height, size_t width) {
        if (!cpu->planes[index] || cpu->linesize[index] <= 0)
            throw std::runtime_error("Invalid CPU plane");
        size_t shape[2] = {height, width};
        int64_t strides[2] = {static_cast<int64_t>(cpu->linesize[index]), 1};
        return FrameArray(cpu->planes[index], 2, shape, owner, strides);
    };

    if (f->pix_fmt == AV_PIX_FMT_NV12)
        return nb::make_tuple(plane(0, h, w), plane(1, ch, cw * 2));
    return nb::make_tuple(plane(0, h, w), plane(1, ch, cw), plane(2, ch, cw));
}

static nb::object cast_frame(const std::shared_ptr<Frame> &f) {
    if (!f)
        return nb::none();
    // Reader constructs the subtype from the same storage index. Use an
    // explicit static cast to avoid requiring RTTI across CUDA shared modules.
    switch (f->storage.index()) {
    case 0:
        return nb::cast(std::static_pointer_cast<CpuPackedFrame>(f));
    case 1:
        return nb::cast(std::static_pointer_cast<CpuPlanarFrame>(f));
    case 2:
        return nb::cast(std::static_pointer_cast<CudaFrame>(f));
    case 3:
        return nb::cast(std::static_pointer_cast<OpenCLBufferFrame>(f));
    case 4:
        return nb::cast(std::static_pointer_cast<OpenCLImageFrame>(f));
    case 5:
        return nb::cast(std::static_pointer_cast<VaapiFrame>(f));
    default:
        throw std::runtime_error("Unknown frame storage alternative");
    }
}

static nb::object frame_dlpack_capsule(std::shared_ptr<Frame> f, nb::object stream) {
    if (!stream.is_none() && std::holds_alternative<OpenCLBufferStorage>(f->storage))
        throw std::runtime_error("OpenCL DLPack stream interop is unsupported; use stream=None");
    nb::object array = nb::cast(frame_to_dlpack(std::move(f)));
    return array.attr("__dlpack__")();
}

NB_MODULE(_framepipe, m) {

    nb::enum_<State>(m, "State")
        .value("INIT", State::INIT)
        .value("CONNECTING", State::CONNECTING)
        .value("STREAMING", State::STREAMING)
        .value("DEGRADED", State::DEGRADED)
        .value("STOPPED", State::STOPPED);

    nb::enum_<OutputFormat>(m, "OutputFormat")
        .value("BGR24", OutputFormat::BGR24)
        .value("RGB24", OutputFormat::RGB24)
        .value("GRAY8", OutputFormat::GRAY8)
        .value("NATIVE", OutputFormat::NATIVE);

    nb::enum_<HWAccel>(m, "HWAccel")
        .value("SOFTWARE", HWAccel::SOFTWARE)
        .value("CUDA", HWAccel::CUDA)
        .value("VAAPI", HWAccel::VAAPI)
        .value("D3D11VA", HWAccel::D3D11VA);

    nb::enum_<PostProcessor>(m, "PostProcessor")
        .value("CPU", PostProcessor::CPU)
        .value("CUDA", PostProcessor::CUDA)
        .value("VAAPI", PostProcessor::VAAPI)
        .value("OPENCL", PostProcessor::OPENCL);
    // .value("VULKAN", PostProcessor::VULKAN)
    // .value("D3D11", PostProcessor::D3D11);

    nb::enum_<MemoryType>(m, "MemoryType").value("CPU", MemoryType::CPU).value("GPU", MemoryType::GPU);

    nb::class_<Frame>(m, "Frame")
        .def_ro("width", &Frame::width)
        .def_ro("height", &Frame::height)
        .def_ro("src_width", &Frame::src_width)
        .def_ro("src_height", &Frame::src_height)
        .def_ro("channels", &Frame::channels)
        .def_ro("pts", &Frame::pts)
        .def_ro("ts", &Frame::ts)
        .def_ro("id", &Frame::id)
        .def_ro("key", &Frame::key)
        .def_prop_ro("memory_type", &Frame::memory_type)
        .def_prop_ro("pix_fmt",
                     [](const Frame &f) {
                         const char *name = av_get_pix_fmt_name(f.pix_fmt);
                         return std::string(name ? name : "none");
                     })
        .def("__repr__", [](const Frame &f) {
            return "Frame(id=" + std::to_string(f.id) + ", " + std::to_string(f.width) + "x" +
                   std::to_string(f.height) + ", pts=" + std::to_string(f.pts) + ")";
        });

    nb::class_<CpuPackedFrame, Frame>(m, "CpuPackedFrame")
        .def("array", [](std::shared_ptr<CpuPackedFrame> f) { return frame_to_numpy(f); })
        .def("numpy", [](std::shared_ptr<CpuPackedFrame> f) { return frame_to_numpy(f); })
        .def("dlpack", [](std::shared_ptr<CpuPackedFrame> f) { return frame_to_dlpack(f); })
        .def("__dlpack_device__", [](const CpuPackedFrame &f) { return nb::make_tuple(frame_dlpack_device(f), 0); })
        .def(
            "__dlpack__",
            [](std::shared_ptr<CpuPackedFrame> f, nb::object stream) { return frame_dlpack_capsule(f, stream); },
            "stream"_a = nb::none());

    nb::class_<CpuPlanarFrame, Frame>(m, "CpuPlanarFrame").def("planes", [](std::shared_ptr<CpuPlanarFrame> f) {
        return frame_planes(f);
    });

    nb::class_<CudaFrame, Frame>(m, "CudaFrame")
        .def_prop_ro(
            "cuda_ptr",
            [](const CudaFrame &f) { return reinterpret_cast<uintptr_t>(std::get<CudaStorage>(f.storage).data); })
        .def_prop_ro(
            "cuda_context_handle",
            [](const CudaFrame &f) { return reinterpret_cast<uintptr_t>(std::get<CudaStorage>(f.storage).context); })
        .def("dlpack", [](std::shared_ptr<CudaFrame> f) { return frame_to_dlpack(f); })
        .def("__dlpack_device__", [](const CudaFrame &f) { return nb::make_tuple(frame_dlpack_device(f), 0); })
        .def(
            "__dlpack__",
            [](std::shared_ptr<CudaFrame> f, nb::object stream) { return frame_dlpack_capsule(f, stream); },
            "stream"_a = nb::none());

    nb::class_<OpenCLBufferFrame, Frame>(m, "OpenCLBufferFrame")
        .def_prop_ro("opencl_buffer_handle",
                     [](const OpenCLBufferFrame &f) {
                         return reinterpret_cast<uintptr_t>(std::get<OpenCLBufferStorage>(f.storage).buffer);
                     })
        .def_prop_ro("opencl_context_handle",
                     [](const OpenCLBufferFrame &f) {
                         return reinterpret_cast<uintptr_t>(std::get<OpenCLBufferStorage>(f.storage).context);
                     })
        .def("dlpack", [](std::shared_ptr<OpenCLBufferFrame> f) { return frame_to_dlpack(f); })
        .def("__dlpack_device__", [](const OpenCLBufferFrame &f) { return nb::make_tuple(frame_dlpack_device(f), 0); })
        .def(
            "__dlpack__",
            [](std::shared_ptr<OpenCLBufferFrame> f, nb::object stream) { return frame_dlpack_capsule(f, stream); },
            "stream"_a = nb::none());

    nb::class_<OpenCLImageFrame, Frame>(m, "OpenCLImageFrame")
        .def_prop_ro("opencl_context_handle",
                     [](const OpenCLImageFrame &f) {
                         return reinterpret_cast<uintptr_t>(std::get<OpenCLImageStorage>(f.storage).context);
                     })
        .def_prop_ro("opencl_plane_handles", [](const OpenCLImageFrame &f) {
            const auto &p = std::get<OpenCLImageStorage>(f.storage).planes;
            return nb::make_tuple(reinterpret_cast<uintptr_t>(p[0]), reinterpret_cast<uintptr_t>(p[1]));
        });

    nb::class_<VaapiFrame, Frame>(m, "VaapiFrame").def_prop_ro("surface_id", [](const VaapiFrame &f) {
        const auto &owner = std::get<VaapiStorage>(f.storage).frame;
        return reinterpret_cast<uintptr_t>(owner->data[3]);
    });

    nb::class_<Reader>(m, "Reader")
        .def(nb::init<std::string, std::string, bool, int, double, double, std::unordered_map<std::string, std::string>,
                      OutputFormat, HWAccel, bool, uint, uint, double, PostProcessor, MemoryType, bool>(),

             nb::arg("source"), nb::arg("name"), nb::arg("verbose") = false, nb::arg("videostream_id") = 0,
             nb::arg("reconnect_delay_sec") = 1.0, nb::arg("degraded_timeout_sec") = 3.0,
             nb::arg("av_options") = std::unordered_map<std::string, std::string>{},
             nb::arg("out_format") = OutputFormat::NATIVE, nb::arg("hwaccel") = HWAccel::SOFTWARE,
             nb::arg("realtime_file") = false, nb::arg("out_w") = 0, nb::arg("out_h") = 0, nb::arg("scale") = 1.0,
             nb::arg("postprocessor") = PostProcessor::CPU, nb::arg("output_memory") = MemoryType::CPU,
             nb::arg("on_demand") = false

             )
        // ndarray reading below
        .def(
            "read",
            [](Reader &r) -> nb::object {
                nb::gil_scoped_release release;
                auto frame = r.read();
                nb::gil_scoped_acquire acquire;
                if (!frame)
                    return nb::none();
                return nb::cast(frame_to_numpy(frame));
            },
            nb::sig("def read(self) -> numpy.ndarray | None"))
        .def(
            "read_new",
            [](Reader &r) -> nb::object {
                nb::gil_scoped_release release;
                auto frame = r.read_new();
                nb::gil_scoped_acquire acquire;
                if (!frame)
                    return nb::none();
                return nb::cast(frame_to_numpy(frame));
            },
            nb::sig("def read_new(self) -> numpy.ndarray | None"))
        .def(
            "read_latest",
            [](Reader &r, double timeout) -> nb::object {
                std::optional<std::shared_ptr<Frame>> result;
                {
                    nb::gil_scoped_release release;
                    result = r.read_latest(timeout);
                }
                if (!result)
                    return nb::none();
                return nb::cast(frame_to_numpy(*result));
            },
            "timeout"_a = 1.0, nb::sig("def read_latest(self, timeout: float = 1.0) -> numpy.ndarray | None"))
        .def(
            "next",
            [](Reader &r) -> nb::object {
                std::shared_ptr<Frame> frame;
                {
                    nb::gil_scoped_release release;
                    frame = r.next();
                }
                if (!frame)
                    return nb::none();
                return nb::cast(frame_to_numpy(frame));
            },
            nb::sig("def next(self) -> numpy.ndarray | None"))
        // Frame reading below
        .def(
            "read_frame",
            [](Reader &r) -> nb::object {
                std::shared_ptr<Frame> frame;
                {
                    nb::gil_scoped_release release;
                    frame = r.read();
                }
                return cast_frame(frame);
            },
            nb::sig("def read_frame(self) -> Frame"))
        .def(
            "read_new_frame",
            [](Reader &r) -> nb::object {
                std::shared_ptr<Frame> frame;
                {
                    nb::gil_scoped_release release;
                    frame = r.read_new();
                }
                return cast_frame(frame);
            },
            nb::sig("def read_new_frame(self) -> Frame | None"))

        .def(
            "read_latest_frame",
            [](Reader &r, double timeout) -> nb::object {
                std::optional<std::shared_ptr<Frame>> result;
                {
                    nb::gil_scoped_release release;
                    result = r.read_latest(timeout);
                }
                if (!result) {
                    return nb::none();
                }
                return cast_frame(*result);
            },
            "timeout"_a = 1.0, nb::sig("def read_latest_frame(self, timeout: float = 1.0) -> Frame | None"))
        .def(
            "next_frame",
            [](Reader &r) -> nb::object {
                std::shared_ptr<Frame> frame;
                {
                    nb::gil_scoped_release release;
                    frame = r.next();
                }
                return cast_frame(frame);
            },
            nb::sig("def next_frame(self) -> Frame | None"))
        // other things below
        .def("grab", &Reader::grab)
        .def(
            "retrieve",
            [](Reader &r) -> std::pair<bool, nb::object> {
                auto [ok, frame] = r.retrieve();
                if (!ok)
                    return {false, nb::none()};
                return {true, nb::cast(frame_to_numpy(frame))};
            },
            nb::sig("def retrieve(self) -> tuple[Literal[False], None] | tuple[Literal[True], numpy.ndarray]"))
        .def("start_thread", &Reader::start_thread)
        .def("is_opened", &Reader::is_opened)
        .def("is_alive", &Reader::is_alive)
        .def("stop",
             [](Reader &r) {
                 nb::gil_scoped_release release;
                 r.stop();
             })
        .def(
            "set_state_callback",
            [](Reader &r, nb::object cb) {
                if (cb.is_none()) {
                    r._on_state_change = nullptr;
                    return;
                }
                r._on_state_change = [cb](const std::string &name, const std::string &old_s, const std::string &new_s) {
                    nb::gil_scoped_acquire acquire;
                    cb(name, old_s, new_s);
                };
            },
            nb::arg("cb").none())

        .def_ro("name", &Reader::name)
        .def_ro("source", &Reader::source)
        .def_ro("_is_live", &Reader::_is_live)
        .def_ro("out_format", &Reader::out_format)
        .def_ro("hwaccel_requested", &Reader::hwaccel_requested)
        .def_ro("hwaccel_active", &Reader::hwaccel_active)
        .def_prop_ro("frame_pix_fmt", &Reader::frame_pix_fmt, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("state", &Reader::state, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("last_pts_seconds", &Reader::last_pts_seconds, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("last_recv_time", &Reader::last_recv_time, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("frames_processed", &Reader::frames_processed, nb::call_guard<nb::gil_scoped_release>())
        .def_prop_ro("packets_processed", &Reader::packets_processed, nb::call_guard<nb::gil_scoped_release>())
        .def("__str__", [](const Reader &r) {
            return "Reader(source:" + r.source + ", name:" + r.name + ", state:" + state_to_str(r.state()) + ")";
        });
}
