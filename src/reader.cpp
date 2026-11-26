/*
 * src/reader.cpp
 * SPDX-License-Identifier: MPL-2.0
 */
#include "reader.hpp"
#include "_base.hpp"
#include "_error.hpp"
#include "cuda_loader.hpp"
#include "frame.hpp"
#include "pipeline.hpp"
#include "post_processors/impls/cpu.hpp"
#include <cmath>
#ifndef _WIN32
#include "post_processors/impls/vaapi.hpp"
#endif
#include <cstdint>
#include <format>
#ifdef FRAMEPIPE_HAS_OPENCL
#include "post_processors/impls/opencl.hpp"
#endif
#include "drivers.cpp"
#include "post_processors/processor.hpp"
#include "post_processors/transfer.hpp"
#include <cerrno>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <nanobind/nanobind.h>
#include <new>
#include <optional>
#include <source_location>
#include <spdlog/common.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#ifndef _WIN32
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#ifndef _WIN32
#include <va/va.h>
#endif
}

// #include <ratio>
// #include <chrono>
// #include <print>
// using Clock = std::chrono::steady_clock;

namespace {

// защита потока декодинга и rtsp-сессии от ошибки при декоде VAAPI поверхности,
class CorruptHardwareFrame final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

#ifndef _WIN32
void check_vaapi_decoded_frame(const AVFrame *frame, spdlog::logger &log) {
    if (frame->format != AV_PIX_FMT_VAAPI || !frame->hw_frames_ctx) {
        return;
    }

    auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
    auto *vaapi_ctx = reinterpret_cast<AVVAAPIDeviceContext *>(frames_ctx->device_ctx->hwctx);
    const auto surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
    const VAStatus status = vaSyncSurface(vaapi_ctx->display, surface);
    if (status == VA_STATUS_SUCCESS) {
        return;
    }

    log.error("VAAPI sync failed: surface=0x{:x}, pts={}, status={} ({})", static_cast<unsigned>(surface), frame->pts,
              vaErrorStr(status), static_cast<int>(status));

    if (status == VA_STATUS_ERROR_DECODING_ERROR) {
        VASurfaceDecodeMBErrors *errors = nullptr;
        const VAStatus query =
            vaQuerySurfaceError(vaapi_ctx->display, surface, status, reinterpret_cast<void **>(&errors));
        if (query == VA_STATUS_SUCCESS && errors) {
            unsigned regions = 0;
            uint64_t reported_mbs = 0;
            for (int i = 0; errors[i].status != -1; ++i) {
                const auto &error = errors[i];
                log.error("VAAPI decode error #{}: type={}, start_mb={}, end_mb={}, num_mb={}, detail_status={}", i,
                          static_cast<int>(error.decode_error_type), error.start_mb, error.end_mb, error.num_mb,
                          error.status);
                reported_mbs += error.num_mb;
                ++regions;
            }
            log.error("VAAPI decoding errors: regions={}, reported_mbs={}", regions, reported_mbs);
        } else if (query != VA_STATUS_SUCCESS) {
            log.warn("vaQuerySurfaceError failed: {}", vaErrorStr(query));
        }
        throw CorruptHardwareFrame(
            std::format("VAAPI decode error at pts={} surface=0x{:x}", frame->pts, static_cast<unsigned>(surface)));
    }

    throw ReaderError(ErrorKind::HardwareFailure,
                      std::format("vaSyncSurface(surface=0x{:x}) failed: {} (status={})",
                                  static_cast<unsigned>(surface), vaErrorStr(status), static_cast<int>(status)));
}
#endif // !_WIN32

AVHWDeviceType to_hw_type(HWAccel h) {
    switch (h) {
    case HWAccel::CUDA:
        return AV_HWDEVICE_TYPE_CUDA;
    case HWAccel::VAAPI:
        return AV_HWDEVICE_TYPE_VAAPI;
        // case HWAccel::QSV:          return AV_HWDEVICE_TYPE_QSV;
#ifdef _WIN32
    case HWAccel::D3D11VA:
        return AV_HWDEVICE_TYPE_D3D11VA;
#else
    case HWAccel::D3D11VA:
        return AV_HWDEVICE_TYPE_NONE;
#endif
    // case HWAccel::DXVA2:        return AV_HWDEVICE_TYPE_DXVA2;
    // case HWAccel::VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    default:
        return AV_HWDEVICE_TYPE_NONE;
    }
}

AVPixelFormat to_av_pixfmt(OutputFormat f) {
    switch (f) {
    case OutputFormat::NATIVE:
        return AV_PIX_FMT_NONE;
    case OutputFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case OutputFormat::GRAY8:
        return AV_PIX_FMT_GRAY8;
    case OutputFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    default:
        throw std::runtime_error("Unsupported output format");
    }
}

AVPixelFormat find_codec_hw_pixfmt(const AVCodec *codec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);

