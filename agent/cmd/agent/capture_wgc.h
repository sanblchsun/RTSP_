#pragma once

#include "capture_base.h"
#include "WinRT-API/capture_wgc.h"

class WGCAdapter : public CaptureBase
{
public:
    WGCAdapter();
    ~WGCAdapter() override;

    bool Initialize(int fps = 15) override;
    bool CaptureFrame(int monitor_id,
        std::vector<uint8_t> &out_bgra, int &out_w, int &out_h) override;
    void Shutdown() override;

    int GetMonitorCount() const override;
    bool GetMonitorInfo(int monitor_id,
        int &out_w, int &out_h, int &out_x, int &out_y) const override;

private:
    WGCCapture m_wgc;
};
