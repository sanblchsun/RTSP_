#pragma once
#include "encoder.h"
#include <vector>
#include <cstdint>
#include <x264.h>

class X264Encoder : public IEncoder
{
public:
    X264Encoder();
    ~X264Encoder() override;

    bool Initialize(int width, int height, int fps, int qp) override;
    bool EncodeFrame(const std::vector<uint8_t> &bgra,
                     std::vector<uint8_t> &out_nal) override;
    void Flush(std::vector<uint8_t> &out_nal) override;
    void Shutdown() override;

    int GetWidth() const override { return m_width; }
    int GetHeight() const override { return m_height; }
    bool IsInitialized() const override { return m_encoder != nullptr; }
    std::string GetName() const override { return "libx264"; }

private:
    x264_t *m_encoder = nullptr;
    x264_picture_t m_pic_out;
    x264_param_t m_params;

    std::vector<uint8_t> m_yuv_buffer;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 15;
    int m_qp = 23;
    int m_frame_count = 0;
};
