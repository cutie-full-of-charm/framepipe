// src/post_processors/transfer.hpp
#pragma once

#include "../_base.hpp"
#include "../frame.hpp"
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
}

struct AVFrameDeleter {
    void operator()(AVFrame *frame) const noexcept { av_frame_free(&frame); }
};

using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

inline FramePtr make_frame() {
    FramePtr frame(av_frame_alloc());

    if (!frame) {
        throw std::bad_alloc();
    }
    return frame;
}

class IFrameTransfer {
  public:
    virtual ~IFrameTransfer() = default;
    virtual FramePtr transfer(const AVFrame *src) = 0;
};

// нулевой трансфер кадра для постпроцессинга, в том же устройстве, где был
// декодирован кадр
class RefTransfer final : public IFrameTransfer {
  public:
    FramePtr transfer(const AVFrame *src) override {
        auto dst = make_frame();

        const int ret = av_frame_ref(dst.get(), src);
        if (ret < 0) {
            throw std::runtime_error("av_frame_ref failed: " + std::to_string(ret));
        }

        return dst;
    }
};

// трансфер кадра из устройства, где он был декодирован, в CPU
class HwToCpuTransfer final : public IFrameTransfer {
  public:
    FramePtr transfer(const AVFrame *src) override {
        if (!src->hw_frames_ctx) {
            throw std::runtime_error("Expected hardware frame");
        }
        auto dst = make_frame();

        int ret = av_hwframe_transfer_data(dst.get(), src, 0);
        if (ret < 0) {
            throw std::runtime_error("Hardware frame download failed");
        }

        ret = av_frame_copy_props(dst.get(), src);
        if (ret < 0) {
            throw std::runtime_error("Failed to copy frame properties");
        }

        return dst;
    }
};

class IOutputTransfer {
  public:
    virtual ~IOutputTransfer() = default;
    virtual FramePayload transfer(FramePayload frame) = 0;
};

class NoOpOutputTransfer final : public IOutputTransfer {
  public:
    FramePayload transfer(FramePayload frame) override { return frame; }
};

// трансферим постобработанный кадр из CUDA VRAM -> CPU RAM
// implemetation moved
class CudaToCpuTransfer final : public IOutputTransfer {
  public:
    FramePayload transfer(FramePayload frame) override;
};

// трансферим постобработанный кадр из VAAPI VRAM -> CPU RAM
struct SwsContext;

class VaapiToCpuTransfer final : public IOutputTransfer {
  public:
    explicit VaapiToCpuTransfer(OutputFormat format);
    ~VaapiToCpuTransfer() override;

    VaapiToCpuTransfer(const VaapiToCpuTransfer &) = delete;
    VaapiToCpuTransfer &operator=(const VaapiToCpuTransfer &) = delete;

    FramePayload transfer(FramePayload frame) override;

  private:
    OutputFormat output_format_;
    SwsContext *sws_ = nullptr;
};
