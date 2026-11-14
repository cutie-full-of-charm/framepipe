// src/frame.hpp
#pragma once

#include "_base.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

struct CUctx_st;
using CUcontext = CUctx_st *;

// A storage alternative contains only fields meaningful for that memory layout.
// Owners retain both the memory and any device context needed for destruction.
struct CpuPackedStorage {
    std::shared_ptr<void> owner;
    uint8_t *data = nullptr;
    int row_stride = 0;
};

struct CpuPlanarStorage {
    std::shared_ptr<void> owner;
    std::array<uint8_t *, 4> planes{};
    std::array<int, 4> linesize{};
};

struct CudaStorage {
    std::shared_ptr<void> owner;
    uint8_t *data = nullptr;
    CUcontext context = nullptr;
};

struct OpenCLBufferStorage {
    std::shared_ptr<void> owner;
    void *buffer = nullptr;  // cl_mem (opaque across the CUDA build)
    void *context = nullptr; // cl_context
};

struct OpenCLImageStorage {
    std::shared_ptr<void> owner;
    std::array<void *, 2> planes{}; // cl_image2d Y and UV
    void *context = nullptr;
};

struct VaapiStorage {
    std::shared_ptr<AVFrame> frame; // keeps the VAAPI surface and device alive
};

using FrameStorage = std::variant<CpuPackedStorage, CpuPlanarStorage, CudaStorage, OpenCLBufferStorage,
                                  OpenCLImageStorage, VaapiStorage>;

// Processor result is a typed payload, not a second Frame with duplicated pointers.
struct FramePayload {
    int width = 0;
    int height = 0;
    int channels = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    FrameStorage storage;
};

struct Frame {
    virtual ~Frame() = default; // for nanobind's polymorphic shared_ptr dispatch

    FrameStorage storage;
    int width = 0;
    int height = 0;
    unsigned src_width = 0;
    unsigned src_height = 0;
    unsigned channels = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    int ndim = 3;
    size_t dims[3] = {0, 0, 0};
    int64_t pts = 0;
    double ts = 0.0;
    uint64_t id = 0;
    bool key = false;

    MemoryType memory_type() const noexcept {
        return std::holds_alternative<CpuPackedStorage>(storage) || std::holds_alternative<CpuPlanarStorage>(storage)
                   ? MemoryType::CPU
                   : MemoryType::GPU;
    }
};

// Concrete types are part of the public Python API; Reader still deals with
// shared_ptr<Frame> and creates the correct subtype from the storage variant.
struct CpuPackedFrame final : Frame {};
struct CpuPlanarFrame final : Frame {};
struct CudaFrame final : Frame {};
struct OpenCLBufferFrame final : Frame {};
struct OpenCLImageFrame final : Frame {};
struct VaapiFrame final : Frame {};

inline std::shared_ptr<Frame> make_typed_frame(FramePayload payload) {
    std::shared_ptr<Frame> frame;
    switch (payload.storage.index()) {
    case 0:
        frame = std::make_shared<CpuPackedFrame>();
        break;
    case 1:
        frame = std::make_shared<CpuPlanarFrame>();
        break;
    case 2:
        frame = std::make_shared<CudaFrame>();
        break;
    case 3:
        frame = std::make_shared<OpenCLBufferFrame>();
        break;
    case 4:
        frame = std::make_shared<OpenCLImageFrame>();
        break;
    case 5:
        frame = std::make_shared<VaapiFrame>();
        break;
    default:
        throw std::bad_variant_access();
    }
    frame->width = payload.width;
    frame->height = payload.height;
    frame->channels = static_cast<unsigned>(payload.channels);
    frame->pix_fmt = payload.pix_fmt;
    frame->storage = std::move(payload.storage);
    return frame;
}
