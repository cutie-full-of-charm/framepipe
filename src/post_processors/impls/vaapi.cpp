/*
 * src/post_processors/impls/vaapi.cpp
 * SPDX-License-Identifier: MPL-2.0
 */
#include "vaapi.hpp"
#include "../../_base.hpp"
#include "../../frame.hpp"
#include "../transfer.hpp"

#include <cerrno>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace {

void check_ffmpeg(int ret, const char *operation) {
    if (ret >= 0) {
        return;
    }

    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}

struct BufferSrcParamsDeleter {
    void operator()(AVBufferSrcParameters *params) const noexcept {
        if (!params) {
            return;
        }

        av_buffer_unref(&params->hw_frames_ctx);
        av_free(params);
    }
};

AVPixelFormat cpu_pixel_format(OutputFormat format, AVPixelFormat native_format) {
    switch (format) {
    case OutputFormat::NATIVE:
        return native_format;
    case OutputFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case OutputFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    case OutputFormat::GRAY8:
        return AV_PIX_FMT_GRAY8;
    default:
        throw std::invalid_argument("Unsupported output format");
    }
}

std::shared_ptr<AVFrame> make_shared_avframe() {
    AVFrame *raw = av_frame_alloc();
    if (!raw) {
        throw std::bad_alloc();
    }
    return {raw, [](AVFrame *f) { av_frame_free(&f); }};
}

} // namespace

VaapiProcessor::VaapiProcessor(AVBufferRef *input_hw_frames_ctx, int input_width, int input_height, int output_width,
                               int output_height, AVRational time_base)
    : output_width_(output_width), output_height_(output_height) {

    if (!input_hw_frames_ctx || !input_hw_frames_ctx->data) {
        throw std::invalid_argument("Missing VAAPI frames context");
    }

    if (input_width <= 0 || input_height <= 0 || output_width <= 0 || output_height <= 0) {
        throw std::invalid_argument("Invalid dimensions");
    }

    if (time_base.num <= 0 || time_base.den <= 0) {
        throw std::invalid_argument("Invalid input time base");
    }

    auto *hw = reinterpret_cast<AVHWFramesContext *>(input_hw_frames_ctx->data);

    if (hw->format != AV_PIX_FMT_VAAPI) {
        throw std::invalid_argument("Expected VAAPI hardware frames");
    }

    graph_ = avfilter_graph_alloc();
    if (!graph_) {
        throw std::bad_alloc();
    }

    try {
        const AVFilter *buffer = avfilter_get_by_name("buffer");
        const AVFilter *scale = avfilter_get_by_name("scale_vaapi");
        const AVFilter *buffersink = avfilter_get_by_name("buffersink");

        if (!buffer || !scale || !buffersink) {
            throw std::runtime_error("Required VAAPI filters unavailable");
        }

        source_ = avfilter_graph_alloc_filter(graph_, buffer, "input");
        if (!source_) {
            throw std::bad_alloc();
        }

        std::unique_ptr<AVBufferSrcParameters, BufferSrcParamsDeleter> params(av_buffersrc_parameters_alloc());

        if (!params) {
            throw std::bad_alloc();
        }

        params->format = AV_PIX_FMT_VAAPI;
        params->width = input_width;
        params->height = input_height;
        params->time_base = time_base;
        params->sample_aspect_ratio = AVRational{1, 1};
        params->hw_frames_ctx = av_buffer_ref(input_hw_frames_ctx);
        if (!params->hw_frames_ctx) {
            throw std::bad_alloc();
        }

        check_ffmpeg(av_buffersrc_parameters_set(source_, params.get()), "configure VAAPI buffer source");

        check_ffmpeg(avfilter_init_str(source_, nullptr), "initialize VAAPI buffer source");

        // VAAPI VPP используется только для ресайза,
        // FIXME: не помню почему здесь нет конверсии цвета ?из-за поддержки только rgba?
        const bool need_vpp = input_width != output_width_ || input_height != output_height_;

        check_ffmpeg(avfilter_graph_create_filter(&sink_, buffersink, "output", nullptr, nullptr, graph_),
                     "create buffer sink");

        if (need_vpp) {
            AVFilterContext *scale_ctx = nullptr;
            std::string args = "w=" + std::to_string(output_width_) + ":h=" + std::to_string(output_height_);

            check_ffmpeg(avfilter_graph_create_filter(&scale_ctx, scale, "scale_vaapi", args.c_str(), nullptr, graph_),
                         "create scale_vaapi");

            check_ffmpeg(avfilter_link(source_, 0, scale_ctx, 0), "link source to scale_vaapi");

            check_ffmpeg(avfilter_link(scale_ctx, 0, sink_, 0), "link scale_vaapi to sink");
        } else {
            check_ffmpeg(avfilter_link(source_, 0, sink_, 0), "link source directly to sink");
        }

        check_ffmpeg(avfilter_graph_config(graph_, nullptr), "configure VAAPI graph");
    } catch (...) {
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_ = nullptr;
        throw;
    }
}

