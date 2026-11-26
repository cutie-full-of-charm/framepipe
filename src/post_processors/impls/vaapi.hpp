/*
 * src/post_processors/impls/vaapi.hpp
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include "../../frame.hpp"
#include "../processor.hpp"
#include <memory>
#include <new>
#include <stdexcept>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

class VaapiProcessor final : public IFrameProcessor {
  public:
    VaapiProcessor(AVBufferRef *input_hw_frames_ctx, int input_width, int input_height, int output_width,
                   int output_height, AVRational time_base);

    ~VaapiProcessor() override;

    FramePayload process(const AVFrame *src) override;

  private:
    AVFilterGraph *graph_ = nullptr;
    AVFilterContext *source_ = nullptr;
    AVFilterContext *sink_ = nullptr;

    int output_width_;
    int output_height_;
};

class VaapiPassthroughProcessor final : public IFrameProcessor {
  public:
    FramePayload process(const AVFrame *src) override {
        if (!src || src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx) {
            throw std::runtime_error("Expected VAAPI frame");
        }

        AVFrame *raw = av_frame_alloc();
        if (!raw) {
            throw std::bad_alloc();
        }

        std::shared_ptr<AVFrame> frame(raw, [](AVFrame *f) { av_frame_free(&f); });

        const int ret = av_frame_ref(frame.get(), src);
        if (ret < 0) {
            throw std::runtime_error("av_frame_ref failed");
        }

        return FramePayload{
            .width = frame->width,
            .height = frame->height,
            .pix_fmt = AV_PIX_FMT_VAAPI,
            .storage = VaapiStorage{std::move(frame)},
        };
    }
};
