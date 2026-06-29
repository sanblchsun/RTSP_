#include "encoder_amf.h"
#include <cstring>
#include <cstdio>
#include <chrono>

extern void logf(const char *fmt, ...);

// AMF headers
#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/core/Surface.h>
#include <AMF/core/Data.h>
#include <AMF/core/Result.h>
#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/Component.h>

#include <d3d11.h>

using namespace amf;

// BGRA -> NV12 (CPU)
static void bgra_to_nv12(const uint8_t *bgra, int w, int h,
                          uint8_t *y_plane, uint8_t *uv_plane)
{
    for (int row = 0; row < h; row++)
    {
        const uint8_t *src = bgra + row * w * 4;
        for (int col = 0; col < w; col++)
        {
            int b = src[col * 4 + 0];
            int g = src[col * 4 + 1];
            int r = src[col * 4 + 2];
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            if (y < 0) y = 0; if (y > 255) y = 255;
            y_plane[row * w + col] = (uint8_t)y;
        }
    }
    for (int row = 0; row < h / 2; row++)
    {
        for (int col = 0; col < w / 2; col++)
        {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; dy++)
            {
                const uint8_t *src = bgra + (row * 2 + dy) * w * 4 + col * 2 * 4;
                r += src[2]; g += src[1]; b += src[0];
                r += src[6]; g += src[5]; b += src[4];
            }
            r >>= 2; g >>= 2; b >>= 2;
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            if (u < 0) u = 0; if (u > 255) u = 255;
            if (v < 0) v = 0; if (v > 255) v = 255;
            uv_plane[(row * (w / 2) + col) * 2 + 0] = (uint8_t)u;
            uv_plane[(row * (w / 2) + col) * 2 + 1] = (uint8_t)v;
        }
    }
}

AMFEncoder::AMFEncoder() {}
AMFEncoder::~AMFEncoder() { Shutdown(); }

bool AMFEncoder::IsAvailable()
{
    HMODULE dll = LoadLibraryA("amfrt64.dll");
    if (!dll) return false;
    FreeLibrary(dll);
    return true;
}

bool AMFEncoder::Initialize(int width, int height, int fps, int qp)
{
    Shutdown();

    if (width <= 0 || height <= 0 || fps <= 0 || qp < 0 || qp > 51 || (width & 1) || (height & 1))
        return false;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_qp = qp;

    // Load AMF runtime
    if (!m_amf_dll)
    {
        m_amf_dll = LoadLibraryA("amfrt64.dll");
        if (!m_amf_dll)
        {
            logf("[AMF] amfrt64.dll not found");
            return false;
        }
    }

    typedef AMF_RESULT (*AMFInit_Fn)(amf_uint64 version, AMFFactory **factory);
    auto AMFInit = (AMFInit_Fn)GetProcAddress(m_amf_dll, "AMFInit");
    if (!AMFInit)
    {
        logf("[AMF] AMFInit not found");
        Shutdown();
        return false;
    }

    AMFFactory *factory = nullptr;
    AMF_RESULT res = AMFInit(AMF_FULL_VERSION, &factory);
    if (res != AMF_OK || !factory)
    {
        logf("[AMF] AMFInit failed: %d", (int)res);
        Shutdown();
        return false;
    }
    m_factory = factory;

    // Create D3D11 device for AMF
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_d3d_device, &fl, &m_d3d_ctx);
    if (FAILED(hr))
    {
        logf("[AMF] D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
        Shutdown();
        return false;
    }

    // Create AMF context and init with DX11
    AMFContextPtr context;
    res = factory->CreateContext(&context);
    if (res != AMF_OK)
    {
        logf("[AMF] CreateContext failed: %d", (int)res);
        Shutdown();
        return false;
    }

    res = context->InitDX11(m_d3d_device);
    if (res != AMF_OK)
    {
        logf("[AMF] InitDX11 failed: %d", (int)res);
        Shutdown();
        return false;
    }
    m_context = context.Detach();

    // Create NV12 staging texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_d3d_device->CreateTexture2D(&desc, nullptr, &m_nv12_tex);
    if (FAILED(hr))
    {
        logf("[AMF] CreateTexture2D NV12 failed: 0x%08X", (unsigned)hr);
        Shutdown();
        return false;
    }

    // Create VCE encoder component
    AMFComponentPtr encoder;
    res = factory->CreateComponent((AMFContext*)m_context, AMFVideoEncoderVCE_AVC, &encoder);
    if (res != AMF_OK)
    {
        logf("[AMF] CreateComponent VCE failed: %d", (int)res);
        Shutdown();
        return false;
    }

    AMFComponent *enc = encoder;

    enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);

    enc->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_MAIN);
    enc->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, AMF_H264_LEVEL__5);
    enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width, height));
    enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));

    enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP);
    enc->SetProperty(AMF_VIDEO_ENCODER_QP_I, qp);
    enc->SetProperty(AMF_VIDEO_ENCODER_QP_P, qp);
    enc->SetProperty(AMF_VIDEO_ENCODER_QP_B, qp);

    enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
    enc->SetProperty(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, 1);
    enc->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);

    enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, fps * 2);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, fps * 2);
    enc->SetProperty(AMF_VIDEO_ENCODER_INSERT_AUD, true);
    enc->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CABAC);
    enc->SetProperty(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, false);

    enc->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    enc->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);

    res = enc->Init(AMF_SURFACE_NV12, width, height);
    if (res != AMF_OK)
    {
        logf("[AMF] encoder Init failed: %d", (int)res);
        Shutdown();
        return false;
    }

    m_encoder = encoder.Detach();
    m_initialized = true;
    m_frame_count = 0;

    logf("[AMF] encoder initialized: %dx%d %dfps qp=%d", width, height, fps, qp);

    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2) * 2;
    m_nv12_buffer.resize(y_size + uv_size);

    return true;
}

