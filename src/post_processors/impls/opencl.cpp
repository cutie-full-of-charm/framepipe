/*
 * src/post_processors/impls/opencl.cpp
 * SPDX-License-Identifier: MPL-2.0
 */
#include "opencl.hpp"
#include "../../_base.hpp"
#include "../../frame.hpp"
#include "opencl_kernel_source.hpp" // Generated from src/gpu/nv12_convert.cl at build time.

#include <CL/cl.h>
#include <CL/cl_platform.h>
#include <cstddef>
#include <cstdint>
// #include <format>
#include <libavutil/buffer.h>
#include <memory>
#include <new>
// #include <print>
#include <spdlog/logger.h>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_opencl.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <va/va.h>
}

namespace {
int check_av(int ret, const char *what) {
    if (ret >= 0)
        return ret;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(ret, message, sizeof(message));
    throw std::runtime_error(std::string(what) + ": " + message);
}
void check_cl(cl_int ret, const char *what) {
    if (ret != CL_SUCCESS)
        throw std::runtime_error(std::string(what) + ": OpenCL error " + std::to_string(ret));
}
using AVFrameOwner = std::unique_ptr<AVFrame, void (*)(AVFrame *)>;
AVFrameOwner make_avframe() {
    auto *ptr = av_frame_alloc();
    if (!ptr)
        throw std::bad_alloc();
    return {ptr, [](AVFrame *f) { av_frame_free(&f); }};
}
} // namespace

OpenCLProcessor::OpenCLProcessor(AVBufferRef *vaapi_frames, int out_w, int out_h, OutputFormat format,
                                 MemoryType output_memory, std::shared_ptr<spdlog::logger> logger)
    : width_(out_w), height_(out_h), channels_(format == OutputFormat::NATIVE  ? 0
                                               : format == OutputFormat::GRAY8 ? 1
                                                                               : 3),
      format_(format), output_memory_(output_memory), logger_(std::move(logger)) {
    if (!vaapi_frames || out_w <= 0 || out_h <= 0 ||
        (format != OutputFormat::BGR24 && format != OutputFormat::RGB24 && format != OutputFormat::GRAY8 &&
         format != OutputFormat::NATIVE))
        throw std::invalid_argument("OpenCLProcessor requires VAAPI NV12 and "
                                    "BGR24/RGB24/GRAY8/NATIVE output");

    auto *input = reinterpret_cast<AVHWFramesContext *>(vaapi_frames->data);
    if (input->sw_format != AV_PIX_FMT_NV12)
        throw std::runtime_error("OpenCLProcessor requires 8-bit VAAPI NV12 surfaces");

    try {
        check_av(av_hwdevice_ctx_create_derived(&device_, AV_HWDEVICE_TYPE_OPENCL, input->device_ref, 0),
                 "derive VAAPI -> OpenCL device");
        check_av(av_hwframe_ctx_create_derived(&frames_, AV_PIX_FMT_OPENCL, device_, vaapi_frames, 0),
                 "derive VAAPI -> OpenCL frames context");
        auto *device_ctx = reinterpret_cast<AVHWDeviceContext *>(device_->data);
        auto *opencl = reinterpret_cast<AVOpenCLDeviceContext *>(device_ctx->hwctx);
        context_ = opencl->context;
        cl_int err = CL_SUCCESS;
        queue_ = clCreateCommandQueueWithProperties(opencl->context, opencl->device_id, nullptr, &err);
        check_cl(err, "create OpenCL command queue");
        if (!queue_)
            throw std::runtime_error("OpenCL command queue is null");

        // FIXME?
        // формат NATIVE поддерживает только VAAPI NV12
        if (format_ != OutputFormat::NATIVE) {
            const char *src = kFramepipeOpenCLKernelSource;
            const size_t source_size = sizeof(kFramepipeOpenCLKernelSource) - 1;
            program_ = clCreateProgramWithSource(opencl->context, 1, &src, &source_size, &err);
            check_cl(err, "create OpenCL program");
            if (!program_)
                throw std::runtime_error("OpenCL program is null");

            err = clBuildProgram(program_, 1, &opencl->device_id, "-cl-std=CL1.2", nullptr, nullptr);
            if (err != CL_SUCCESS) {
                size_t len = 0;
                clGetProgramBuildInfo(program_, opencl->device_id, CL_PROGRAM_BUILD_LOG, 0, nullptr, &len);
                std::string log(len, '\0');
                if (len)
                    clGetProgramBuildInfo(program_, opencl->device_id, CL_PROGRAM_BUILD_LOG, len, log.data(), nullptr);
                throw std::runtime_error("OpenCL kernel build failed: " + log);
            }
            kernel_ = clCreateKernel(program_, "nv12_convert", &err);
            check_cl(err, "create nv12_convert kernel");
            if (!kernel_)
                throw std::runtime_error("OpenCL kernel is null");
        }

        output_bytes_ = static_cast<size_t>(width_) * static_cast<size_t>(height_) * channels_;
        if (format_ != OutputFormat::NATIVE && output_memory_ == MemoryType::CPU) {
            output_ = clCreateBuffer(opencl->context, CL_MEM_WRITE_ONLY, output_bytes_, nullptr, &err);
            check_cl(err, "allocate OpenCL CPU-download buffer");
            if (!output_)
                throw std::runtime_error("OpenCL output buffer is null");
        }
    } catch (...) {
        if (output_)
            clReleaseMemObject(output_);
        if (kernel_)
            clReleaseKernel(kernel_);
        if (program_)
            clReleaseProgram(program_);
        if (queue_)
            clReleaseCommandQueue(queue_);
        av_buffer_unref(&frames_);
        av_buffer_unref(&device_);
        throw;
    }
}

