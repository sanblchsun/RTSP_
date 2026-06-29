#include "desktop_streamer.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdarg>

#include "WinRT-API/capture_wgc.h"
#include "WinRT-API/encoder_x264.h"

void logf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

DesktopStreamer::DesktopStreamer() = default;
DesktopStreamer::~DesktopStreamer() { Shutdown(); }

void DesktopStreamer::Log(const std::string &msg)
{
    if (log_)
        log_(msg);
    else
        logf("%s", msg.c_str());
}

bool DesktopStreamer::Initialize()
{
    capture_ = std::make_unique<WGCCapture>();
    if (!capture_->Initialize(cap_.framerate))
    {
        Log("Failed to initialize WGC capture");
        return false;
    }

    if (capture_->GetMonitorCount() == 0)
    {
        Log("No monitors found");
        return false;
    }

    int w = 0, h = 0, x = 0, y = 0;
    capture_->GetMonitorInfo(cap_.monitor_id, w, h, x, y);

    encoder_ = std::make_unique<X264Encoder>();
    if (!encoder_->Initialize(w, h, cap_.framerate, enc_.quality))
    {
        Log("Failed to initialize x264 encoder");
        capture_->Shutdown();
        capture_.reset();
        return false;
    }

    Log("Initialized: " + std::to_string(w) + "x" + std::to_string(h) +
        " @" + std::to_string(cap_.framerate) + " fps");
    return true;
}

void DesktopStreamer::Shutdown()
{
    running_.store(false);
    if (encoder_)
    {
        encoder_->Shutdown();
        encoder_.reset();
    }
    if (capture_)
    {
        capture_->Shutdown();
        capture_.reset();
    }
}

void DesktopStreamer::SetCaptureSettings(const CaptureSettings &s) { cap_ = s; }
void DesktopStreamer::SetEncodingSettings(const EncodingSettings &s) { enc_ = s; }
void DesktopStreamer::SetLogCallback(LogCallback cb) { log_ = std::move(cb); }

int DesktopStreamer::GetMonitorCount() const
{
    return capture_ ? capture_->GetMonitorCount() : 0;
}

bool DesktopStreamer::GetMonitorInfo(int monitor_id, int &w, int &h, int &x, int &y) const
{
    return capture_ ? capture_->GetMonitorInfo(monitor_id, w, h, x, y) : false;
}

void DesktopStreamer::Run(FrameCallback on_frame)
{
    if (!capture_ || !encoder_)
    {
        Log("Not initialized");
        return;
    }

    running_.store(true);

    std::vector<uint8_t> bgra;
    std::vector<uint8_t> nals;
    int w = 0, h = 0;
    int64_t pts = 0;

    while (running_.load())
    {
        if (!capture_->CaptureFrame(cap_.monitor_id, bgra, w, h))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        nals.clear();
        if (!encoder_->EncodeFrame(bgra, nals))
        {
            Log("Encode error");
            break;
        }

        if (!nals.empty() && on_frame)
            on_frame(nals, pts++);
    }
}