        if (!cfg)
            break;

        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
            return cfg->pix_fmt;
        }
    }

    throw std::runtime_error(std::string("Decoder '") + codec->name + "' does not support HW acceleration '" +
                             av_hwdevice_get_type_name(type) + "'");
}

std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof(buf));
    return std::string(buf);
}

const char *pixname(AVPixelFormat f) {
    const char *n = av_get_pix_fmt_name(f);
    return n ? n : "none";
}

} // namespace

AVPixelFormat Reader::_get_hw_format(AVCodecContext *ctx, const AVPixelFormat *fmts) {
    auto *self = static_cast<Reader *>(ctx->opaque);

    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->_hw_pix_fmt) {
            return *p;
        }
    }
    self->_log->error("HW pixel format '{}' not offered by decoder", pixname(self->_hw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

Reader::Reader(const std::string &source, const std::string &name, bool verbose, int videostream_id,
               double reconnect_delay_sec, double degraded_timeout_sec,
               std::unordered_map<std::string, std::string> av_options, OutputFormat out_format, HWAccel hwaccel,
               bool realtime_file, uint out_w, uint out_h, double scale, PostProcessor postprocessor,
               MemoryType output_memory, bool on_demand)
    : source(source), name(name), out_format(out_format), hwaccel_requested(hwaccel), _verbose(verbose),
      _videostream_id(videostream_id), _reconnect_delay_sec(reconnect_delay_sec),
      _degraded_timeout_sec(degraded_timeout_sec), _realtime_file(realtime_file), _on_demand(on_demand),
      _out_width(out_w), _out_height(out_h), _scale(scale), _postprocessor_requested(postprocessor),
      _output_memory_requested(output_memory) {

    // FIXME: не помню почему здесь этот throw, возможно он более неактуален?
    // GPU surface здесь действительно нельзя никак представить в другом пиксельном формате прям в gpu-mem,
    // кроме как в NATIVE? только через openCL у меня это получилось?
    if (output_memory == MemoryType::GPU && postprocessor == PostProcessor::VAAPI &&
        out_format != OutputFormat::NATIVE) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI output memory requires OutputFormat.NATIVE");
    }

#ifdef _WIN32
    if (hwaccel == HWAccel::VAAPI) {
        throw ReaderError(ErrorKind::UnsupportedConfig,
                          "VAAPI hardware decoding is unavailable on Windows; use D3D11VA");
    }
    if (postprocessor == PostProcessor::VAAPI || postprocessor == PostProcessor::OPENCL) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI/OpenCL postprocessors are unavailable in Windows build");
    }
#endif

    _is_live = _detect_live_source(source);
    if (_on_demand && _is_live) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "on_demand=True is supported only for file sources");
    }
    if (_on_demand && _realtime_file) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "on_demand=True is incompatible with realtime_file=True");
    }

    if (out_format == OutputFormat::NATIVE) {
        _out_pix_fmt = AV_PIX_FMT_NONE;
        _out_channels = 0;
    } else {
        _out_pix_fmt = to_av_pixfmt(out_format);
        _out_channels = output_format_channels(out_format);
    }

    if (_is_live) {
        _av_options = {
            {"rtsp_transport", "tcp"}, {"fflags", "nobuffer"},   {"flags", "low_delay"},
            {"max_delay", "0"},        {"analyzeduration", "0"}, {"probesize", "32"},
        };
    }
    for (const auto &[k, v] : av_options)
        _av_options[k] = v;

    _log = spdlog::get(name);
    if (!_log)
        _log = spdlog::stderr_color_mt(name);

    _log->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%n] [%l]: %$ %v");
    _log->set_level(_verbose ? spdlog::level::debug : spdlog::level::info);

    av_log_set_level(_verbose ? AV_LOG_WARNING : AV_LOG_ERROR);
    // av_log_set_level(AV_LOG_DEBUG);

    _log->info("init '{}', source=\"{}\", type={}, out_format={}, hw={}, av_opts={}", name, source,
               _is_live ? "LIVE" : "FILE", output_format_to_str(out_format), hwaccel_to_str(hwaccel_requested),
               _av_options.size());
    for (const auto &[k, v] : _av_options)
        _log->debug("av_option {} = {}", k, v);
}

Reader::~Reader() { stop(); }

std::shared_ptr<Frame> Reader::read() {
    std::lock_guard<std::mutex> guard(_lock);
    if (!_frame)
        return nullptr;
    return _frame;
}

