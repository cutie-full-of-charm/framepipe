// src/gpu/cuda.hpp
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <libavutil/frame.h>
#include <sys/types.h>
}

#if defined(_WIN32) || defined(_WIN64)
using uint = std::uint32_t;
#endif

enum class CudaOutputFormat { RGB, BGR, GRAY, NATIVE };

cudaError_t cuda_resize_convert(const AVFrame *src, uint8_t *dst, uint out_w, uint out_h, CudaOutputFormat format,
                                cudaStream_t stream = nullptr);
cudaError_t cuda_alloc_output(uint8_t **dst, size_t bytes);
cudaError_t cuda_free_output(uint8_t *dst);
cudaError_t cuda_wait(cudaStream_t stream = nullptr);
