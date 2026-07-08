#pragma once

#include "encoder.h"
#include "WinRT-API/encoder_x264.h"

class X264EncoderAdapter : public IEncoder
{
public:
    X264EncoderAdapter();
    ~X264EncoderAdapter() override;

    bool Initialize(int width, int height, int fps, int qp) override;
    bool EncodeFrame(const std::vector<uint8_t> &bgra,
                     std::vector<uint8_t> &out_nal) override;
    void Flush(std::vector<uint8_t> &out_nal) override;
    void Shutdown() override;

    int GetWidth() const override;
    int GetHeight() const override;
    bool IsInitialized() const override;
    std::string GetName() const override;

private:
    X264Encoder m_enc;
};
