#include "capture_wgc.h"

WGCAdapter::WGCAdapter() {}
WGCAdapter::~WGCAdapter() { Shutdown(); }

bool WGCAdapter::Initialize(int fps)
{
    return m_wgc.Initialize(fps);
}

bool WGCAdapter::CaptureFrame(int monitor_id,
    std::vector<uint8_t> &out_bgra, int &out_w, int &out_h)
{
    return m_wgc.CaptureFrame(monitor_id, out_bgra, out_w, out_h);
}

void WGCAdapter::Shutdown()
{
    m_wgc.Shutdown();
}

int WGCAdapter::GetMonitorCount() const
{
    return m_wgc.GetMonitorCount();
}

bool WGCAdapter::GetMonitorInfo(int monitor_id,
    int &out_w, int &out_h, int &out_x, int &out_y) const
{
    return m_wgc.GetMonitorInfo(monitor_id, out_w, out_h, out_x, out_y);
}