std::shared_ptr<Frame> Reader::read_new() {
    std::lock_guard<std::mutex> guard(_lock);
    if (!_frame || _frame_id == _last_read_id)
        return nullptr;
    _last_read_id = _frame_id;
    return _frame;
}

// FIXME: on_demand_error теряет код FFmpeg
std::shared_ptr<Frame> Reader::next() {
    if (!_on_demand) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "next() requires on_demand=True");
    }

    std::lock_guard<std::mutex> next_guard(_next_lock);
    std::unique_lock<std::mutex> lock(_lock);

    if (_on_demand_finished || _stop_flag.load()) {
        if (!_on_demand_error.empty()) {
            throw ReaderError(ErrorKind::InternalError, _on_demand_error);
        }
        return nullptr;
    }
    if (!_thread.joinable()) {
        throw ReaderError(ErrorKind::InternalError, "Call start_thread() before next()");
    }

    const uint64_t previous_id = _frame_id;
    _request_pending = true;
    _request_cv.notify_one();

    _cv.wait(lock, [&] { return _frame_id != previous_id || _on_demand_finished || _stop_flag.load(); });

    if (_frame_id != previous_id) {
        return _frame;
    }
    if (!_on_demand_error.empty()) {
        throw ReaderError(ErrorKind::InternalError, _on_demand_error);
    }
    return nullptr; // EOF
}

bool Reader::_await_frame_request() {
    if (!_on_demand) {
        return !_stop_flag.load();
    }
    std::unique_lock<std::mutex> lock(_lock);
    _request_cv.wait(lock, [&] { return _stop_flag.load() || _request_pending; });
    return !_stop_flag.load();
}

std::optional<std::shared_ptr<Frame>> Reader::read_latest(double timeout_sec) {
    if (!std::isfinite(timeout_sec) || timeout_sec < 0.0) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "timeout_sec must be finite and non-negative");
    }

    std::unique_lock<std::mutex> lock(_lock);

    const bool ready = _cv.wait_for(lock, std::chrono::duration<double>(timeout_sec), [this] {
        return (_frame && _frame_id != _last_read_id) || _stop_flag.load() || _on_demand_finished;
    });

    if (!ready) {
        return std::nullopt;
    }

    if (_frame && _frame_id != _last_read_id) {
        _last_read_id = _frame_id;
        return _frame;
    }

    // stop/EOF
    return std::nullopt;
}

void Reader::grab() {
    std::lock_guard<std::mutex> guard(_lock);
    _grabbed_frame = _frame;
}

std::pair<bool, std::shared_ptr<Frame>> Reader::retrieve() {
    std::lock_guard<std::mutex> guard(_lock);
    if (!_grabbed_frame)
        return {false, nullptr};
    return {true, _grabbed_frame};
}

bool Reader::is_opened() const { return !_stop_flag.load() && state() != State::STOPPED; }

bool Reader::is_alive() const {
    return (monotonic_now() - _frame_ts.load()) < _degraded_timeout_sec && !_stop_flag.load();
}

double Reader::last_pts_seconds() const {
    std::lock_guard<std::mutex> guard(_lock);
    if (_frame)
        return _frame->pts * _time_base;
    return 0.0;
}

std::string Reader::frame_pix_fmt() const { return pixname((AVPixelFormat)_last_buf_pix_fmt.load()); }

void Reader::start_thread() {
    if (_thread.joinable()) {
        throw ReaderError(ErrorKind::InternalError, "Reader worker already started");
    }
    _thread = std::thread(&Reader::_loop, this);
}

void Reader::stop() {
    _stop_flag.store(true);
    _cv.notify_all();
    _request_cv.notify_all();
    if (_thread.joinable())
        _thread.join();
    {
        nanobind::gil_scoped_acquire acquire;
        _on_state_change = nullptr;
    }
}

void Reader::_set_state(State new_state) {
    State old = _state.load();
    if (old == new_state)
        return;
    _state.store(new_state);
    _last_state_change = monotonic_now();
    std::string old_str = state_to_str(old);
    std::string new_str = state_to_str(new_state);
    _log->info("state {} -> {}", old_str, new_str);
    if (_on_state_change)
        _on_state_change(name, old_str, new_str);
}

bool Reader::_detect_live_source(const std::string &source) {
    static const std::vector<std::string> prefixes = {"rtsp://", "rtmp://", "rtmps://", "udp://", "srt://", "mms://"};
    for (const auto &p : prefixes)
        if (source.size() >= p.size() && source.starts_with(p))
            return true;
    return false;
}

std::exception_ptr Reader::get_last_exception() const {
    std::lock_guard<std::mutex> lock(_lock);
    return _last_exception;
}

