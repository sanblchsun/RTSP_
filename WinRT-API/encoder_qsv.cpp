#include "encoder_qsv.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

extern void logf(const char *fmt, ...);

// oneVPL / Media SDK headers
#include <vpl/mfx.h>

// BGRA -> NV12 (same as encoder_amf.cpp)
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

QSVEncoder::QSVEncoder() {}
QSVEncoder::~QSVEncoder() { Shutdown(); }

bool QSVEncoder::IsAvailable()
{
    // Check if the Intel Media SDK / oneVPL runtime DLL is loadable
    HMODULE h = LoadLibraryA("libmfxhw64.dll");
    if (h) { FreeLibrary(h); return true; }
    h = LoadLibraryA("mfxhw64.dll");
    if (h) { FreeLibrary(h); return true; }
    return false;
}

bool QSVEncoder::Initialize(int width, int height, int fps, int qp)
{
    Shutdown();

    if (width <= 0 || height <= 0 || fps <= 0 || qp < 0 || qp > 51 || (width & 1) || (height & 1))
        return false;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_qp = qp;

    // Create MFX session via oneVPL dispatcher (statically linked libvpl.a)
    mfxSession session = nullptr;
    mfxVersion ver = { 1, 1 };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    mfxStatus sts = MFXInit(MFX_IMPL_HARDWARE_ANY, &ver, &session);
#pragma GCC diagnostic pop
    if (sts != MFX_ERR_NONE || !session)
    {
        logf("[QSV] MFXInit failed: %d", (int)sts);
        Shutdown();
        return false;
    }
    m_session = session;

    // ---- Configure encoding params ----
    mfxVideoParam vp = {};
    vp.mfx.CodecId = MFX_CODEC_AVC;
    vp.mfx.CodecProfile = MFX_PROFILE_AVC_MAIN;
    vp.mfx.TargetUsage = 4;               // balanced quality/speed (was 7=fastest)
    vp.mfx.GopPicSize = fps * 2;          // IDR every 2 seconds
    vp.mfx.GopRefDist = 1;                // no B-frames
    vp.mfx.IdrInterval = 0;               // every I-frame is IDR
    vp.mfx.NumRefFrame = 2;               // 2 ref frames (was 1 — artefacts)
    vp.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
    vp.mfx.QPI = (mfxU16)qp;
    vp.mfx.QPP = (mfxU16)qp;
    vp.mfx.QPB = (mfxU16)qp;
    vp.mfx.BufferSizeInKB = 1024;
    vp.mfx.NumSlice = 1;
    vp.mfx.EncodedOrder = 0;

    vp.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    // Frame info
    vp.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    vp.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    vp.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    vp.mfx.FrameInfo.Width = (mfxU16)width;
    vp.mfx.FrameInfo.Height = (mfxU16)height;
    vp.mfx.FrameInfo.CropX = 0;
    vp.mfx.FrameInfo.CropY = 0;
    vp.mfx.FrameInfo.CropW = (mfxU16)width;
    vp.mfx.FrameInfo.CropH = (mfxU16)height;
    vp.mfx.FrameInfo.FrameRateExtN = (mfxU32)fps;
    vp.mfx.FrameInfo.FrameRateExtD = 1;

    // Validate params (warnings are OK — Query adjusted params)
    sts = MFXVideoENCODE_Query(session, &vp, &vp);
    if (sts < 0)
    {
        logf("[QSV] MFXVideoENCODE_Query failed: %d", (int)sts);
        Shutdown();
        return false;
    }

    // Query surface requirements
    mfxFrameAllocRequest request = {};
    sts = MFXVideoENCODE_QueryIOSurf(session, &vp, &request);
    if (sts != MFX_ERR_NONE)
    {
        logf("[QSV] MFXVideoENCODE_QueryIOSurf failed: %d", (int)sts);
        Shutdown();
        return false;
    }

    if (request.NumFrameSuggested < 4)
        request.NumFrameSuggested = 4;

    // Init encoder (warnings are OK)
    sts = MFXVideoENCODE_Init(session, &vp);
    if (sts < 0)
    {
        logf("[QSV] MFXVideoENCODE_Init failed: %d", (int)sts);
        Shutdown();
        return false;
    }

    // Allocate surfaces
    m_n_surfaces = (int)request.NumFrameSuggested;
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2) * 2;
    int frame_bytes = y_size + uv_size;

    m_surface_data = new uint8_t[(size_t)m_n_surfaces * frame_bytes];
    m_surfaces = new mfxFrameSurface1[(size_t)m_n_surfaces];

    {
        mfxFrameSurface1 *surfaces = (mfxFrameSurface1*)m_surfaces;
        for (int i = 0; i < m_n_surfaces; i++)
        {
            memset(&surfaces[i], 0, sizeof(mfxFrameSurface1));
            surfaces[i].Info = vp.mfx.FrameInfo;
            surfaces[i].Data.Y = m_surface_data + (size_t)i * frame_bytes;
            surfaces[i].Data.U = surfaces[i].Data.Y + y_size;
            surfaces[i].Data.V = surfaces[i].Data.U + 1;
            surfaces[i].Data.PitchLow = (mfxU16)width;
        }
    }

    // Allocate bitstream output buffer (4MB, enough for worst-case)
    m_bitstream_data.resize(8 * 1024 * 1024);
    m_bitstream = new mfxBitstream();
    memset(m_bitstream, 0, sizeof(mfxBitstream));
    ((mfxBitstream*)m_bitstream)->Data = m_bitstream_data.data();
    ((mfxBitstream*)m_bitstream)->MaxLength = (mfxU32)m_bitstream_data.size();

    // NV12 scratch buffer for BGRA conversion
    m_nv12_buffer.resize((size_t)frame_bytes);

    m_initialized = true;
    m_frame_count = 0;
    m_surface_index = 0;

    logf("[QSV] encoder initialized: %dx%d %dfps qp=%d", width, height, fps, qp);
    return true;
}