bool AMFEncoder::EncodeFrame(const std::vector<uint8_t> &bgra,
                             std::vector<uint8_t> &out_nal)
{
    if (!m_initialized || !m_encoder || !m_context)
        return false;

    auto t0 = std::chrono::steady_clock::now();

    int y_size = m_width * m_height;
    int uv_size = (m_width / 2) * (m_height / 2) * 2;
    uint8_t *y_plane = m_nv12_buffer.data();
    uint8_t *uv_plane = y_plane + y_size;

    bgra_to_nv12(bgra.data(), m_width, m_height, y_plane, uv_plane);

    auto t1 = std::chrono::steady_clock::now();

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_d3d_ctx->Map(m_nv12_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        logf("[AMF] Map texture failed: 0x%08X", (unsigned)hr);
        return false;
    }

    for (int row = 0; row < m_height; row++)
        memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
               y_plane + row * m_width, m_width);

    uint8_t *uv_dst = (uint8_t*)mapped.pData + m_height * mapped.RowPitch;
    int uv_rows = m_height / 2;
    int uv_row_bytes = m_width;
    for (int row = 0; row < uv_rows; row++)
        memcpy(uv_dst + row * mapped.RowPitch,
               uv_plane + row * uv_row_bytes, uv_row_bytes);

    m_d3d_ctx->Unmap(m_nv12_tex, 0);

    auto t2 = std::chrono::steady_clock::now();

    AMFSurfacePtr surface;
    AMF_RESULT res = ((AMFContext*)m_context)->CreateSurfaceFromDX11Native(
        m_nv12_tex, &surface, nullptr);
    if (res != AMF_OK)
    {
        logf("[AMF] CreateSurfaceFromDX11Native failed: %d", (int)res);
        return false;
    }

    surface->SetPts(m_frame_count);

    auto t3 = std::chrono::steady_clock::now();

    res = ((AMFComponent*)m_encoder)->SubmitInput(surface);
    while (res == AMF_INPUT_FULL)
    {
        if (!CreateOutputBuffer(out_nal))
            return false;
        res = ((AMFComponent*)m_encoder)->SubmitInput(surface);
    }
    if (res != AMF_OK)
    {
        logf("[AMF] SubmitInput failed: %d at frame %d", (int)res, m_frame_count);
        return false;
    }

    auto t4 = std::chrono::steady_clock::now();

    int drained = 0;
    while (CreateOutputBuffer(out_nal))
    {
        drained++;
    }

    auto t5 = std::chrono::steady_clock::now();

    (void)t0; (void)t1; (void)t2; (void)t3; (void)t4; (void)t5; (void)drained;

    m_frame_count++;
    return true;
}

bool AMFEncoder::CreateOutputBuffer(std::vector<uint8_t> &out_nal)
{
    AMFDataPtr data;
    AMF_RESULT res = ((AMFComponent*)m_encoder)->QueryOutput(&data);
    if (res != AMF_OK || !data)
        return false;

    AMFBuffer *buf = nullptr;
    if (data->QueryInterface(AMFBuffer::IID(), (void**)&buf) != AMF_OK || !buf)
        return false;

    amf_size size = buf->GetSize();
    if (size > 0)
    {
        const uint8_t *buf_data = (const uint8_t*)buf->GetNative();
        if (buf_data)
        {
            size_t old_sz = out_nal.size();
            out_nal.resize(out_nal.size() + size);
            memcpy(out_nal.data() + old_sz, buf_data, size);
        }
    }

    buf->Release();
    return true;
}

void AMFEncoder::Flush(std::vector<uint8_t> &out_nal)
{
    if (!m_encoder || !m_context)
        return;

    ((AMFComponent*)m_encoder)->Flush();

    while (CreateOutputBuffer(out_nal))
    {
    }
}

void AMFEncoder::Shutdown()
{
    m_initialized = false;

    if (m_nv12_tex)
    {
        m_nv12_tex->Release();
        m_nv12_tex = nullptr;
    }
    if (m_d3d_ctx)
    {
        m_d3d_ctx->Release();
        m_d3d_ctx = nullptr;
    }
    if (m_d3d_device)
    {
        m_d3d_device->Release();
        m_d3d_device = nullptr;
    }

    if (m_encoder)
    {
        ((AMFComponent*)m_encoder)->Drain();
        ((AMFComponent*)m_encoder)->Terminate();
        ((AMFComponent*)m_encoder)->Release();
        m_encoder = nullptr;
    }
    if (m_context)
    {
        ((AMFContext*)m_context)->Terminate();
        ((AMFContext*)m_context)->Release();
        m_context = nullptr;
    }
    m_factory = nullptr;
    if (m_amf_dll)
    {
        FreeLibrary(m_amf_dll);
        m_amf_dll = nullptr;
    }

    m_nv12_buffer.clear();
    m_width = 0;
    m_height = 0;
    m_frame_count = 0;
}
