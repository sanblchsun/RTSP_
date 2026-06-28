#include "desktop_streamer.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

DesktopStreamer::DesktopStreamer(const std::string& rtsp_server_url)
    : input_ctx_(nullptr)
    , output_ctx_(nullptr)
    , decoder_ctx_(nullptr)
    , encoder_ctx_(nullptr)
    , sws_ctx_(nullptr)
    , rtsp_url_(rtsp_server_url)
    , status_(StreamerStatus::UNINITIALIZED)
    , video_stream_index_(-1)
    , should_stop_(false)
    , frames_processed_(0)
{
    initialize_ffmpeg();
}

DesktopStreamer::~DesktopStreamer() {
    stop();
    cleanup();
}

DesktopStreamer::DesktopStreamer(DesktopStreamer&& other) noexcept {
    move_from(std::move(other));
}

DesktopStreamer& DesktopStreamer::operator=(DesktopStreamer&& other) noexcept {
    if (this != &other) {
        stop();
        cleanup();
        move_from(std::move(other));
    }
    return *this;
}

void DesktopStreamer::initialize_ffmpeg() {
    static std::once_flag initialized;
    std::call_once(initialized, []() {
        av_log_set_level(AV_LOG_INFO);
        avdevice_register_all();
    });
}

std::string DesktopStreamer::status_to_string(StreamerStatus status) {
    switch (status) {
    case StreamerStatus::UNINITIALIZED: return "Не инициализирован";
    case StreamerStatus::INITIALIZED: return "Инициализирован";
    case StreamerStatus::STREAMING: return "Транслирует";
    case StreamerStatus::STOPPED: return "Остановлен";
    case StreamerStatus::ERROR_STATE: return "Ошибка";
    default: return "Неизвестный статус";
    }
}