bool QSVEncoder::EncodeFrame(const std::vector<uint8_t> &bgra,
                              std::vector<uint8_t> &out_nal)
{
    if (!m_initialized)
        return false;

    auto t0 = std::chrono::steady_clock::now();

    int y_size = m_width * m_height;
    int uv_size = (m_width / 2) * (m_height / 2) * 2;
    uint8_t *y_plane = m_nv12_buffer.data();
    uint8_t *uv_plane = y_plane + y_size;

    bgra_to_nv12(bgra.data(), m_width, m_height, y_plane, uv_plane);

    auto t1 = std::chrono::steady_clock::now();

    // Get next surface from the pool (round-robin)
    mfxFrameSurface1 *surf = &((mfxFrameSurface1*)m_surfaces)[m_surface_index];
    m_surface_index = (m_surface_index + 1) % m_n_surfaces;

    // Copy NV12 data to surface
    memcpy(surf->Data.Y, y_plane, (size_t)y_size);
    memcpy(surf->Data.U, uv_plane, (size_t)uv_size);

    surf->Data.TimeStamp = m_frame_count * 1000LL / m_fps; // ms
    surf->Info.CropW = (mfxU16)m_width;
    surf->Info.CropH = (mfxU16)m_height;

    auto t2 = std::chrono::steady_clock::now();

    if (!DoEncode((void*)surf, out_nal))
        return false;

    auto t3 = std::chrono::steady_clock::now();

    (void)t0; (void)t1; (void)t2; (void)t3;

    m_frame_count++;
    return true;
}