void Reader::_loop() noexcept {
    std::exception_ptr fatal_error;

    try {
        uint64_t attempt = 0;

        while (!_stop_flag.load()) {
            ++attempt;
            bool retry = false;

            try {
                _set_state(State::CONNECTING);
                _log->info("session#{}", attempt);

                _run_stream();

                retry = _is_live && !_on_demand && !_stop_flag.load();

            } catch (const ReaderError &e) {
                if (_stop_flag.load())
                    break;

                retry = _is_live && !_on_demand &&
                        (e.kind() == ErrorKind::ConnectionLost || e.kind() == ErrorKind::SourceUnavailable);

                _log->error("session#{}: kind={}, code={}, error={}", attempt, static_cast<int>(e.kind()),
                            e.ffmpeg_code(), e.what());

                if (!retry) {
                    fatal_error = std::current_exception();

                    if (_on_demand) {
                        std::lock_guard lock(_lock);
                        _on_demand_error = e.what();
                    }
                }

            } catch (const std::exception &e) {
                if (_stop_flag.load())
                    break;

                fatal_error = std::current_exception();
                retry = false;

                _log->error("session#{}: unexpected error: {}", attempt, e.what());

                if (_on_demand) {
                    std::lock_guard lock(_lock);
                    _on_demand_error = e.what();
                }
            }

            if (!retry || _stop_flag.load())
                break;

            _set_state(State::DEGRADED);

            std::unique_lock lock(_lock);
            _cv.wait_for(lock, std::chrono::duration<double>(_reconnect_delay_sec),
                         [this] { return _stop_flag.load(); });
        }

    } catch (...) {
        if (!fatal_error && !_stop_flag.load())
            fatal_error = std::current_exception();

        try {
            _log->error("Unexpected failure in Reader::_loop");
        } catch (...) {
        }
    }
    try {
        _set_state(State::STOPPED);
    } catch (...) {
        _state.store(State::STOPPED);
    }

    {
        std::lock_guard lock(_lock);

        if (fatal_error)
            _last_exception = fatal_error;

        _on_demand_finished = true;
        _request_pending = false;
    }

    _cv.notify_all();
    _request_cv.notify_all();
}

struct FormatDeleter {
    void operator()(AVFormatContext *p) const { avformat_close_input(&p); }
};

struct CodecDeleter {
    void operator()(AVCodecContext *p) const { avcodec_free_context(&p); }
};

struct PacketDeleter {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};

struct DictDeleter {
    void operator()(AVDictionary *p) const { av_dict_free(&p); }
};

struct BufferDeleter {
    void operator()(AVBufferRef *p) const { av_buffer_unref(&p); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using DictPtr = std::unique_ptr<AVDictionary, DictDeleter>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferDeleter>;

auto check = [](int ret, const char *operation, ErrorKind error_kind,
                std::source_location loc = std::source_location::current()) {
    if (ret < 0) {
        throw ReaderError(error_kind,
                          std::format("{}:{} [{}] {} failed (code={}): {}", loc.file_name(), loc.line(),
                                      loc.function_name(), operation, ret, averr(ret)),
                          ret);
    }
};

void Reader::_run_stream() {
    // 1. Открытие источника
    AVDictionary *raw_opts = nullptr;

    for (const auto &[key, value] : _av_options) {
        int ret = av_dict_set(&raw_opts, key.c_str(), value.c_str(), 0);
        if (ret < 0) {
            av_dict_free(&raw_opts);
            check(ret, "av_dict_set", ErrorKind::InternalError);
        }
    }

    DictPtr opts(raw_opts);
    AVFormatContext *raw_fmt = nullptr;

    int ret = avformat_open_input(&raw_fmt, source.c_str(), nullptr, &raw_opts);

    // FFmpeg может изменить указатель словаря
    opts.release();
    opts.reset(raw_opts);

    if (ret < 0) {
        if (raw_fmt) {
            avformat_close_input(&raw_fmt);
        }
        check(ret, "avformat_open_input", ErrorKind::SourceUnavailable);
    }

    FormatPtr fmt_ctx(raw_fmt);

    check(avformat_find_stream_info(fmt_ctx.get(), nullptr), "avformat_find_stream_info", ErrorKind::InternalError);

    // 2. Поиск видеопотока
    int stream_idx = -1;
    int found = 0;

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        const auto *params = fmt_ctx->streams[i]->codecpar;

        if (params->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }

        if (found == _videostream_id) {
            stream_idx = static_cast<int>(i);
            break;
        }
        found++;
    }

    if (stream_idx < 0) {
        throw ReaderError(ErrorKind::InternalError, "No video stream found");
    }

    AVStream *stream = fmt_ctx->streams[stream_idx];
    _time_base = av_q2d(stream->time_base);

    // 3. Инициализация декодера
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        throw ReaderError(ErrorKind::UnsupportedConfig, "Video decoder not found");
    }