VaapiProcessor::~VaapiProcessor() { avfilter_graph_free(&graph_); }

FramePayload VaapiProcessor::process(const AVFrame *src) {
    if (!src || src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx) {
        throw std::runtime_error("VaapiProcessor requires VAAPI AVFrame");
    }

    check_ffmpeg(av_buffersrc_add_frame_flags(source_, const_cast<AVFrame *>(src), AV_BUFFERSRC_FLAG_KEEP_REF),
                 "submit VAAPI frame");

    AVFrame *raw = av_frame_alloc();
    if (!raw) {
        throw std::bad_alloc();
    }

    std::shared_ptr<AVFrame> output(raw, [](AVFrame *f) { av_frame_free(&f); });

    const int ret = av_buffersink_get_frame(sink_, output.get());
    if (ret == AVERROR(EAGAIN)) {
        throw std::runtime_error("VAAPI filter has no output frame yet");
    }

    check_ffmpeg(ret, "receive VAAPI frame");

    if (output->format != AV_PIX_FMT_VAAPI || !output->hw_frames_ctx) {
        throw std::runtime_error("Expected VAAPI output frame");
    }

    return FramePayload{
        .width = output->width,
        .height = output->height,
        .pix_fmt = AV_PIX_FMT_VAAPI,
        .storage = VaapiStorage{std::move(output)},
    };
}

VaapiToCpuTransfer::VaapiToCpuTransfer(OutputFormat format) : output_format_(format) {}

VaapiToCpuTransfer::~VaapiToCpuTransfer() { sws_freeContext(sws_); }

// #include <chrono>
// #include <ratio>
// #include <print>
// using Clock = std::chrono::steady_clock;
FramePayload VaapiToCpuTransfer::transfer(FramePayload frame) {
    auto *vaapi = std::get_if<VaapiStorage>(&frame.storage);
    if (!vaapi || !vaapi->frame)
        throw std::runtime_error("Expected owned VAAPI frame");
    auto *gpu = vaapi->frame.get();
    if (!gpu || gpu->format != AV_PIX_FMT_VAAPI || !gpu->hw_frames_ctx) {
        throw std::runtime_error("Invalid VAAPI output frame");
    }

    auto downloaded = make_shared_avframe();
    // Exactly one transfer: let FFmpeg choose a supported native CPU format.
    // const auto t0 = Clock::now();
    check_ffmpeg(av_hwframe_transfer_data(downloaded.get(), gpu, 0), "download VAAPI frame");
    // const auto t1 = Clock::now();
    // const double transfer_ms = std::chrono::duration<double, std::milli>(t1 -
    // t0).count(); std::print("hw transfer ms={}\n", transfer_ms);

    check_ffmpeg(av_frame_copy_props(downloaded.get(), gpu), "copy VAAPI frame properties");

    const auto src_fmt = static_cast<AVPixelFormat>(downloaded->format);
    const auto dst_fmt = cpu_pixel_format(output_format_, src_fmt);
    auto output = downloaded;

    if (dst_fmt != src_fmt) {
        auto converted = make_shared_avframe();
        converted->format = dst_fmt;
        converted->width = downloaded->width;
        converted->height = downloaded->height;

        check_ffmpeg(av_frame_get_buffer(converted.get(), 32), "allocate converted CPU frame");

        SwsContext *ctx = sws_getCachedContext(sws_, downloaded->width, downloaded->height, src_fmt, converted->width,
                                               converted->height, dst_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!ctx) {
            sws_ = nullptr;
            throw std::runtime_error("Unable to initialize VAAPI CPU color conversion");
        }
        sws_ = ctx;

        const int rows = sws_scale(sws_, downloaded->data, downloaded->linesize, 0, downloaded->height, converted->data,
                                   converted->linesize);
        if (rows != converted->height) {
            throw std::runtime_error("VAAPI CPU color conversion failed");
        }
        check_ffmpeg(av_frame_copy_props(converted.get(), downloaded.get()), "copy converted frame properties");
        output = std::move(converted);
    }

    frame.width = output->width;
    frame.height = output->height;
    frame.pix_fmt = static_cast<AVPixelFormat>(output->format);
    frame.channels = output_format_ == OutputFormat::NATIVE ? 0 : output_format_ == OutputFormat::GRAY8 ? 1 : 3;
    if (output_format_ == OutputFormat::NATIVE) {
        CpuPlanarStorage storage{};
        storage.owner = std::static_pointer_cast<void>(output);
        for (int i = 0; i < 4; ++i) {
            storage.planes[i] = output->data[i];
            storage.linesize[i] = output->linesize[i];
        }
        frame.storage = std::move(storage);
    } else {
        frame.storage = CpuPackedStorage{std::static_pointer_cast<void>(output), output->data[0], output->linesize[0]};
    }
    return frame;
}
