#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <vector>
#include <cstdint>

#include "WinRT-API/encoder.h"
#include "WinRT-API/capture_base.h"

class DesktopStreamer
{
public:
    struct CaptureSettings
    {
        int monitor_id = 0;
        int framerate = 30;
    };

    struct EncodingSettings
    {
        int quality = 23;
    };

    using LogCallback = std::function<void(const std::string &msg)>;

    DesktopStreamer();
    ~DesktopStreamer();

    DesktopStreamer(const DesktopStreamer &) = delete;
    DesktopStreamer &operator=(const DesktopStreamer &) = delete;

    bool Initialize();
    void Shutdown();

    void SetCaptureSettings(const CaptureSettings &s);
    void SetEncodingSettings(const EncodingSettings &s);
    void SetLogCallback(LogCallback cb);

    int GetMonitorCount() const;
    bool GetMonitorInfo(int monitor_id, int &w, int &h, int &x, int &y) const;

    // main loop: capture → encode → callback
    using FrameCallback = std::function<void(const std::vector<uint8_t> &nal_data, int64_t pts)>;
    void Run(FrameCallback on_frame);

private:
    std::unique_ptr<CaptureBase> capture_;
    std::unique_ptr<IEncoder> encoder_;

    CaptureSettings cap_;
    EncodingSettings enc_;
    LogCallback log_;

    std::atomic<bool> running_{false};

    void Log(const std::string &msg);
};