    CodecPtr codec_ctx(avcodec_alloc_context3(codec));
    if (!codec_ctx) {
        throw ReaderError(ErrorKind::HardwareFailure, "codec_ctx allocation error");
    }

    check(avcodec_parameters_to_context(codec_ctx.get(), stream->codecpar), "avcodec_parameters_to_context",
          ErrorKind::InternalError);

    const bool hw_enabled = hwaccel_requested != HWAccel::SOFTWARE;

    // Локальный владелец устройства.
    // В конце _run_stream() освободится автоматически.
    BufferPtr hw_device;

    _hw_pix_fmt = AV_PIX_FMT_NONE;
    _hw_device_ctx = nullptr;
    hwaccel_active = "SOFTWARE";

    if (hw_enabled) {
        const AVHWDeviceType type = to_hw_type(hwaccel_requested);

        if (type == AV_HWDEVICE_TYPE_NONE) {
            throw ReaderError(ErrorKind::HardwareFailure, "Unknown hardware acceleration backend");
        }

        _hw_pix_fmt = find_codec_hw_pixfmt(codec, type);

        // для VAAPI при необходимости можно указать render node?
        const char *device = nullptr;

        // FIXME: может ли ffmpeg выбрать самостоятельно? есть ли случаи когда
        // юзер должен указывать конкретный девайс?
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            device = "/dev/dri/renderD128";
        }

        AVBufferRef *raw_device = nullptr;

        configure_vaapi_driver_path();

        check(av_hwdevice_ctx_create(&raw_device, type, device, nullptr, 0), "av_hwdevice_ctx_create",
              ErrorKind::HardwareFailure);

        hw_device.reset(raw_device);

        codec_ctx->hw_device_ctx = av_buffer_ref(hw_device.get());

        if (!codec_ctx->hw_device_ctx) {
            throw ReaderError(ErrorKind::InternalError, "hw_device_ctx allocation error");
        }

        codec_ctx->opaque = this;
        codec_ctx->get_format = &Reader::_get_hw_format;
        codec_ctx->thread_count = 2;

