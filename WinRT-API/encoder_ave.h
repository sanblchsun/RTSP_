#pragma once
#include "encoder.h"
#include <vector>
#include <cstdint>
#include <string>
#include <windows.h>

class AVEEncoder : public IEncoder
{
public:
    AVEEncoder();
    ~AVEEncoder() override;

    bool Initialize(int width, int height, int fps, int qp) override;
    bool EncodeFrame(const std::vector<uint8_t> &bgra,
                     std::vector<uint8_t> &out_nal) override;
    void Flush(std::vector<uint8_t> &out_nal) override;
    void Shutdown() override;

    int GetWidth() const override { return m_width; }
    int GetHeight() const override { return m_height; }
    bool IsInitialized() const override { return m_initialized; }
    std::string GetName() const override { return "h264_ave"; }

    static bool IsAvailable();

private:
    bool LoadAVE();

    HMODULE m_ave_dll = nullptr;
    void *m_encoder = nullptr;
    bool m_initialized = false;

    // AVE API function pointers
    FARPROC m_fn_encode = nullptr;
    FARPROC m_fn_flush = nullptr;
    FARPROC m_fn_destroy = nullptr;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    int m_qp = 23;
    int m_frame_count = 0;

    // NV12 buffer for BGRA->NV12 conversion
    std::vector<uint8_t> m_nv12_buffer;
};