bool DesktopStreamer::setup_input() {
    const AVInputFormat* input_fmt = av_find_input_format("gdigrab");
    if (!input_fmt) {
        handle_error("GDIGrab input format not found");
        return false;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "framerate", std::to_string(capture_settings_.framerate).c_str(), 0);
    av_dict_set(&options, "draw_mouse", "1", 0);

    if (!capture_settings_.device_name.empty()) {
        av_dict_set(&options, "title", capture_settings_.device_name.c_str(), 0);
    }

    int ret = avformat_open_input(&input_ctx_, "desktop", input_fmt, &options);
    if (ret < 0) {
        handle_error("Failed to open input", ret);
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);

    ret = avformat_find_stream_info(input_ctx_, nullptr);
    if (ret < 0) {
        handle_error("Failed to find stream info", ret);
        return false;
    }

    for (unsigned int i = 0; i < input_ctx_->nb_streams; ++i) {
        if (input_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ < 0) {
        handle_error("No video stream found in input");
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(
        input_ctx_->streams[video_stream_index_]->codecpar->codec_id);
    if (!decoder) {
        handle_error("Decoder not found");
        return false;
    }

    decoder_ctx_ = avcodec_alloc_context3(decoder);
    if (!decoder_ctx_) {
        handle_error("Failed to allocate decoder context");
        return false;
    }

    ret = avcodec_parameters_to_context(decoder_ctx_,
        input_ctx_->streams[video_stream_index_]->codecpar);
    if (ret < 0) {
        handle_error("Failed to copy decoder parameters", ret);
        return false;
    }

    ret = avcodec_open2(decoder_ctx_, decoder, nullptr);
    if (ret < 0) {
        handle_error("Failed to open decoder", ret);
        return false;
    }

    capture_settings_.width = decoder_ctx_->width;
    capture_settings_.height = decoder_ctx_->height;

    return true;
}

bool DesktopStreamer::setup_output() {
    avformat_alloc_output_context2(&output_ctx_, nullptr, "mpegts", rtsp_url_.c_str());
    if (!output_ctx_) {
        handle_error("Failed to create output context");
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        handle_error("H.264 encoder not found");
        return false;
    }

    AVStream* out_stream = avformat_new_stream(output_ctx_, encoder);
    if (!out_stream) {
        handle_error("Failed to create output stream");
        return false;
    }

    encoder_ctx_ = avcodec_alloc_context3(encoder);
    if (!encoder_ctx_) {
        handle_error("Failed to allocate encoder context");
        return false;
    }

    encoder_ctx_->width = capture_settings_.width;
    encoder_ctx_->height = capture_settings_.height;
    encoder_ctx_->time_base = AVRational{ 1, capture_settings_.framerate };
    encoder_ctx_->framerate = AVRational{ capture_settings_.framerate, 1 };
    encoder_ctx_->pix_fmt = encoding_settings_.pixel_format;
    encoder_ctx_->bit_rate = encoding_settings_.bitrate;
    encoder_ctx_->gop_size = encoding_settings_.gop_size;
    encoder_ctx_->max_b_frames = encoding_settings_.max_b_frames;
    encoder_ctx_->thread_count = 0;

    AVDictionary* codec_options = nullptr;
    if (!encoding_settings_.preset.empty()) {
        av_dict_set(&codec_options, "preset", encoding_settings_.preset.c_str(), 0);
    }
    if (!encoding_settings_.tune.empty()) {
        av_dict_set(&codec_options, "tune", encoding_settings_.tune.c_str(), 0);
    }
    if (!encoding_settings_.profile.empty()) {
        av_dict_set(&codec_options, "profile", encoding_settings_.profile.c_str(), 0);
    }
    av_dict_set(&codec_options, "x264-params", "repeat_headers=1:annexb=1", 0);

    int ret = avcodec_open2(encoder_ctx_, encoder, &codec_options);
    av_dict_free(&codec_options);
    if (ret < 0) {
        handle_error("Failed to open encoder", ret);
        return false;
    }

    ret = avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx_);
    if (ret < 0) {
        handle_error("Failed to copy encoder parameters", ret);
        return false;
    }

    out_stream->time_base = encoder_ctx_->time_base;

    if (rtsp_url_.find("listen=1") != std::string::npos) {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "listen", "1", 0);
        av_dict_set(&opts, "listen_timeout", "1000", 0);
        do {
            ret = avio_open2(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
            if (ret >= 0) break;
            if (output_ctx_->pb) avio_closep(&output_ctx_->pb);
        } while (!should_stop_.load());
        av_dict_free(&opts);
    } else if (rtsp_url_.find("tcp://") == 0) {
        do {
            ret = avio_open(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE);
            if (ret >= 0) break;
            if (output_ctx_->pb) avio_closep(&output_ctx_->pb);
        } while (!should_stop_.load());
    } else {
        ret = avio_open(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE);
    }
    if (ret < 0) {
        handle_error("Failed to open output", ret);
        return false;
    }

    av_opt_set(output_ctx_->priv_data, "mpegts_flags", "+resend_headers", 0);

    ret = avformat_write_header(output_ctx_, nullptr);
    if (ret < 0) {
        handle_error("Failed to write header", ret);
        return false;
    }

    return true;
}

bool DesktopStreamer::initialize() {
    if (status_ != StreamerStatus::UNINITIALIZED) {
        cleanup();
    }

    status_ = StreamerStatus::UNINITIALIZED;

    if (!setup_input()) {
        return false;
    }

    if (!setup_output()) {
        cleanup();
        return false;
    }

    status_ = StreamerStatus::INITIALIZED;
    return true;
}

void DesktopStreamer::streaming_loop() {
    AVPacket* input_packet = av_packet_alloc();
    AVPacket* encoded_packet = av_packet_alloc();
    AVFrame* input_frame = av_frame_alloc();

    if (!input_packet || !encoded_packet || !input_frame) {
        handle_error("Failed to allocate packets/frames");
        goto cleanup;
    }

    start_time_ = std::chrono::high_resolution_clock::now();
    last_stats_time_ = start_time_;

    while (!should_stop_.load()) {
        int ret = av_read_frame(input_ctx_, input_packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                break;
            }
            if (!should_stop_.load()) {
                handle_error("Failed to read frame", ret);
            }
            break;
        }

        if (input_packet->stream_index != video_stream_index_) {
            av_packet_unref(input_packet);
            continue;
        }

        ret = avcodec_send_packet(decoder_ctx_, input_packet);
        av_packet_unref(input_packet);
        if (ret < 0) {
            if (!should_stop_.load()) {
                handle_error("Failed to send packet to decoder", ret);
            }
            break;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(decoder_ctx_, input_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                if (!should_stop_.load()) {
                    handle_error("Failed to receive frame from decoder", ret);
                }
                goto cleanup;
            }

            sws_ctx_ = sws_getCachedContext(sws_ctx_,
                input_frame->width, input_frame->height, static_cast<AVPixelFormat>(input_frame->format),
                encoder_ctx_->width, encoder_ctx_->height, encoder_ctx_->pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr);

            if (!sws_ctx_) {
                handle_error("Failed to create scaling context");
                goto cleanup;
            }

            AVFrame* converted_frame = av_frame_alloc();
            converted_frame->format = encoder_ctx_->pix_fmt;
            converted_frame->width = encoder_ctx_->width;
            converted_frame->height = encoder_ctx_->height;
            av_frame_get_buffer(converted_frame, 0);

            sws_scale(sws_ctx_,
                input_frame->data, input_frame->linesize, 0, input_frame->height,
                converted_frame->data, converted_frame->linesize);

            converted_frame->pts = frames_processed_++;

            ret = avcodec_send_frame(encoder_ctx_, converted_frame);
            av_frame_free(&converted_frame);
            if (ret < 0) {
                if (!should_stop_.load()) {
                    handle_error("Failed to send frame to encoder", ret);
                }
                goto cleanup;
            }

            while (ret >= 0) {
                ret = avcodec_receive_packet(encoder_ctx_, encoded_packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    if (!should_stop_.load()) {
                        handle_error("Failed to receive packet from encoder", ret);
                    }
                    goto cleanup;
                }

                av_packet_rescale_ts(encoded_packet,
                    encoder_ctx_->time_base,
                    output_ctx_->streams[0]->time_base);
                encoded_packet->stream_index = 0;

                ret = av_interleaved_write_frame(output_ctx_, encoded_packet);
                av_packet_unref(encoded_packet);
                if (ret < 0) {
                    if (!should_stop_.load()) {
                        handle_error("Client disconnected", ret);
                    }
                    goto cleanup;
                }

                update_statistics();
            }
        }
        av_frame_unref(input_frame);
    }

cleanup:
    if (output_ctx_) {
        av_write_trailer(output_ctx_);
    }
    av_packet_free(&input_packet);
    av_packet_free(&encoded_packet);
    av_frame_free(&input_frame);

    if (status_ == StreamerStatus::STREAMING) {
        status_ = StreamerStatus::STOPPED;
    }
}

