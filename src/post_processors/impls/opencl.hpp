/*
 * src/post_processors/impls/opencl.hpp
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once
#include "../../_base.hpp"
#include "../../frame.hpp"
#include "../processor.hpp"

#include <CL/cl.h>
#include <cstddef>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace spdlog {
class logger;
}

// VAAPI NV12 -> OpenCL Y/UV images -> packed CPU pixels (rgb/bgr/gray) or cl_mem GPU buffer.
class OpenCLProcessor final : public IFrameProcessor {
  public:
    OpenCLProcessor(AVBufferRef *vaapi_frames, int out_w, int out_h, OutputFormat format, MemoryType output_memory,
                    std::shared_ptr<spdlog::logger> logger = {});
    ~OpenCLProcessor() override;
    OpenCLProcessor(const OpenCLProcessor &) = delete;
    OpenCLProcessor &operator=(const OpenCLProcessor &) = delete;
    FramePayload process(const AVFrame *src) override;

  private:
    AVBufferRef *device_ = nullptr;
    AVBufferRef *frames_ = nullptr;
    cl_command_queue queue_ = nullptr;
    cl_program program_ = nullptr;
    cl_kernel kernel_ = nullptr;
    cl_mem output_ = nullptr;
    size_t output_bytes_ = 0;
    int width_;
    int height_;
    int channels_;
    OutputFormat format_;
    MemoryType output_memory_;
    cl_context context_ = nullptr;
    std::shared_ptr<spdlog::logger> logger_;
    bool have_previous_color_metadata_ = false;
    int previous_matrix_ = -1;
    int previous_range_ = -1;
    int previous_chroma_ = -1;
};