bool QSVEncoder::DoEncode(void *surface_ptr, std::vector<uint8_t> &out_nal)
{
    mfxFrameSurface1 *surface = (mfxFrameSurface1*)surface_ptr;
    mfxBitstream *bs = (mfxBitstream*)m_bitstream;
    bs->DataLength = 0;
    bs->DataOffset = 0;

    mfxSyncPoint syncp = nullptr;
    mfxSession session = (mfxSession)m_session;

    mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(session, nullptr, surface, bs, &syncp);

    // If buffer too small — grow and retry
    if (sts == MFX_ERR_NOT_ENOUGH_BUFFER)
    {
        size_t new_sz = m_bitstream_data.size() * 2;
        m_bitstream_data.resize(new_sz);
        bs->Data = m_bitstream_data.data();
        bs->MaxLength = (mfxU32)m_bitstream_data.size();
        bs->DataOffset = 0;
        bs->DataLength = 0;
        sts = MFXVideoENCODE_EncodeFrameAsync(session, nullptr, surface, bs, &syncp);
    }

    if (sts == MFX_ERR_NONE && syncp)
    {
        sts = MFXVideoCORE_SyncOperation(session, syncp, 10000);
        if (sts != MFX_ERR_NONE)
        {
            logf("[QSV] SyncOperation failed: %d at frame %d", (int)sts, m_frame_count);
            return false;
        }
        if (bs->DataLength > 0)
        {
            size_t old_sz = out_nal.size();
            out_nal.resize(old_sz + bs->DataLength);
            memcpy(out_nal.data() + old_sz, bs->Data + bs->DataOffset, bs->DataLength);
        }
        return true;
    }

    if (sts == MFX_ERR_NONE && !syncp)
    {
        // Frame already in bitstream (async immediate completion)
        if (bs->DataLength > 0)
        {
            size_t old_sz = out_nal.size();
            out_nal.resize(old_sz + bs->DataLength);
            memcpy(out_nal.data() + old_sz, bs->Data + bs->DataOffset, bs->DataLength);
        }
        return true;
    }

    if (sts == MFX_ERR_MORE_DATA)
    {
        return true;
    }

    if (sts == MFX_WRN_DEVICE_BUSY)
        return true;

    logf("[QSV] EncodeFrameAsync failed: %d at frame %d", (int)sts, m_frame_count);
    return false;
}

void QSVEncoder::Flush(std::vector<uint8_t> &out_nal)
{
    if (!m_session)
        return;

    mfxBitstream *bs = (mfxBitstream*)m_bitstream;
    mfxSession session = (mfxSession)m_session;

    for (int retry = 0; retry < 5; retry++)
    {
        bs->DataLength = 0;
        mfxSyncPoint syncp = nullptr;
        mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(session, nullptr, nullptr, bs, &syncp);
        if (sts == MFX_ERR_NONE && syncp)
        {
            retry = 0;
            sts = MFXVideoCORE_SyncOperation(session, syncp, 10000);
            if (sts == MFX_ERR_NONE && bs->DataLength > 0)
            {
                size_t old_sz = out_nal.size();
                out_nal.resize(old_sz + bs->DataLength);
                memcpy(out_nal.data() + old_sz, bs->Data + bs->DataOffset, bs->DataLength);
            }
        }
        else if (sts == MFX_ERR_MORE_DATA)
        {
            break;
        }
        else if (sts == MFX_WRN_DEVICE_BUSY)
        {
            retry--;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        else
        {
            break;
        }
    }
}

void QSVEncoder::FreeSurfaces()
{
    if (m_surfaces)
    {
        delete[] (mfxFrameSurface1*)m_surfaces;
        m_surfaces = nullptr;
    }
    if (m_surface_data)
    {
        delete[] m_surface_data;
        m_surface_data = nullptr;
    }
    m_n_surfaces = 0;
}

void QSVEncoder::Shutdown()
{
    m_initialized = false;

    if (m_session)
    {
        MFXVideoENCODE_Close((mfxSession)m_session);
        MFXClose((mfxSession)m_session);
        m_session = nullptr;
    }

    FreeSurfaces();

    if (m_bitstream)
    {
        delete (mfxBitstream*)m_bitstream;
        m_bitstream = nullptr;
    }
    m_bitstream_data.clear();
    m_nv12_buffer.clear();

    m_width = 0;
    m_height = 0;
    m_frame_count = 0;
}