void DesktopStreamer::update_statistics() {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_stats_time_).count();

    if (elapsed >= 1000 && stats_callback_) {
        auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time_).count();
        double fps = static_cast<double>(frames_processed_) / (total_elapsed / 1000.0);
        double bitrate = (encoder_ctx_ && encoder_ctx_->bit_rate > 0)
            ? static_cast<double>(encoder_ctx_->bit_rate) : 0.0;
        stats_callback_(frames_processed_, fps, bitrate);
        last_stats_time_ = now;
    }
}

bool DesktopStreamer::stream() {
    if (status_ != StreamerStatus::INITIALIZED && status_ != StreamerStatus::STOPPED) {
        handle_error("Streamer is not initialized");
        return false;
    }

    bool listen_mode = rtsp_url_.find("listen=1") != std::string::npos;

    do {
        status_ = StreamerStatus::STREAMING;
        streaming_loop();
        if (should_stop_.load()) break;
        if (!listen_mode) break;
        status_ = StreamerStatus::INITIALIZED;
    } while (!should_stop_.load() && reconnect_output());

    return true;
}

bool DesktopStreamer::start_async() {
    if (status_ != StreamerStatus::INITIALIZED) {
        handle_error("Streamer is not initialized");
        return false;
    }

    status_ = StreamerStatus::STREAMING;
    streaming_thread_ = std::thread([this]() {
        streaming_loop();
    });
    return true;
}

