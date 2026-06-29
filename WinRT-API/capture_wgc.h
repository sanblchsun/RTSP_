#pragma once

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <mutex>
#include <memory>
#include <string>

#ifdef GetMonitorInfo
#undef GetMonitorInfo
#endif

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Foundation.h>

#include "capture_base.h"


struct WGCOutput
{
    int monitor_x = 0, monitor_y = 0;
    int width = 0, height = 0;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    ID3D11Texture2D *staging = nullptr;
};

class WGCCapture : public CaptureBase
{
public:
    WGCCapture();
    ~WGCCapture() override;

    bool Initialize(int fps = 15) override;
    bool CaptureFrame(int monitor_id,
        std::vector<uint8_t> &out_bgra, int &out_w, int &out_h) override;
    void Shutdown() override;

    int GetMonitorCount() const override { return (int)m_outputs.size(); }
    bool GetMonitorInfo(int monitor_id,
        int &out_w, int &out_h, int &out_x, int &out_y) const override;
    static bool IsWGCSupported();

private:
    bool EnumerateOutputs();
    bool InitInterop();
    void CreateD3D11Device();

    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    std::vector<WGCOutput> m_outputs;

    int m_fps = 15;
    bool m_initialized = false;
};
