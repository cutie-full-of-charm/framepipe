// src/gpu/nv12_convert.cl

__constant sampler_t sampler_linear = CLK_NORMALIZED_COORDS_FALSE |
    CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

__kernel void nv12_convert(read_only image2d_t y_image,
                           read_only image2d_t uv_image,
                           __global uchar* dst,
                           const int src_width, const int src_height,
                           const int dst_width, const int dst_height,
                           const int channels, const int bgr,
                           const float kr, const float kb,
                           const int full_range,
                           const float chroma_offset_x,
                           const float chroma_offset_y)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= dst_width || y >= dst_height) return;

    // Unnormalized coordinates: image centers at (i + 0.5, j + 0.5).
    // This is the pixel-center mapping for bilinear scale.
    const float2 coord = (float2)(
        ((float)x + 0.5f) * ((float)src_width / (float)dst_width),
        ((float)y + 0.5f) * ((float)src_height / (float)dst_height));

    const float yy = read_imagef(y_image, sampler_linear, coord).x;
    const size_t idx = ((size_t)y * (size_t)dst_width + (size_t)x) * (size_t)channels;

    const float Y = full_range ? yy : clamp(
        (yy - 16.0f / 255.0f) * (255.0f / 219.0f),
        0.0f,
        1.0f
    );

    if (channels == 1) {
        dst[idx] = convert_uchar_sat_rte(Y * 255.0f);
        return;
    }

    // FFmpeg AVChromaLocation expressed in unnormalized OpenCL UV coordinates.
    const float2 uv_coord = coord * 0.5f +
                            (float2)(chroma_offset_x, chroma_offset_y);
    const float2 uv = read_imagef(uv_image, sampler_linear, uv_coord).xy;

    const float Cb = (uv.x - (128.0f / 255.0f)) * (full_range ? 1.0f : (255.0f / 224.0f));
    const float Cr = (uv.y - (128.0f / 255.0f)) * (full_range ? 1.0f : (255.0f / 224.0f));
    const float kg = 1.0f - kr - kb;
    const float r = Y + (2.0f * (1.0f - kr)) * Cr;
    const float b = Y + (2.0f * (1.0f - kb)) * Cb;
    const float g = Y - (2.0f * kb * (1.0f - kb) / kg) * Cb
                      - (2.0f * kr * (1.0f - kr) / kg) * Cr;
    const uchar R = convert_uchar_sat_rte(r * 255.0f);
    const uchar G = convert_uchar_sat_rte(g * 255.0f);
    const uchar B = convert_uchar_sat_rte(b * 255.0f);
    dst[idx]     = bgr ? B : R;
    dst[idx + 1] = G;
    dst[idx + 2] = bgr ? R : B;
}