void DesktopStreamer::stop() {
    should_stop_.store(true);
    if (streaming_thread_.joinable()) {
        streaming_thread_.join();
    }
}

void DesktopStreamer::wait_for_completion() {
    if (streaming_thread_.joinable()) {
        streaming_thread_.join();
    }
}

DesktopStreamer::StreamerStatus DesktopStreamer::get_status() const {
    return status_;
}

void DesktopStreamer::set_capture_settings(const CaptureSettings& settings) {
    capture_settings_ = settings;
}

void DesktopStreamer::set_encoding_settings(const EncodingSettings& settings) {
    encoding_settings_ = settings;
}

const DesktopStreamer::CaptureSettings& DesktopStreamer::get_capture_settings() const {
    return capture_settings_;
}

const DesktopStreamer::EncodingSettings& DesktopStreamer::get_encoding_settings() const {
    return encoding_settings_;
}

void DesktopStreamer::set_statistics_callback(const StatisticsCallback& callback) {
    stats_callback_ = callback;
}

void DesktopStreamer::set_error_callback(const ErrorCallback& callback) {
    error_callback_ = callback;
}

void DesktopStreamer::get_statistics(
    int64_t& frames_processed, double& fps, double& duration_seconds) const
{
    frames_processed = frames_processed_;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time_).count();
    duration_seconds = elapsed / 1000.0;
    if (duration_seconds > 0) {
        fps = static_cast<double>(frames_processed_) / duration_seconds;
    } else {
        fps = 0.0;
    }
}

std::vector<std::string> DesktopStreamer::get_available_capture_devices() {
    return {};
}

std::string DesktopStreamer::get_system_info() {
    std::ostringstream info;
    info << "FFmpeg версия: " << av_version_info();
    info << "\nКонфигурация: " << avcodec_configuration();
    info << "\nУстройства захвата: gdigrab";
    info << "\nПоддержка RTSP: " << (is_rtsp_supported() ? "да" : "нет");
    return info.str();
}

bool DesktopStreamer::is_rtsp_supported() {
    const AVOutputFormat* fmt = av_guess_format("rtsp", nullptr, nullptr);
    return fmt != nullptr;
}

void DesktopStreamer::handle_error(const std::string& message, int ffmpeg_error) {
    status_ = StreamerStatus::ERROR_STATE;
    if (error_callback_) {
        error_callback_(message, ffmpeg_error);
    } else {
        std::cerr << "[DesktopStreamer] " << message;
        if (ffmpeg_error != 0) {
            std::cerr << " (код: " << ffmpeg_error << ")";
        }
        std::cerr << std::endl;
    }
}

bool DesktopStreamer::reconnect_output() {
    if (output_ctx_) {
        if (!(output_ctx_->oformat->flags & AVFMT_NOFILE) && output_ctx_->pb) {
            avio_closep(&output_ctx_->pb);
        }
        avformat_free_context(output_ctx_);
        output_ctx_ = nullptr;
    }

    avformat_alloc_output_context2(&output_ctx_, nullptr, "mpegts", rtsp_url_.c_str());
    if (!output_ctx_) {
        handle_error("Failed to create output context for reconnection");
        return false;
    }

    AVStream* out_stream = avformat_new_stream(output_ctx_, nullptr);
    if (!out_stream) {
        handle_error("Failed to create stream for reconnection");
        return false;
    }

    if (encoder_ctx_) {
        int ret = avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx_);
        if (ret < 0) {
            handle_error("Failed to copy encoder params for reconnection", ret);
            return false;
        }
        out_stream->time_base = encoder_ctx_->time_base;
    }

    int ret;
    if (rtsp_url_.find("listen=1") != std::string::npos) {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "listen", "1", 0);
        av_dict_set(&opts, "listen_timeout", "1000", 0);
        do {
            ret = avio_open2(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
            if (ret >= 0) break;
            if (output_ctx_->pb) avio_closep(&output_ctx_->pb);
        } while (!should_stop_.load());
        av_dict_free(&opts);
    } else if (rtsp_url_.find("tcp://") == 0) {
        do {
            ret = avio_open(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE);
            if (ret >= 0) break;
            if (output_ctx_->pb) avio_closep(&output_ctx_->pb);
        } while (!should_stop_.load());
    } else {
        ret = avio_open(&output_ctx_->pb, rtsp_url_.c_str(), AVIO_FLAG_WRITE);
    }
    if (ret < 0) {
        return false;
    }

    ret = avformat_write_header(output_ctx_, nullptr);
    if (ret < 0) {
        handle_error("Failed to write header for reconnection", ret);
        return false;
    }

    return true;
}

