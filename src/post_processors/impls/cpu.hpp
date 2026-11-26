/*
 * src/post_processors/impls/cpu.hpp
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include "../../_base.hpp"
#include "../../frame.hpp"
#include "../processor.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

class CpuProcessor final : public IFrameProcessor {
  public:
    CpuProcessor(int width, int height, OutputFormat format);
    ~CpuProcessor() override;
    FramePayload process(const AVFrame *src) override;

  private:
    int _width;
    int _height;
    AVPixelFormat _format;

    SwsContext *_sws = nullptr;
};

class NativeCpuProcessor final : public IFrameProcessor {
  public:
    FramePayload process(const AVFrame *src) override;
};
