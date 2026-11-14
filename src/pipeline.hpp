// src/pipeline.hpp
#pragma once
#include "frame.hpp"
#include "post_processors/processor.hpp"
#include "post_processors/transfer.hpp"

#include <memory>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
}

// #include <chrono>
// #include <print>
// #include <ratio>
// using Clock = std::chrono::steady_clock;

class FramePipeline {
  public:
    FramePipeline(std::unique_ptr<IFrameTransfer> input_transfer, std::unique_ptr<IFrameProcessor> processor,
                  std::unique_ptr<IOutputTransfer> output_transfer)
        : _input_transfer(std::move(input_transfer)), _processor(std::move(processor)),
          _output_transfer(std::move(output_transfer)) {}
    // FIXME: логгирование затрат времени на разные этапы пайплайна
    FramePayload process(const AVFrame *frame) {
        auto prepared = _input_transfer->transfer(frame);
        // auto t0 = Clock::now();
        auto processed = _processor->process(prepared.get());
        // auto t1 = Clock::now();
        auto result = _output_transfer->transfer(std::move(processed));
        // auto t2 = Clock::now();
        // std::print(
        //     "VPP: {:.3f} ms, GPU->CPU: {:.3f} ms\n",
        //     std::chrono::duration<double, std::milli>(t1 - t0).count(),
        //     std::chrono::duration<double, std::milli>(t2 - t1).count()
        // );
        return result;
    }

  private:
    std::unique_ptr<IFrameTransfer> _input_transfer;
    std::unique_ptr<IFrameProcessor> _processor;
    std::unique_ptr<IOutputTransfer> _output_transfer;
};