void DesktopStreamer::cleanup() {
    if (output_ctx_) {
        if (!(output_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&output_ctx_->pb);
        }
        avformat_free_context(output_ctx_);
    }
    avformat_close_input(&input_ctx_);
    avcodec_free_context(&decoder_ctx_);
    avcodec_free_context(&encoder_ctx_);
    sws_freeContext(sws_ctx_);

    input_ctx_ = nullptr;
    output_ctx_ = nullptr;
    decoder_ctx_ = nullptr;
    encoder_ctx_ = nullptr;
    sws_ctx_ = nullptr;
    video_stream_index_ = -1;
    frames_processed_ = 0;
}

void DesktopStreamer::move_from(DesktopStreamer&& other) noexcept {
    input_ctx_ = other.input_ctx_;
    output_ctx_ = other.output_ctx_;
    decoder_ctx_ = other.decoder_ctx_;
    encoder_ctx_ = other.encoder_ctx_;
    sws_ctx_ = other.sws_ctx_;
    rtsp_url_ = std::move(other.rtsp_url_);
    capture_settings_ = std::move(other.capture_settings_);
    encoding_settings_ = std::move(other.encoding_settings_);
    status_ = other.status_;
    video_stream_index_ = other.video_stream_index_;
    frames_processed_ = other.frames_processed_;
    start_time_ = other.start_time_;
    last_stats_time_ = other.last_stats_time_;
    should_stop_.store(other.should_stop_.load());

    other.input_ctx_ = nullptr;
    other.output_ctx_ = nullptr;
    other.decoder_ctx_ = nullptr;
    other.encoder_ctx_ = nullptr;
    other.sws_ctx_ = nullptr;
    other.status_ = StreamerStatus::UNINITIALIZED;
    other.video_stream_index_ = -1;
}

std::string FFmpegError::to_string(int error_code) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_make_error_string(err_buf, AV_ERROR_MAX_STRING_SIZE, error_code);
    return std::string(err_buf);
}

bool FFmpegError::is_eof(int error_code) {
    return error_code == AVERROR_EOF;
}

bool RTSPUtils::validate_url(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 && url.length() > 7;
}

std::string RTSPUtils::extract_host(const std::string& url) {
    if (!validate_url(url)) return {};

    std::string stripped = url.substr(7);
    size_t colon_pos = stripped.find(':');
    size_t slash_pos = stripped.find('/');
    size_t end_pos = std::min(
        colon_pos != std::string::npos ? colon_pos : std::string::npos,
        slash_pos != std::string::npos ? slash_pos : std::string::npos);

    if (end_pos != std::string::npos) {
        return stripped.substr(0, end_pos);
    }
    return stripped;
}

int RTSPUtils::extract_port(const std::string& url) {
    if (!validate_url(url)) return -1;

    std::string stripped = url.substr(7);
    size_t colon_pos = stripped.find(':');
    if (colon_pos == std::string::npos) return 554;

    size_t slash_pos = stripped.find('/', colon_pos);
    std::string port_str = stripped.substr(colon_pos + 1,
        slash_pos != std::string::npos ? slash_pos - colon_pos - 1 : std::string::npos);
    return std::stoi(port_str);
}

std::string RTSPUtils::extract_path(const std::string& url) {
    if (!validate_url(url)) return {};

    size_t slash_pos = url.find('/', 7);
    if (slash_pos == std::string::npos) return "/";
    return url.substr(slash_pos);
}
