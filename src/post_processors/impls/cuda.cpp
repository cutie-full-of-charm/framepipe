// src/post_processors/impls/cuda.cpp
#include "cuda.hpp"
#include "../../_base.hpp"
#include "../../gpu/cuda.hpp"
#include "../processor.hpp"
#include "../transfer.hpp"

#include <cstddef>
#include <cstdint>

#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "../../frame.hpp"
#include "cuda_runtime_api.h"
#include "driver_types.h"
#include <cuda.h>
#include <variant>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace {

void check_cuda(cudaError_t error) {
    if (error != cudaSuccess) {
        throw std::runtime_error(cudaGetErrorString(error));
    }
}

class ContextGuard {
  public:
    explicit ContextGuard(CUcontext context) {
        if (cuCtxPushCurrent(context) != CUDA_SUCCESS) {
            throw std::runtime_error("Failed to activate FFmpeg CUDA context");
        }
    }

    ~ContextGuard() {
        CUcontext previous = nullptr;
        cuCtxPopCurrent(&previous);
    }

    ContextGuard(const ContextGuard &) = delete;
    ContextGuard &operator=(const ContextGuard &) = delete;
};

CudaOutputFormat to_cuda_format(OutputFormat format) {
    switch (format) {
    case OutputFormat::RGB24:
        return CudaOutputFormat::RGB;
    case OutputFormat::BGR24:
        return CudaOutputFormat::BGR;
    case OutputFormat::GRAY8:
        return CudaOutputFormat::GRAY;
    case OutputFormat::NATIVE:
        return CudaOutputFormat::NATIVE;
    default:
        throw std::runtime_error("Unsupported CUDA output format");
    }
}

} // namespace

CudaProcessor::CudaProcessor(int width, int height, OutputFormat format)
    : _width(width), _height(height), _format(format) {

    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Invalid output size");
    }

    to_cuda_format(format);
}

FramePayload CudaProcessor::process(const AVFrame *src) {
    if (_format == OutputFormat::NATIVE) {
        throw std::invalid_argument("CudaProcessor does not support NATIVE output");
    }

    if (!src || src->format != AV_PIX_FMT_CUDA || !src->hw_frames_ctx) {
        throw std::runtime_error("CudaProcessor requires CUDA AVFrame");
    }

    auto *frames_context = reinterpret_cast<AVHWFramesContext *>(src->hw_frames_ctx->data);
    if (frames_context->sw_format != AV_PIX_FMT_NV12) {
        throw std::runtime_error("Only CUDA NV12 is supported");
    }

    auto *device = reinterpret_cast<AVHWDeviceContext *>(frames_context->device_ref->data);
    auto *cuda_device = reinterpret_cast<AVCUDADeviceContext *>(device->hwctx);
    CUcontext context = cuda_device->cuda_ctx;
    ContextGuard guard(context);

    const int channels = _format == OutputFormat::GRAY8 ? 1 : 3;
    const size_t bytes = static_cast<size_t>(_width) * _height * channels;

    uint8_t *raw = nullptr;
    check_cuda(cuda_alloc_output(&raw, bytes));

    // сохраняем FFmpeg device context живым пока существует выходной CUDA-буфер
    AVBufferRef *device_ref = av_buffer_ref(frames_context->device_ref);

    if (!device_ref) {
        cuda_free_output(raw);
        throw std::bad_alloc();
    }

    std::shared_ptr<void> owner(raw, [device_ref, context](void *ptr) {
        CUcontext previous = nullptr;

        if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            cuda_free_output(static_cast<uint8_t *>(ptr));
            cuCtxPopCurrent(&previous);
        }

        AVBufferRef *ref = device_ref;
        av_buffer_unref(&ref);
    });

    check_cuda(cuda_resize_convert(src, raw, _width, _height, to_cuda_format(_format)));
    check_cuda(cuda_wait());

    return FramePayload{
        .width = _width,
        .height = _height,
        .channels = channels,
        .pix_fmt = _format == OutputFormat::RGB24   ? AV_PIX_FMT_RGB24
                   : _format == OutputFormat::BGR24 ? AV_PIX_FMT_BGR24
                                                    : AV_PIX_FMT_GRAY8,
        .storage = CudaStorage{std::move(owner), raw, context},
    };
}

FramePayload CudaToCpuTransfer::transfer(FramePayload frame) {
    auto *cuda = std::get_if<CudaStorage>(&frame.storage);
    if (!cuda || !cuda->context || !cuda->data)
        throw std::runtime_error("Expected valid CUDA frame");

    const size_t bytes = static_cast<size_t>(frame.width) * frame.height * frame.channels;

    void *raw = av_malloc(bytes);
    if (!raw) {
        throw std::bad_alloc();
    }

    std::shared_ptr<void> owner(raw, [](void *p) { av_free(p); });

    if (cuCtxPushCurrent(cuda->context) != CUDA_SUCCESS) {
        throw std::runtime_error("Failed to activate CUDA context for download");
    }

    const cudaError_t ret = cudaMemcpy(raw, cuda->data, bytes, cudaMemcpyDeviceToHost);

    CUcontext previous = nullptr;
    const CUresult pop_ret = cuCtxPopCurrent(&previous);

    if (ret != cudaSuccess) {
        throw std::runtime_error(cudaGetErrorString(ret));
    }

    if (pop_ret != CUDA_SUCCESS) {
        throw std::runtime_error("Failed to restore CUDA context after download");
    }

    frame.storage = CpuPackedStorage{std::move(owner), static_cast<uint8_t *>(raw), frame.width * frame.channels};
    return frame;
}

#if defined(_WIN32)
#define FRAMEPIPE_CUDA_EXPORT extern "C" __declspec(dllexport)
#else
#define FRAMEPIPE_CUDA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

FRAMEPIPE_CUDA_EXPORT
IFrameProcessor *framepipe_create_cuda_processor(int width, int height, OutputFormat format) {
    return new CudaProcessor(width, height, format);
}

FRAMEPIPE_CUDA_EXPORT
IOutputTransfer *framepipe_create_cuda_transfer() { return new CudaToCpuTransfer(); }
