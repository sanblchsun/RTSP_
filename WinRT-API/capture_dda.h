#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <mutex>
#include <thread>
#include <string>

#include "capture_base.h"

struct DDAOutput
{
    int id;
    IDXGIOutputDuplication *dup = nullptr;
    ID3D11Texture2D *staging = nullptr;
    int monitor_x = 0, monitor_y = 0;
    int width = 0, height = 0;
};

class DDACapture : public CaptureBase
{
public:
    DDACapture();
    ~DDACapture() override;

    bool Initialize(int fps = 15) override;
    bool CaptureFrame(int monitor_id,
        std::vector<uint8_t> &out_bgra, int &out_w, int &out_h) override;
    void Shutdown() override;

    int GetMonitorCount() const override { return (int)m_outputs.size(); }
    bool GetMonitorInfo(int monitor_id,
        int &out_w, int &out_h, int &out_x, int &out_y) const override;

private:
    bool EnumerateOutputs();
    bool RecreateDevice();
    void CreateOutputBuffer();

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    IDXGIAdapter *m_adapter = nullptr;
    IDXGIDevice *m_dxgi_device = nullptr;

    std::vector<DDAOutput> m_outputs;
    std::vector<uint8_t> m_buffer;

    int m_fps = 15;
    int m_virtual_w = 0;
    int m_virtual_h = 0;
    bool m_initialized = false;
    int m_virtual_origin_x = 0;
    int m_virtual_origin_y = 0;

};
