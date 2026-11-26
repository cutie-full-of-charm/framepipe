/*
 * src/gpu/cuda.cu
 * SPDX-License-Identifier: MPL-2.0
 */
#include "cuda.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

extern "C" {
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <libavutil/frame.h>
#include <sys/types.h>
#include <vector_types.h>
}

__device__ __forceinline__ uint8_t clamp_u8(float v) { return static_cast<uint8_t>(fminf(fmaxf(v, 0.f), 255.f)); }

__device__ float bilinear(const uint8_t *src, int pitch, int width, int height, float x, float y, int stride,
                          int offset) {

    x = fminf(fmaxf(x, 0.f), width - 1.f);
    y = fminf(fmaxf(y, 0.f), height - 1.f);

    int x0 = static_cast<int>(floorf(x));
    int y0 = static_cast<int>(floorf(y));
    int x1 = min(x0 + 1, width - 1);
    int y1 = min(y0 + 1, height - 1);

    float dx = x - x0;
    float dy = y - y0;

    float a = src[y0 * pitch + x0 * stride + offset];
    float b = src[y0 * pitch + x1 * stride + offset];
    float c = src[y1 * pitch + x0 * stride + offset];
    float d = src[y1 * pitch + x1 * stride + offset];

    return (1.f - dy) * ((1.f - dx) * a + dx * b) + dy * ((1.f - dx) * c + dx * d);
}

__global__ void nv12_resize_kernel(const uint8_t *y_plane, const uint8_t *uv_plane, int y_pitch, int uv_pitch,
                                   int src_w, int src_h, uint8_t *dst, uint out_w, uint out_h, int channels, bool bgr) {

    uint x = blockIdx.x * blockDim.x + threadIdx.x;
    uint y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= out_w || y >= out_h) {
        return;
    }

    float sx = (x + 0.5f) * src_w / out_w - 0.5f;
    float sy = (y + 0.5f) * src_h / out_h - 0.5f;

    float Y = bilinear(y_plane, y_pitch, src_w, src_h, sx, sy, 1, 0);

    int idx = (y * out_w + x) * channels;

    if (channels == 1) {
        dst[idx] = clamp_u8(Y);
        return;
    }

    float U = bilinear(uv_plane, uv_pitch, src_w / 2, src_h / 2, (sx - 0.5f) / 2.f, (sy - 0.5f) / 2.f, 2, 0);

    float V = bilinear(uv_plane, uv_pitch, src_w / 2, src_h / 2, (sx - 0.5f) / 2.f, (sy - 0.5f) / 2.f, 2, 1);

    float C = 1.16438356f * (Y - 16.f);
    float D = U - 128.f;
    float E = V - 128.f;

    uint8_t R = clamp_u8(C + 1.79274107f * E);
    uint8_t G = clamp_u8(C - 0.21324861f * D - 0.53290933f * E);
    uint8_t B = clamp_u8(C + 2.11240179f * D);

    dst[idx + 0] = bgr ? B : R;
    dst[idx + 1] = G;
    dst[idx + 2] = bgr ? R : B;
}

cudaError_t cuda_resize_convert(const AVFrame *src, uint8_t *dst, uint out_w, uint out_h, CudaOutputFormat format,
                                cudaStream_t stream) {

    int channels = format == CudaOutputFormat::GRAY ? 1 : 3;
    bool bgr = format == CudaOutputFormat::BGR;

    dim3 block(16, 16);
    dim3 grid((out_w + 15) / 16, (out_h + 15) / 16);

    nv12_resize_kernel<<<grid, block, 0, stream>>>(src->data[0], src->data[1], src->linesize[0], src->linesize[1],
                                                   src->width, src->height, dst, out_w, out_h, channels, bgr);

    return cudaGetLastError();
}

cudaError_t cuda_alloc_output(uint8_t **dst, size_t bytes) { return cudaMalloc(reinterpret_cast<void **>(dst), bytes); }

cudaError_t cuda_free_output(uint8_t *dst) { return cudaFree(dst); }

cudaError_t cuda_wait(cudaStream_t stream) { return cudaStreamSynchronize(stream); }
