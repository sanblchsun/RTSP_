#include "encoder_x264.h"

X264EncoderAdapter::X264EncoderAdapter() {}
X264EncoderAdapter::~X264EncoderAdapter() { Shutdown(); }

bool X264EncoderAdapter::Initialize(int width, int height, int fps, int qp)
{
    return m_enc.Initialize(width, height, fps, qp);
}

bool X264EncoderAdapter::EncodeFrame(const std::vector<uint8_t> &bgra,
                                     std::vector<uint8_t> &out_nal)
{
    return m_enc.EncodeFrame(bgra, out_nal);
}

void X264EncoderAdapter::Flush(std::vector<uint8_t> &out_nal)
{
    m_enc.Flush(out_nal);
}

void X264EncoderAdapter::Shutdown()
{
    m_enc.Shutdown();
}

int X264EncoderAdapter::GetWidth() const
{
    return m_enc.GetWidth();
}

int X264EncoderAdapter::GetHeight() const
{
    return m_enc.GetHeight();
}

bool X264EncoderAdapter::IsInitialized() const
{
    return m_enc.IsInitialized();
}

std::string X264EncoderAdapter::GetName() const
{
    return "x264";
}
