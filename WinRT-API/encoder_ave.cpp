#include "encoder_ave.h"
#include <cstring>
#include <cstdio>
#include <chrono>

extern void logf(const char *fmt, ...);

// AMD AVE (Advanced Video Encoder) API — native VCE interface via amdave64.dll
//
// This encoder uses the AMD display driver's VCE (Video Coding Engine) directly,
// without requiring amfrt64.dll. Works on VCE 1.0+ (Trinity/Kabini APUs).

// ============ AVE API type definitions ============

#pragma pack(push, 1)

struct AVEConfig {
    DWORD size;             // sizeof(AVEConfig)
    DWORD width;            // frame width
    DWORD height;           // frame height
    DWORD fps_num;          // FPS numerator
    DWORD fps_den;          // FPS denominator
    DWORD bitrate;          // bitrate in bps
    DWORD vbv_size;         // VBV buffer in bits
    DWORD rc_mode;          // 0=CBR, 1=VBR
    DWORD profile;          // 66=Baseline, 77=Main, 100=High
    DWORD level;            // level*10 (40=4.0, 42=4.2, 50=5.0)
    DWORD idr_period;       // IDR interval (frames)
    DWORD qp;               // QP for CQP
    DWORD bframes;          // B-frame count
    DWORD ref_frames;       // reference frames
    DWORD cabac;            // 0=CAVLC, 1=CABAC
    DWORD deblock;          // 0=off, 1=on
    DWORD aud;              // 0=no AUD, 1=AUD
};

struct AVEInput {
    DWORD size;             // sizeof(AVEInput)
    const void *data;       // NV12 data
    DWORD width;            // data width
    DWORD height;           // data height
    DWORD pitch;            // line pitch (bytes)
    DWORD pts;              // presentation timestamp
};

struct AVEOutput {
    DWORD size;             // sizeof(AVEOutput)
    BYTE *data;             // encoded data buffer
    DWORD data_size;        // valid encoded bytes
    DWORD data_capacity;    // allocated buffer size
    DWORD frame_type;       // 0=delta, 1=IDR
    DWORD pts;              // pts
};

#pragma pack(pop)

static const DWORD AVE_OK = 0;

// ============ BGRA -> NV12 conversion (same as AMF) ============

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

// ============ AVEEncoder ============

AVEEncoder::AVEEncoder() {}
AVEEncoder::~AVEEncoder() { Shutdown(); }

bool AVEEncoder::IsAvailable()
{
    HMODULE dll = LoadLibraryA("amdave64.dll");
    if (!dll)
    {
        logf("[AVE] amdave64.dll not found");
        return false;
    }

    //logf("[AVE] amdave64.dll found");
    return true;
}

bool AVEEncoder::LoadAVE()
{
    if (m_ave_dll) return true;

    m_ave_dll = LoadLibraryA("amdave64.dll");
    if (!m_ave_dll)
    {
        logf("[AVE] amdave64.dll not found");
        return false;
    }

    // Resolve API functions — try both "AVE" and "AMDAVE" prefixes
    auto resolve = [&](const char *name) -> FARPROC {
        FARPROC p = GetProcAddress(m_ave_dll, name);
        if (!p)
        {
            std::string alt = "AMD" + std::string(name);
            p = GetProcAddress(m_ave_dll, alt.c_str());
        }
        return p;
    };

    // Only Encode and Destroy are truly required for basic operation
    m_fn_encode = resolve("AVEEncoderEncode");
    m_fn_flush  = resolve("AVEEncoderFlush");
    m_fn_destroy = resolve("AVEDestroyEncoder");

    if (!m_fn_encode)
    {
        logf("[AVE] no Encode entry point found");
        FreeLibrary(m_ave_dll);
        m_ave_dll = nullptr;
        return false;
    }

    //logf("[AVE] loaded amdave64.dll, encode=0x%p flush=0x%p destroy=0x%p",
    //     m_fn_encode, m_fn_flush, m_fn_destroy);
    return true;
}

bool AVEEncoder::Initialize(int width, int height, int fps, int qp)
{
    Shutdown();

    if (width <= 0 || height <= 0 || fps <= 0 || qp < 0 || qp > 51 || (width & 1) || (height & 1))
        return false;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_qp = qp;

    if (!LoadAVE())
        return false;

    // Resolve CreateEncoder + SetConfig here (not needed after init)
    typedef DWORD (WINAPI *CreateEncoder_fn)(DWORD, void**);
    typedef DWORD (WINAPI *SetConfig_fn)(void*, const AVEConfig*);

    auto resolve = [&](const char *name) -> FARPROC {
        FARPROC p = GetProcAddress(m_ave_dll, name);
        if (!p)
        {
            std::string alt = "AMD" + std::string(name);
            p = GetProcAddress(m_ave_dll, alt.c_str());
        }
        return p;
    };

    auto fn_create  = (CreateEncoder_fn)resolve("AVECreateEncoder");
    auto fn_setcfg  = (SetConfig_fn)resolve("AVESetConfig");

    if (!fn_create || !fn_setcfg)
    {
        logf("[AVE] CreateEncoder or SetConfig not found");
        Shutdown();
        return false;
    }

    // Create encoder instance
    void *enc = nullptr;
    DWORD res = fn_create(0, &enc);  // type 0 = H.264
    if (res != AVE_OK || !enc)
    {
        logf("[AVE] CreateEncoder failed: %lu", res);
        Shutdown();
        return false;
    }
    m_encoder = enc;

    // Configure encoder
    AVEConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.width = width;
    cfg.height = height;
    cfg.fps_num = fps;
    cfg.fps_den = 1;
    cfg.bitrate = 0;
    cfg.vbv_size = 0;
    cfg.rc_mode = 2;  // CQP
    cfg.profile = 77;  // Main
    cfg.level = 50;    // 5.0
    cfg.idr_period = fps * 2;
    cfg.qp = qp;
    cfg.bframes = 0;
    cfg.ref_frames = 1;
    cfg.cabac = 1;
    cfg.deblock = 0;
    cfg.aud = 1;

    res = fn_setcfg(enc, &cfg);
    if (res != AVE_OK)
    {
        logf("[AVE] SetConfig failed: %lu", res);
        Shutdown();
        return false;
    }

    // Allocate NV12 conversion buffer
    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2) * 2;
    m_nv12_buffer.resize(y_size + uv_size);

    m_initialized = true;
    m_frame_count = 0;

    logf("[AVE] encoder initialized: %dx%d %dfps qp=%d", width, height, fps, qp);
    return true;
}

