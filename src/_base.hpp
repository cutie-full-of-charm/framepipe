// src/_base.hpp
#pragma once
#include <chrono>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/pixfmt.h>
}

enum class State : uint8_t { INIT, CONNECTING, STREAMING, DEGRADED, STOPPED };

// NOTE: native format can be any format provided via ffmpeg -pxl_fmts,
//       but typically is some of { nv12, yuv420p, yuvj420p, P010, etc... },
//       youll see your actual via spdlog then reader initiating
enum class OutputFormat : uint8_t { BGR24, RGB24, GRAY8, NATIVE };

// target HWAccels { SOFTWARE, CUDA, QSV, VAAPI, D3D11VA, DXVA2, VIDEOTOOLBOX };
// current HWAccels
enum class HWAccel : uint8_t { SOFTWARE, CUDA, VAAPI, D3D11VA };
enum class PostProcessor : uint8_t {
    CPU,
    CUDA,
    VAAPI,
    OPENCL,
    /* VULKAN, D3D11 */
};
enum class MemoryType : uint8_t { CPU, GPU };

inline std::string state_to_str(State s) {
    switch (s) {
    case State::INIT:
        return "INIT";
    case State::CONNECTING:
        return "CONNECTING";
    case State::STREAMING:
        return "STREAMING";
    case State::DEGRADED:
        return "DEGRADED";
    case State::STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

inline std::string output_format_to_str(OutputFormat f) {
    switch (f) {
    case OutputFormat::BGR24:
        return "BGR24";
    case OutputFormat::RGB24:
        return "RGB24";
    case OutputFormat::GRAY8:
        return "GRAY8";
    case OutputFormat::NATIVE:
        return "NATIVE";
    default:
        return "UNKNOWN";
    }
}

inline int output_format_channels(OutputFormat f) {
    switch (f) {
    case OutputFormat::GRAY8:
        return 1;
    case OutputFormat::NATIVE:
        return 0;
    default:
        return 3;
    }
}

inline std::string hwaccel_to_str(HWAccel h) {
    switch (h) {
    case HWAccel::SOFTWARE:
        return "SOFTWARE";
    case HWAccel::CUDA:
        return "CUDA";
    case HWAccel::VAAPI:
        return "VAAPI";
    // case HWAccel::QSV:          return "QSV";
    case HWAccel::D3D11VA:
        return "D3D11VA";
    // case HWAccel::DXVA2:        return "DXVA2";
    // case HWAccel::VIDEOTOOLBOX: return "VIDEOTOOLBOX";
    default:
        return "UNKNOWN";
    }
}

inline double monotonic_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
