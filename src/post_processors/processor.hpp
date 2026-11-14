// src/post_processors/processor.hpp
#pragma once

#include "../frame.hpp"
extern "C" {
#include <libavutil/frame.h>
}

class IFrameProcessor {
  public:
    virtual ~IFrameProcessor() = default;
    virtual FramePayload process(const AVFrame *src) = 0;
};
