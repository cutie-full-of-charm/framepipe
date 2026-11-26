/*
 * src/post_processors/impls/cuda.hpp
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include "../../_base.hpp"
#include "../../frame.hpp"
#include "../processor.hpp"

extern "C" {
#include <libavutil/frame.h>
}

class CudaProcessor final : public IFrameProcessor {
  public:
    CudaProcessor(int width, int height, OutputFormat format);
    FramePayload process(const AVFrame *src) override;

  private:
    int _width;
    int _height;
    OutputFormat _format;
};
