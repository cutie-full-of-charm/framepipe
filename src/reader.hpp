// src/reader.hpp
#pragma once
#include "_base.hpp"
#include "frame.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#if defined(_WIN32) || defined(_WIN64)
using uint = std::uint32_t;
#endif

namespace spdlog {
class logger;
}

class Reader {
  public:
    Reader(const std::string &source, const std::string &name, bool verbose, int videostream_id,
           double reconnect_delay_sec, double degraded_timeout_sec,
           std::unordered_map<std::string, std::string> av_options, OutputFormat out_format, HWAccel hwaccel,
           bool realtime_file, uint out_w, uint out_h, double scale, PostProcessor postprocessor,
           MemoryType output_memory, bool on_demand = false);

    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    Reader(Reader &&) = delete;
    Reader &operator=(Reader &&) = delete;

    std::shared_ptr<Frame> read();
    std::shared_ptr<Frame> read_new();
    // Request exactly one successfully processed frame (FILE mode only).
    // Returns nullptr on EOF/stop. Requires start_thread().
    std::shared_ptr<Frame> next();
    std::optional<std::shared_ptr<Frame>> read_latest(double timeout_sec);

    void grab();
    std::pair<bool, std::shared_ptr<Frame>> retrieve();
    bool is_opened() const;
    bool is_alive() const;
    void stop();
    void start_thread();

    double last_pts_seconds() const;
    double last_recv_time() const { return _frame_ts.load(); }

    uint64_t frames_processed() const { return _frames_processed.load(); }
    uint64_t packets_processed() const { return _packets_processed.load(); }
    State state() const { return _state.load(); }
    std::string frame_pix_fmt() const;

    std::string source;
    std::string name;
    bool _is_live = false;
    OutputFormat out_format;
    HWAccel hwaccel_requested;
    std::string hwaccel_active = "SOFTWARE";

    std::function<void(const std::string &, const std::string &, const std::string &)> _on_state_change;

  private:
    void _loop();
    void _run_stream();
    bool _await_frame_request();
    void _set_state(State new_state);
    // void _process_frame(AVFrame *src, SwsContext *&sws_ctx);
    static bool _detect_live_source(const std::string &source);
    static AVPixelFormat _get_hw_format(AVCodecContext *ctx, const AVPixelFormat *fmts);

    std::shared_ptr<Frame> _frame;
    std::shared_ptr<Frame> _grabbed_frame;
    std::condition_variable _cv;
    std::condition_variable _request_cv;
    std::mutex _next_lock;            // serialize concurrent next() calls
    bool _request_pending = false;    // protected by _lock
    bool _on_demand_finished = false; // protected by _lock
    std::string _on_demand_error;     // protected by _lock
    uint64_t _frame_id = 0;
    uint64_t _last_read_id = 0;
    mutable std::mutex _lock;

    std::atomic<bool> _stop_flag{false};
    std::thread _thread;

    bool _verbose;
    int _videostream_id;
    double _reconnect_delay_sec;
    double _degraded_timeout_sec;
    std::atomic<State> _state{State::INIT};
    std::unordered_map<std::string, std::string> _av_options;

    AVPixelFormat _out_pix_fmt = AV_PIX_FMT_BGR24; // NONE при NATIVE
    int _out_channels = 3;
    std::atomic<int> _last_buf_pix_fmt{AV_PIX_FMT_NONE};

    AVBufferRef *_hw_device_ctx = nullptr;
    AVPixelFormat _hw_pix_fmt = AV_PIX_FMT_NONE;

    std::atomic<double> _frame_ts{0.0};
    double _last_state_change = 0.0;
    double _time_base = 0.0;
    std::atomic<uint64_t> _frames_processed{0};
    std::atomic<uint64_t> _packets_processed{0};

    std::shared_ptr<spdlog::logger> _log;

    bool _realtime_file = false;
    bool _on_demand = false;
    int _out_width = 0; // если 0, то берется scale
    int _out_height = 0;
    double _scale = 1.0;
    PostProcessor _postprocessor_requested;
    MemoryType _output_memory_requested;
};