OpenCLProcessor::~OpenCLProcessor() {
    if (queue_)
        clFinish(queue_);
    if (output_)
        clReleaseMemObject(output_);
    if (kernel_)
        clReleaseKernel(kernel_);
    if (program_)
        clReleaseProgram(program_);
    if (queue_)
        clReleaseCommandQueue(queue_);
    av_buffer_unref(&frames_);
    av_buffer_unref(&device_);
}

FramePayload OpenCLProcessor::process(const AVFrame *src) {
    if (!src || src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx)
        throw std::runtime_error("OpenCLProcessor expected VAAPI AVFrame");
    auto *input = reinterpret_cast<AVHWFramesContext *>(src->hw_frames_ctx->data);
    if (!input || input->sw_format != AV_PIX_FMT_NV12)
        throw std::runtime_error("OpenCLProcessor currently supports NV12 only");

    auto mapped = make_avframe();
    mapped->format = AV_PIX_FMT_OPENCL;
    mapped->hw_frames_ctx = av_buffer_ref(frames_);
    if (!mapped->hw_frames_ctx)
        throw std::bad_alloc();

    check_av(av_hwframe_map(mapped.get(), src, AV_HWFRAME_MAP_READ), "map VAAPI NV12 to OpenCL");

    auto y = reinterpret_cast<cl_mem>(mapped->data[0]);
    auto uv = reinterpret_cast<cl_mem>(mapped->data[1]);
    if (!y || !uv)
        throw std::runtime_error("OpenCL mapping returned missing NV12 plane");

    if (format_ == OutputFormat::NATIVE) {
        if (src->width != width_ || src->height != height_)
            // std::print("src->width={}, width_={}, src->height={},
            // height_={}",
            //            src->width, width_, src->height, height_);
            throw std::runtime_error("OpenCL NATIVE NV12 does not support resize");

        if (output_memory_ == MemoryType::GPU) {
            // This mapped frame retains both OpenCL images and their VAAPI backing.
            auto owner = std::shared_ptr<void>(mapped.release(), [](void *p) {
                auto *f = static_cast<AVFrame *>(p);
                av_frame_free(&f);
            });
            return FramePayload{
                .width = width_,
                .height = height_,
                .pix_fmt = AV_PIX_FMT_NV12,
                .storage = OpenCLImageStorage{std::move(owner), {y, uv}, context_},
            };
        }

        const size_t w = static_cast<size_t>(width_);
        const size_t h = static_cast<size_t>(height_);
        const size_t cw = (w + 1) / 2;
        const size_t ch = (h + 1) / 2;
        const size_t uv_pitch = cw * 2;
        const size_t size = w * h + uv_pitch * ch;
        auto *memory = static_cast<uint8_t *>(av_malloc(size));
        if (!memory)
            throw std::bad_alloc();
        std::shared_ptr<void> owner(memory, [](void *p) { av_free(p); });
        const size_t origin[3] = {0, 0, 0};
        const size_t y_region[3] = {w, h, 1};
        const size_t uv_region[3] = {cw, ch, 1};
        check_cl(clEnqueueReadImage(queue_, y, CL_TRUE, origin, y_region, w, 0, memory, 0, nullptr, nullptr),
                 "read native NV12 Y");
        check_cl(clEnqueueReadImage(queue_, uv, CL_TRUE, origin, uv_region, uv_pitch, 0, memory + w * h, 0, nullptr,
                                    nullptr),
                 "read native NV12 UV");
        CpuPlanarStorage storage{};
        storage.owner = std::move(owner);
        storage.planes[0] = memory;
        storage.planes[1] = memory + w * h;
        storage.linesize[0] = static_cast<int>(w);
        storage.linesize[1] = static_cast<int>(uv_pitch);
        return FramePayload{
            .width = width_,
            .height = height_,
            .pix_fmt = AV_PIX_FMT_NV12,
            .storage = std::move(storage),
        };
    }

    struct FinishOnError {
        cl_command_queue queue;
        bool completed = false;
        ~FinishOnError() {
            if (!completed)
                clFinish(queue);
        }
    } finish_guard{queue_};

    const int matrix = static_cast<int>(src->colorspace);
    const int range = static_cast<int>(src->color_range);
    const int chroma = static_cast<int>(src->chroma_location);
    const bool metadata_changed = !have_previous_color_metadata_ || matrix != previous_matrix_ ||
                                  range != previous_range_ || chroma != previous_chroma_;
    if (metadata_changed) {
        have_previous_color_metadata_ = true;
        previous_matrix_ = matrix;
        previous_range_ = range;
        previous_chroma_ = chroma;
    }

    float kr = 0.299f, kb = 0.114f;
    const char *matrix_name = "BT.601";
    bool matrix_fallback = false;
    switch (src->colorspace) {
    case AVCOL_SPC_BT709:
        kr = 0.2126f;
        kb = 0.0722f;
        matrix_name = "BT.709";
        break;
    case AVCOL_SPC_BT2020_NCL:
        kr = 0.2627f;
        kb = 0.0593f;
        matrix_name = "BT.2020 NCL";
        break;
    case AVCOL_SPC_SMPTE240M:
        kr = 0.212f;
        kb = 0.087f;
        matrix_name = "SMPTE 240M";
        break;
    case AVCOL_SPC_FCC:
        kr = 0.30f;
        kb = 0.11f;
        matrix_name = "FCC";
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        break; // BT.601
    case AVCOL_SPC_UNSPECIFIED:
        matrix_fallback = true;
        break;
    default:
        throw std::runtime_error("OpenCLProcessor: unsupported color matrix " + std::to_string(matrix) +
                                 " (cannot convert safely using YCbCr coefficients)");
    }

    bool range_fallback = false;
    int full_range = 0;
    switch (src->color_range) {
    case AVCOL_RANGE_JPEG:
        full_range = 1;
        break;
    case AVCOL_RANGE_MPEG:
        break;
    default:
        range_fallback = true;
        break; // assume limited (TV)
    }

    // Chroma center mappings in OpenCL unnormalized image coordinates.
    // Luma centers are (i+0.5, j+0.5); UV centers are (i+0.5, j+0.5).
    // For 4:2:0, horizontal left -> +0.25, center -> 0;
    // vertical top -> +0.25, middle -> 0, bottom -> -0.25.
    float chroma_offset_x = 0.25f, chroma_offset_y = 0.0f;
    const char *chroma_name = "left";
    bool chroma_fallback = false;
    switch (src->chroma_location) {
    case AVCHROMA_LOC_LEFT:
        break;
    case AVCHROMA_LOC_CENTER:
        chroma_offset_x = 0.0f;
        chroma_name = "center";
        break;
    case AVCHROMA_LOC_TOPLEFT:
        chroma_offset_y = 0.25f;
        chroma_name = "topleft";
        break;
    case AVCHROMA_LOC_TOP:
        chroma_offset_x = 0.0f;
        chroma_offset_y = 0.25f;
        chroma_name = "top";
        break;
    case AVCHROMA_LOC_BOTTOMLEFT:
        chroma_offset_y = -0.25f;
        chroma_name = "bottomleft";
        break;
    case AVCHROMA_LOC_BOTTOM:
        chroma_offset_x = 0.0f;
        chroma_offset_y = -0.25f;
        chroma_name = "bottom";
        break;
    default:
        chroma_fallback = true;
        break; // MPEG-2/H.264 4:2:0 left
    }
    if (metadata_changed && logger_) {
        if (matrix_fallback)
            logger_->warn("OpenCL: source color matrix={} unspecified; fallback BT.601", matrix);
        if (range_fallback)
            logger_->warn("OpenCL: source color range={} unspecified/invalid; "
                          "fallback limited (TV)",
                          range);
        if (chroma_fallback)
            logger_->warn("OpenCL: source chroma_location={} unspecified/invalid; "
                          "fallback left",
                          chroma);
        logger_->info("OpenCL color: matrix={} range={} chroma={} (source: "
                      "matrix={} range={} chroma={})",
                      matrix_name, full_range ? "full" : "limited", chroma_name, matrix, range, chroma);
    }
    const int bgr = format_ == OutputFormat::BGR24 ? 1 : 0;
    const int sw = src->width, sh = src->height;

    cl_mem destination = output_;
    std::shared_ptr<void> gpu_owner;
    if (output_memory_ == MemoryType::GPU) {
        cl_int err = CL_SUCCESS;
        destination = clCreateBuffer(context_, CL_MEM_READ_WRITE, output_bytes_, nullptr, &err);
        check_cl(err, "allocate per-frame GPU output");
        if (!destination)
            throw std::runtime_error("OpenCL GPU output buffer is null");
        gpu_owner = std::shared_ptr<void>(destination, [](void *p) {
            if (p)
                clReleaseMemObject(static_cast<cl_mem>(p));
        });
    }

    check_cl(clSetKernelArg(kernel_, 0, sizeof(y), &y), "set Y input");
    check_cl(clSetKernelArg(kernel_, 1, sizeof(uv), &uv), "set UV input");
    check_cl(clSetKernelArg(kernel_, 2, sizeof(destination), &destination), "set output buffer");
    check_cl(clSetKernelArg(kernel_, 3, sizeof(sw), &sw), "set source width");
    check_cl(clSetKernelArg(kernel_, 4, sizeof(sh), &sh), "set source height");
    check_cl(clSetKernelArg(kernel_, 5, sizeof(width_), &width_), "set output width");
    check_cl(clSetKernelArg(kernel_, 6, sizeof(height_), &height_), "set output height");
    check_cl(clSetKernelArg(kernel_, 7, sizeof(channels_), &channels_), "set channels");
    check_cl(clSetKernelArg(kernel_, 8, sizeof(bgr), &bgr), "set BGR flag");
    check_cl(clSetKernelArg(kernel_, 9, sizeof(kr), &kr), "set Kr");
    check_cl(clSetKernelArg(kernel_, 10, sizeof(kb), &kb), "set Kb");
    check_cl(clSetKernelArg(kernel_, 11, sizeof(full_range), &full_range), "set YUV range");
    check_cl(clSetKernelArg(kernel_, 12, sizeof(chroma_offset_x), &chroma_offset_x), "set chroma X offset");
    check_cl(clSetKernelArg(kernel_, 13, sizeof(chroma_offset_y), &chroma_offset_y), "set chroma Y offset");

    size_t global[] = {static_cast<size_t>(width_), static_cast<size_t>(height_)};
    check_cl(clEnqueueNDRangeKernel(queue_, kernel_, 2, nullptr, global, nullptr, 0, nullptr, nullptr),
             "enqueue NV12 conversion");

    const auto pix_fmt = format_ == OutputFormat::BGR24   ? AV_PIX_FMT_BGR24
                         : format_ == OutputFormat::RGB24 ? AV_PIX_FMT_RGB24
                                                          : AV_PIX_FMT_GRAY8;
    if (output_memory_ == MemoryType::GPU) {
        check_cl(clFinish(queue_), "finish OpenCL GPU output");
        finish_guard.completed = true;
        return FramePayload{
            .width = width_,
            .height = height_,
            .channels = channels_,
            .pix_fmt = pix_fmt,
            .storage = OpenCLBufferStorage{std::move(gpu_owner), destination, context_},
        };
    }

    uint8_t *bytes = static_cast<uint8_t *>(av_malloc(output_bytes_));
    if (!bytes)
        throw std::bad_alloc();
    std::shared_ptr<void> owner(bytes, [](void *p) { av_free(p); });
    check_cl(clEnqueueReadBuffer(queue_, destination, CL_TRUE, 0, output_bytes_, bytes, 0, nullptr, nullptr),
             "download OpenCL output");
    finish_guard.completed = true;
    return FramePayload{
        .width = width_,
        .height = height_,
        .channels = channels_,
        .pix_fmt = pix_fmt,
        .storage = CpuPackedStorage{std::move(owner), bytes, width_ * channels_},
    };
}