        hwaccel_active = av_hwdevice_get_type_name(type);
    } else {
        // FIXME: добавить управление политикой потоков из python
        codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    check(avcodec_open2(codec_ctx.get(), codec, nullptr), "avcodec_open2", ErrorKind::InternalError);

    _log->info("opened: codec={} {}x{} src_fmt={} tb={:.6g} hw={}", codec->name, codec_ctx->width, codec_ctx->height,
               pixname(codec_ctx->pix_fmt), _time_base, hwaccel_active);

    _log->info("decoder: thread_count={} thread_type={} "
               "active_thread_type={}\n",
               codec_ctx->thread_count, codec_ctx->thread_type, codec_ctx->active_thread_type);

    // 4. Подготовка pipeline
    // if (out_format == OutputFormat::NATIVE) {
    //     throw std::runtime_error("NATIVE output is not supported by
    //     FramePipeline");
    // }

    std::unique_ptr<FramePipeline> pipeline;

    int pipeline_w = 0;
    int pipeline_h = 0;

    auto create_pipeline = [&](const AVFrame *src, int width, int height) -> std::unique_ptr<FramePipeline> {
        const auto hw = hwaccel_requested;
        const auto pp = _postprocessor_requested;
        const auto mem = _output_memory_requested;

        if (hw == HWAccel::D3D11VA && (pp != PostProcessor::CPU || mem != MemoryType::CPU)) {
            throw ReaderError(ErrorKind::UnsupportedConfig,
                              "D3D11VA currently supports only CPU postprocessing and CPU output");
        }

        // 1. Проверка совместимости backend
        switch (pp) {
        case PostProcessor::CUDA:
            if (hw != HWAccel::CUDA) {
                throw ReaderError(ErrorKind::UnsupportedConfig, "CUDA processing requires CUDA decoding");
            }
            break;

        case PostProcessor::VAAPI:
#ifdef _WIN32
            throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI postprocessing is unavailable on Windows");
#else
            if (hw != HWAccel::VAAPI) {
                throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI processing requires VAAPI decoding");
            }
            if (!src || src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx) {
                throw ReaderError(ErrorKind::HardwareFailure, "VAAPI processing requires a hardware AVFrame");
            }
            break;
#endif

        case PostProcessor::CPU:
            if (mem == MemoryType::GPU) {
                throw ReaderError(ErrorKind::HardwareFailure, "CPU postprocessor cannot produce GPU output");
            }
            break;

        case PostProcessor::OPENCL:
#ifdef _WIN32
            throw ReaderError(ErrorKind::UnsupportedConfig, "OpenCL/VAAPI interop is unavailable on Windows");
#else
            if (hw != HWAccel::VAAPI) {
                throw ReaderError(ErrorKind::UnsupportedConfig, "OpenCL processing currently requires VAAPI decoding");
            }
            if (!src || src->format != AV_PIX_FMT_VAAPI || !src->hw_frames_ctx) {
                throw ReaderError(ErrorKind::HardwareFailure, "OpenCL processing requires a VAAPI hardware AVFrame");
            }
            break;
#endif

        default:
            throw ReaderError(ErrorKind::UnsupportedConfig, "Unsupported postprocessor");
        }

        // 2. Проверка формата NATIVE
        if (out_format == OutputFormat::NATIVE) {
            if (pp == PostProcessor::CUDA) {
                throw ReaderError(ErrorKind::UnsupportedConfig, "CUDA processing does not support NATIVE");
            }

            if (pp == PostProcessor::OPENCL && (width != src->width || height != src->height)) {
                // std::print(
                // "src->width={}, width_={}, src->height={}, height_={}",
                // src->width, width, src->height, height);
                throw ReaderError(ErrorKind::UnsupportedConfig, "OpenCL NATIVE NV12 does not support "
                                                                "resize; use source dimensions");
            }

            if (pp == PostProcessor::CPU) {

                if (mem != MemoryType::CPU) {
                    throw ReaderError(ErrorKind::UnsupportedConfig, "CPU NATIVE requires CPU output");
                }
                if (width != codec_ctx->width || height != codec_ctx->height) {
                    throw ReaderError(ErrorKind::UnsupportedConfig, "CPU NATIVE does not support resize");
                }
            }
        }

        // 3. InputTransfer
        std::unique_ptr<IFrameTransfer> input_transfer;

        switch (hw) {
        case HWAccel::CUDA:
        case HWAccel::VAAPI:
        case HWAccel::D3D11VA:
            if (pp == PostProcessor::CPU) {
                input_transfer = std::make_unique<HwToCpuTransfer>();
            } else {
                input_transfer = std::make_unique<RefTransfer>();
            }
            break;

        default:
            input_transfer = std::make_unique<RefTransfer>();
            break;
        }

        // 4. Processor
        std::unique_ptr<IFrameProcessor> processor;

        switch (pp) {
        case PostProcessor::CPU:
            if (out_format == OutputFormat::NATIVE) {
                processor = std::make_unique<NativeCpuProcessor>();
            } else {
                processor = std::make_unique<CpuProcessor>(width, height, out_format);
            }
            break;

        case PostProcessor::CUDA:
            processor = create_cuda_processor(width, height, out_format);
            break;

        case PostProcessor::OPENCL:
#ifdef FRAMEPIPE_HAS_OPENCL
            processor = std::make_unique<OpenCLProcessor>(src->hw_frames_ctx, width, height, out_format, mem, _log);
            _log->info("OpenCL: VAAPI NV12 -> {} {}",
                       out_format == OutputFormat::NATIVE ? "native NV12" : "packed pixels",
                       mem == MemoryType::GPU ? "GPU" : "CPU");
#else
            throw ReaderError(ErrorKind::UnsupportedConfig, "framepipe built without OpenCL support");

#endif
            break;

        case PostProcessor::VAAPI:
#ifdef _WIN32
            throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI postprocessing is unavailable on Windows");
#else
            if (width == src->width && height == src->height) {
                processor = std::make_unique<VaapiPassthroughProcessor>();
                _log->info("VAAPI: passthrough (filter graph bypassed)");
            } else {
                processor = std::make_unique<VaapiProcessor>(src->hw_frames_ctx, src->width, src->height, width, height,
                                                             stream->time_base);
                _log->info("VAAPI: scale_vaapi enabled");
            }
            break;
#endif

        default:
            throw ReaderError(ErrorKind::UnsupportedConfig, "Unsupported postprocessor");
        }

        // 5. OutputTransfer
        std::unique_ptr<IOutputTransfer> output_transfer;

        switch (mem) {
        case MemoryType::CPU:
            if (pp == PostProcessor::CUDA) {
                output_transfer = create_cuda_output_transfer();
                ;
            } else if (pp == PostProcessor::VAAPI) {
#ifndef _WIN32
                output_transfer = std::make_unique<VaapiToCpuTransfer>(out_format);
#else
                throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI postprocessing is unavailable on Windows");
#endif
            } else {
                output_transfer = std::make_unique<NoOpOutputTransfer>();
            }
            break;

        case MemoryType::GPU:
            if (pp == PostProcessor::VAAPI) {
                if (out_format != OutputFormat::NATIVE) {
                    throw ReaderError(ErrorKind::UnsupportedConfig, "VAAPI GPU output requires NATIVE format");
                }
            } else if (pp != PostProcessor::CUDA && pp != PostProcessor::OPENCL) {
                throw ReaderError(ErrorKind::UnsupportedConfig,
                                  "GPU output requires CUDA, VAAPI, or OpenCL processing");
            }
            output_transfer = std::make_unique<NoOpOutputTransfer>();
            break;

        default:
            throw ReaderError(ErrorKind::UnsupportedConfig, "Unsupported output memory type");
        }

        return std::make_unique<FramePipeline>(std::move(input_transfer), std::move(processor),
                                               std::move(output_transfer));
    };

    // 5. Буферы декодирования
    PacketPtr packet(av_packet_alloc());
    FramePtr decoded = make_frame();

    if (!packet) {
        throw ReaderError(ErrorKind::HardwareFailure, "packet allocation error");
    }

    double start_wall = -1.0;
    double start_pts = -1.0;

    // 6. Обработка одного кадра
    auto process_frame = [&](const AVFrame *src) {
        if (hw_enabled && src->format != _hw_pix_fmt) {
            throw ReaderError(ErrorKind::DecodeFailed, "Decoder returned software frame despite HW acceleration");
        }

        if (!hw_enabled && src->hw_frames_ctx) {
            throw ReaderError(ErrorKind::DecodeFailed, "Unexpected hardware frame from software decoder");
        }

        const int src_w = src->width;
        const int src_h = src->height;

        int out_w = src_w;
        int out_h = src_h;

        if (_out_width > 0 && _out_height > 0) {
            out_w = _out_width;
            out_h = _out_height;
        } else if (_scale != 1.0) {
            out_w = static_cast<int>(src_w * _scale);
            out_h = static_cast<int>(src_h * _scale);
        }

        if (out_w <= 0 || out_h <= 0) {
            throw ReaderError(ErrorKind::DecodeFailed, "Invalid output dimensions");
        }

        // Check the decoded surface before any transfer or GPU postprocessing.
        // A decode failure is recoverable at the frame boundary, not in the RTSP loop.
#ifndef _WIN32
        if (src->format == AV_PIX_FMT_VAAPI) {
            check_vaapi_decoded_frame(src, *_log);
        }
#endif

        // создаём pipeline при первом кадре
        // и при изменении выходного размера
        // pipeline = create_pipeline(src, out_w, out_h);
        // pipeline_w = out_w;
        // pipeline_h = out_h;

        if (!pipeline || pipeline_w != out_w || pipeline_h != out_h) {
            pipeline = create_pipeline(src, out_w, out_h);
            pipeline_w = out_w;
            pipeline_h = out_h;
        }

        FramePayload processed = pipeline->process(src);
        auto frame = make_typed_frame(std::move(processed));
        frame->src_width = src_w;
        frame->src_height = src_h;

        if (out_format == OutputFormat::NATIVE) {
            frame->ndim = 0;
            frame->dims[0] = 0;
            frame->dims[1] = 0;
            frame->dims[2] = 0;
        } else {
            frame->ndim = frame->channels == 1 ? 2 : 3;
            frame->dims[0] = frame->height;
            frame->dims[1] = frame->width;
            frame->dims[2] = frame->channels;
        }

        frame->pts = src->pts != AV_NOPTS_VALUE ? src->pts : 0;
        frame->key = (src->flags & AV_FRAME_FLAG_KEY) != 0;
        frame->ts = monotonic_now();

        _last_buf_pix_fmt.store(static_cast<int>(frame->pix_fmt));

        {
            std::lock_guard<std::mutex> lock(_lock);

            frame->id = ++_frame_id;
            _frame = frame;
            if (_on_demand) {
                _request_pending = false;
            }
        }

        _frame_ts.store(frame->ts);
        _cv.notify_all();

        ++_frames_processed;

        if (state() != State::STREAMING && frame->key) {
            _set_state(State::STREAMING);
        }

        if (_verbose) {
            _log->debug("  frame #{} id={} pts={} key={} "
                        "{}x{} (src {}x{}) fmt={}",
                        _frames_processed.load(), frame->id, frame->pts, frame->key, frame->width, frame->height, src_w,
                        src_h, pixname(frame->pix_fmt));
        }
    };

    // 7. Realtime pacing
    // FIXME: replace 1ms polling with an interruptible wait_until().
    auto pace_frame = [&](const AVFrame *frame) {
        if (!_realtime_file || _is_live || frame->pts == AV_NOPTS_VALUE || _time_base <= 0) {
            return;
        }

        const double frame_time = frame->pts * _time_base;
        const double now = monotonic_now();

        if (start_wall < 0) {
            start_wall = now;
            start_pts = frame_time;
        }

        const double target = start_wall + (frame_time - start_pts);
        const double sleep_for = target - now;

        if (sleep_for <= 0) {
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(sleep_for);

        while (!_stop_flag.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    constexpr unsigned max_consecutive_decode_errors = 25;
    unsigned consecutive_decode_errors = 0;
    uint64_t dropped_decode_frames = 0;

    // 8. Получение всех доступных кадров
    auto receive_frames = [&]() -> int {
        while (!_stop_flag.load()) {
            if (!_await_frame_request()) {
                return AVERROR(EAGAIN);
            }
            // const auto t0 = Clock::now();
            int ret = avcodec_receive_frame(codec_ctx.get(), decoded.get());
            // const auto t1 = Clock::now();
            // const double receive_ms = std::chrono::duration<double,
            // std::milli>(t1 - t0).count(); std::print("receive frame={}\n",
            // receive_ms);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return ret;
            }

            check(ret, "avcodec_receive_frame", ErrorKind::DecodeFailed);

            struct UnrefGuard {
                AVFrame *frame;

                ~UnrefGuard() { av_frame_unref(frame); }
            } guard{decoded.get()};

            pace_frame(decoded.get());

            if (_stop_flag.load()) {
                break;
            }

            try {
                process_frame(decoded.get());
                consecutive_decode_errors = 0;
            } catch (const CorruptHardwareFrame &e) {
                ++dropped_decode_frames;
                ++consecutive_decode_errors;
                _log->warn("Skipping damaged VAAPI frame (dropped={}, consecutive={}): {}", dropped_decode_frames,
                           consecutive_decode_errors, e.what());
                if (consecutive_decode_errors >= max_consecutive_decode_errors) {
                    throw ReaderError(ErrorKind::HardwareFailure,
                                      std::format("VAAPI decode did not recover after {} consecutive damaged frames",
                                                  consecutive_decode_errors));
                }
                continue;
            }
        }

        return AVERROR(EAGAIN);
    };

    // 9. Основной цикл
    bool reached_eof = false;

    while (!_stop_flag.load()) {
        if (!_await_frame_request()) {
            break;
        }
        ret = av_read_frame(fmt_ctx.get(), packet.get());

        if (ret == AVERROR_EOF) {
            reached_eof = true;
            break;
        }

        if (ret < 0) {
            check(ret, "av_read_frame", ErrorKind::ConnectionLost);
        }

        if (packet->stream_index != stream_idx) {
            av_packet_unref(packet.get());
            continue;
        }

        ++_packets_processed;

        if (_verbose) {
            _log->debug("pkt #{} pts={} dts={} size={}", _packets_processed.load(), packet->pts, packet->dts,
                        packet->size);
        }

        // Если декодер заполнен, сначала забираем кадры

        // const auto t0 = Clock::now();
        ret = avcodec_send_packet(codec_ctx.get(), packet.get());
        // const auto t1 = Clock::now();
        // const double send_ms = std::chrono::duration<double, std::milli>(t1 -
        // t0).count(); std::print("send_packet={}\n", send_ms);

        if (ret == AVERROR(EAGAIN)) {
            int drain_ret = receive_frames();

            if (_stop_flag.load())
                break;

            if (drain_ret != AVERROR(EAGAIN)) {
                throw ReaderError(ErrorKind::DecodeFailed, "Decoder could not accept pending packet");
            }

            ret = avcodec_send_packet(codec_ctx.get(), packet.get());
        }

        av_packet_unref(packet.get());
        check(ret, "avcodec_send_packet", ErrorKind::DecodeFailed);
        receive_frames();
    }

    // 10. Flush декодера на EOF
    if (reached_eof && !_stop_flag.load()) {
        check(avcodec_send_packet(codec_ctx.get(), nullptr), "avcodec_send_packet(flush)", ErrorKind::InternalError);

        while (!_stop_flag.load()) {
            int result = receive_frames();
            if (result == AVERROR_EOF) {
                break;
            }

            if (result == AVERROR(EAGAIN)) {
                throw ReaderError(ErrorKind::InternalError, "Unexpected EAGAIN during decoder flush");
            }
        }
    }

    // 11. Завершение
    pipeline.reset();

    if (reached_eof && !_is_live) {
        _stop_flag.store(true);
        _set_state(State::STOPPED);
        _cv.notify_all();
    }
}
