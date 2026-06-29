#pragma once
#include "encoder.h"
#include <vector>
#include <cstdint>
#include <string>
#include <windows.h>

// Forward declare DX11 types
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class AMFEncoder : public IEncoder
{
public:
    AMFEncoder();
    ~AMFEncoder() override;

    bool Initialize(int width, int height, int fps, int qp) override;
    bool EncodeFrame(const std::vector<uint8_t> &bgra,
                     std::vector<uint8_t> &out_nal) override;
    void Flush(std::vector<uint8_t> &out_nal) override;
    void Shutdown() override;

    int GetWidth() const override { return m_width; }
    int GetHeight() const override { return m_height; }
    bool IsInitialized() const override { return m_initialized; }
    std::string GetName() const override { return "h264_amf"; }

    // Runtime detection
    static bool IsAvailable();

private:
    bool LoadAMF();
    bool InitDX11();
    bool CreateOutputBuffer(std::vector<uint8_t> &out_nal);

    // AMF types are opaque — use void* for the AMF interface pointers
    HMODULE m_amf_dll = nullptr;
    void *m_factory = nullptr;    // AMFFactory*
    void *m_context = nullptr;    // AMFContext*
    void *m_encoder = nullptr;    // AMFComponent*
    bool m_initialized = false;

    // DX11
    ID3D11Device *m_d3d_device = nullptr;
    ID3D11DeviceContext *m_d3d_ctx = nullptr;
    ID3D11Texture2D *m_nv12_tex = nullptr; // reusable NV12 staging texture

    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    int m_qp = 23;
    int m_frame_count = 0;

    // Scratch buffer for BGRA->NV12 conversion
    std::vector<uint8_t> m_nv12_buffer;
};
