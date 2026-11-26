/*
 * src/post_processors/impls/cpu.cpp
 * SPDX-License-Identifier: MPL-2.0
 */
#include "cpu.hpp"
#include "../../_base.hpp"
#include "../../frame.hpp"
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

AVPixelFormat to_pix_fmt(OutputFormat format) {
    switch (format) {
    case OutputFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case OutputFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    case OutputFormat::GRAY8:
        return AV_PIX_FMT_GRAY8;
    case OutputFormat::NATIVE:
        return AV_PIX_FMT_NONE;
    default:
        throw std::runtime_error("Unsupported CPU output format");
    }
}

} // namespace

CpuProcessor::CpuProcessor(int width, int height, OutputFormat format)
    : _width(width), _height(height), _format(to_pix_fmt(format)) {

    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Invalid output size");
    }
}

CpuProcessor::~CpuProcessor() { sws_freeContext(_sws); }

// базовый процессор, делает конверсию цвета и ресайз
// FIXME: возможно здесь делается лишняя работа, если скейл не требуется
FramePayload CpuProcessor::process(const AVFrame *src) {
    if (!src || src->hw_frames_ctx) {
        throw std::runtime_error("CpuProcessor requires software AVFrame");
    }

    if (src->width == _width && src->height == _height && static_cast<AVPixelFormat>(src->format) == _format) {
        AVFrame *ref = av_frame_clone(src);
        if (!ref) {
            throw std::bad_alloc();
        }
        std::shared_ptr<void> owner(ref, [](void *p) {
            auto *f = static_cast<AVFrame *>(p);
            av_frame_free(&f);
        });
        return FramePayload{
            .width = ref->width,
            .height = ref->height,
            .channels = _format == AV_PIX_FMT_GRAY8 ? 1 : 3,
            .pix_fmt = _format,
            .storage = CpuPackedStorage{std::move(owner), ref->data[0], ref->linesize[0]},
        };
    }

    _sws = sws_getCachedContext(_sws, src->width, src->height, static_cast<AVPixelFormat>(src->format), _width, _height,
                                _format, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!_sws) {
        throw std::runtime_error("sws_getCachedContext failed");
    }

    int bytes = av_image_get_buffer_size(_format, _width, _height, 1);
    if (bytes < 0) {
        throw std::runtime_error("Invalid output image size");
    }

    uint8_t *raw = static_cast<uint8_t *>(av_malloc(bytes + 64));
    if (!raw) {
        throw std::bad_alloc();
    }

    std::shared_ptr<void> owner(raw, [](void *p) { av_free(p); });

    uint8_t *dst_data[4] = {};
    int dst_linesize[4] = {};

    int ret = av_image_fill_arrays(dst_data, dst_linesize, raw, _format, _width, _height, 1);
    if (ret < 0) {
        throw std::runtime_error("av_image_fill_arrays failed");
    }

    ret = sws_scale(_sws, src->data, src->linesize, 0, src->height, dst_data, dst_linesize);
    if (ret != _height) {
        throw std::runtime_error("sws_scale failed");
    }

    return FramePayload{
        .width = _width,
        .height = _height,
        .channels = _format == AV_PIX_FMT_GRAY8 ? 1 : 3,
        .pix_fmt = _format,
        .storage = CpuPackedStorage{std::move(owner), raw, dst_linesize[0]},
    };
}

// нативный процессор, без конверсии цвета и ресазйа
FramePayload NativeCpuProcessor::process(const AVFrame *src) {
    if (!src || src->hw_frames_ctx) {
        throw std::runtime_error("Expected software AVFrame");
    }

    AVFrame *ref = av_frame_clone(src);
    if (!ref) {
        throw std::bad_alloc();
    }

    std::shared_ptr<void> owner(ref, [](void *p) {
        auto *frame = static_cast<AVFrame *>(p);
        av_frame_free(&frame);
    });

    CpuPlanarStorage storage{};
    storage.owner = std::move(owner);
    for (int i = 0; i < 4; ++i) {
        storage.planes[i] = ref->data[i];
        storage.linesize[i] = ref->linesize[i];
    }
    return FramePayload{
        .width = ref->width,
        .height = ref->height,
        .pix_fmt = static_cast<AVPixelFormat>(ref->format),
        .storage = std::move(storage),
    };
}