bool AVEEncoder::EncodeFrame(const std::vector<uint8_t> &bgra,
                              std::vector<uint8_t> &out_nal)
{
    if (!m_initialized || !m_encoder || !m_fn_encode)
        return false;

    auto t0 = std::chrono::steady_clock::now();

    // Convert BGRA -> NV12
    int y_size = m_width * m_height;
    int uv_size = (m_width / 2) * (m_height / 2) * 2;
    uint8_t *y_plane = m_nv12_buffer.data();
    uint8_t *uv_plane = y_plane + y_size;

    bgra_to_nv12(bgra.data(), m_width, m_height, y_plane, uv_plane);

    auto t1 = std::chrono::steady_clock::now();

    // Prepare input
    AVEInput input;
    memset(&input, 0, sizeof(input));
    input.size = sizeof(input);
    input.data = m_nv12_buffer.data();
    input.width = m_width;
    input.height = m_height;
    input.pitch = m_width;
    input.pts = m_frame_count;

    // Output buffer (static to avoid realloc)
    static std::vector<BYTE> enc_buf;
    static const DWORD OUTPUT_SIZE = 2 * 1024 * 1024;
    if (enc_buf.empty())
        enc_buf.resize(OUTPUT_SIZE);

    AVEOutput output;
    memset(&output, 0, sizeof(output));
    output.size = sizeof(output);
    output.data = enc_buf.data();
    output.data_capacity = OUTPUT_SIZE;

    auto t2 = std::chrono::steady_clock::now();

    // Encode
    typedef DWORD (WINAPI *EncodeFn)(void*, const AVEInput*, AVEOutput*);
    DWORD res = ((EncodeFn)m_fn_encode)(m_encoder, &input, &output);

    auto t3 = std::chrono::steady_clock::now();

    if (res != AVE_OK)
    {
        logf("[AVE] Encode failed: %lu at frame %d", res, m_frame_count);
        return false;
    }

    // Copy output
    if (output.data_size > 0)
    {
        size_t old_sz = out_nal.size();
        out_nal.resize(old_sz + output.data_size);
        memcpy(out_nal.data() + old_sz, output.data, output.data_size);
    }

    // Log timing every 100 frames
    if ((m_frame_count % 100) == 0 && (m_frame_count > 0))
    {
        auto cvt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        auto setup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        auto enc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    }

    m_frame_count++;
    return true;
}

void AVEEncoder::Flush(std::vector<uint8_t> &out_nal)
{
    if (!m_encoder || !m_fn_flush)
        return;

    static std::vector<BYTE> enc_buf;
    static const DWORD OUTPUT_SIZE = 2 * 1024 * 1024;
    if (enc_buf.empty())
        enc_buf.resize(OUTPUT_SIZE);

    typedef DWORD (WINAPI *FlushFn)(void*, AVEOutput*);

    while (true)
    {
        AVEOutput output;
        memset(&output, 0, sizeof(output));
        output.size = sizeof(output);
        output.data = enc_buf.data();
        output.data_capacity = OUTPUT_SIZE;

        DWORD res = ((FlushFn)m_fn_flush)(m_encoder, &output);
        if (res != AVE_OK || output.data_size == 0)
            break;
        size_t old_sz = out_nal.size();
        out_nal.resize(old_sz + output.data_size);
        memcpy(out_nal.data() + old_sz, output.data, output.data_size);
    }
}

void AVEEncoder::Shutdown()
{
    m_initialized = false;

    if (m_encoder && m_fn_destroy)
    {
        typedef DWORD (WINAPI *DestroyFn)(void*);
        ((DestroyFn)m_fn_destroy)(m_encoder);
        m_encoder = nullptr;
    }

    m_fn_encode = nullptr;
    m_fn_flush = nullptr;
    m_fn_destroy = nullptr;

    if (m_ave_dll)
    {
        FreeLibrary(m_ave_dll);
        m_ave_dll = nullptr;
    }

    m_nv12_buffer.clear();
    m_width = 0;
    m_height = 0;
    m_frame_count = 0;
}
