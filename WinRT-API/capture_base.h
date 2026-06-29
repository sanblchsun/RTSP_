#pragma once

#ifdef GetMonitorInfo
#undef GetMonitorInfo
#endif

#include <vector>
#include <cstdint>

class CaptureBase
{
public:
    virtual ~CaptureBase() = default;

    virtual bool Initialize(int fps = 15) = 0;
    virtual bool CaptureFrame(int monitor_id,
        std::vector<uint8_t> &out_bgra, int &out_w, int &out_h) = 0;
    virtual void Shutdown() = 0;

    virtual int GetMonitorCount() const = 0;
    virtual bool GetMonitorInfo(int monitor_id,
        int &out_w, int &out_h, int &out_x, int &out_y) const = 0;
};
