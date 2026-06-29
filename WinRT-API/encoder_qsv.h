#pragma once
#include "encoder.h"
#include <vector>
#include <cstdint>
#include <string>
#include <windows.h>

class QSVEncoder : public IEncoder
{
public:
    QSVEncoder();
    ~QSVEncoder() override;

    bool Initialize(int width, int height, int fps, int qp) override;
    bool EncodeFrame(const std::vector<uint8_t> &bgra,
                     std::vector<uint8_t> &out_nal) override;
    void Flush(std::vector<uint8_t> &out_nal) override;
    void Shutdown() override;

    int GetWidth() const override { return m_width; }
    int GetHeight() const override { return m_height; }
    bool IsInitialized() const override { return m_initialized; }
    std::string GetName() const override { return "h264_qsv"; }

    static bool IsAvailable();

private:
    bool AllocSurfaces();
    void FreeSurfaces();
    bool DoEncode(void *surface, std::vector<uint8_t> &out_nal);

    void *m_session = nullptr;    // mfxSession
    void *m_surfaces = nullptr;   // mfxFrameSurface1*
    int m_n_surfaces = 0;
    int m_surface_index = 0;
    uint8_t *m_surface_data = nullptr; // backing memory for all NV12 planes

    std::vector<uint8_t> m_bitstream_data;
    void *m_bitstream = nullptr; // mfxBitstream*

    bool m_initialized = false;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    int m_qp = 23;
    int m_frame_count = 0;

    // Scratch buffer for BGRA->NV12 conversion
    std::vector<uint8_t> m_nv12_buffer;
};
