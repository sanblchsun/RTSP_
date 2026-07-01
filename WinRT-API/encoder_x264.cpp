#include "encoder_x264.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>

extern void logf(const char *fmt, ...);

static void x264_log_cb(void *, int i_level, const char *psz, va_list va)
{
    // Prepend "[x264]" and forward through logf
    char buf[512];
    vsnprintf(buf, sizeof(buf), psz, va);
    logf("[x264] %s", buf);
}

X264Encoder::X264Encoder()
{
    x264_picture_init(&m_pic_out);
}

X264Encoder::~X264Encoder()
{
    Shutdown();
}

bool X264Encoder::Initialize(int width, int height, int fps, int qp)
{
    if (m_encoder)
        Shutdown();

    if (width <= 0 || height <= 0 || fps <= 0 || qp < 0 || qp > 51 || (width & 1) || (height & 1))
        return false;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_qp = qp;

    x264_param_default_preset(&m_params, "ultrafast", "zerolatency");
    // Use I420 (YUV 4:2:0) for maximum compatibility, not BGRA
    m_params.i_csp = X264_CSP_I420;
    x264_param_apply_profile(&m_params, "main");

    m_params.i_width = width;
    m_params.i_height = height;
    m_params.i_fps_num = fps;
    m_params.i_fps_den = 1;
    m_params.i_timebase_num = 1;
    m_params.i_timebase_den = fps;

    m_params.i_threads = 0;             // auto-detect CPU cores (4 on R-464L)
    m_params.b_sliced_threads = 1;      // slice-level parallelism (encode 1 frame using all cores)
    m_params.i_sync_lookahead = 0;
    m_params.rc.i_lookahead = 0;

    m_params.i_keyint_max = fps * 2;
    m_params.i_keyint_min = fps;
    m_params.i_scenecut_threshold = 0;
    m_params.i_frame_reference = 1;
    m_params.i_bframe = 0;
    m_params.b_intra_refresh = 1;
    m_params.b_deblocking_filter = 0;
    // Set x264 log callback to capture encoder errors
    m_params.pf_log = x264_log_cb;
    m_params.i_log_level = X264_LOG_WARNING;
    m_params.p_log_private = nullptr;

    m_params.rc.i_rc_method = X264_RC_CRF;
    m_params.rc.f_rf_constant = (float)qp;
    m_params.rc.f_rf_constant_max = (float)(qp + 3);

    // NAL HRD conformance + AUD
    m_params.b_repeat_headers = 1;
    m_params.b_annexb = 1;
    m_params.b_aud = 0;

    m_params.i_sps_id = 0;

    m_params.analyse.i_me_method = X264_ME_DIA;
    m_params.analyse.i_subpel_refine = 0;
    m_params.analyse.i_weighted_pred = 0;
    m_params.analyse.i_direct_mv_pred = X264_DIRECT_PRED_NONE;
    m_params.analyse.i_me_range = 0;
    m_params.analyse.i_mv_range = -1;
    m_params.analyse.i_trellis = 0;
    m_params.analyse.b_fast_pskip = 1;
    m_params.analyse.i_noise_reduction = 0;

    m_encoder = x264_encoder_open(&m_params);
    if (!m_encoder)
    {
        logf("[x264] x264_encoder_open failed: %dx%d fps=%d qp=%d",
             width, height, fps, qp);
        return false;
    }

    m_frame_count = 0;

    // Save SPS/PPS from encoder headers
    m_sps.clear();
    m_pps.clear();
    {
        x264_nal_t *nals;
        int i_nals;
        x264_encoder_headers(m_encoder, &nals, &i_nals);
        for (int i = 0; i < i_nals; i++)
        {
            const uint8_t *p = nals[i].p_payload;
            int sz = nals[i].i_payload;
            // Skip annex-B start code (3 or 4 bytes)
            int offset = 0;
            if (sz >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
                offset = 4;
            else if (sz >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
                offset = 3;
            else
                continue;
            uint8_t type = p[offset] & 0x1F;
            if (type == 7)
                m_sps.assign(p + offset, p + sz);
            else if (type == 8)
                m_pps.assign(p + offset, p + sz);
        }
    }
    logf("[x264] encoder initialized: %dx%d %dfps qp=%d sps=%zu pps=%zu",
         width, height, fps, qp, m_sps.size(), m_pps.size());

    return true;
}

static void bgra_to_i420(const uint8_t *bgra, int w, int h,
                         uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane)
{
    const int y_stride = w;
    const int uv_stride = w / 2;
    for (int row = 0; row < h; row++)
    {
        const uint8_t *src = bgra + row * w * 4;
        uint8_t *ydst = y_plane + row * y_stride;
        for (int col = 0; col < w; col++)
        {
            int b = src[col * 4 + 0];
            int g = src[col * 4 + 1];
            int r = src[col * 4 + 2];
            int a = src[col * 4 + 3]; (void)a;
            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            if (y < 0) y = 0; if (y > 255) y = 255;
            ydst[col] = (uint8_t)y;
        }
    }
    for (int row = 0; row < h / 2; row++)
    {
        const uint8_t *src1 = bgra + (row * 2) * w * 4;
        const uint8_t *src2 = bgra + (row * 2 + 1) * w * 4;
        uint8_t *udst = u_plane + row * uv_stride;
        uint8_t *vdst = v_plane + row * uv_stride;
        for (int col = 0; col < w / 2; col++)
        {
            int col2 = col * 2;
            int b = (src1[col2 * 4 + 0] + src1[(col2 + 1) * 4 + 0] +
                     src2[col2 * 4 + 0] + src2[(col2 + 1) * 4 + 0]) / 4;
            int g = (src1[col2 * 4 + 1] + src1[(col2 + 1) * 4 + 1] +
                     src2[col2 * 4 + 1] + src2[(col2 + 1) * 4 + 1]) / 4;
            int r = (src1[col2 * 4 + 2] + src1[(col2 + 1) * 4 + 2] +
                     src2[col2 * 4 + 2] + src2[(col2 + 1) * 4 + 2]) / 4;
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            if (u < 0) u = 0; if (u > 255) u = 255;
            if (v < 0) v = 0; if (v > 255) v = 255;
            udst[col] = (uint8_t)u;
            vdst[col] = (uint8_t)v;
        }
    }
}

bool X264Encoder::EncodeFrame(const std::vector<uint8_t> &bgra,
                              std::vector<uint8_t> &out_nal)
{
    if (!m_encoder)
        return false;
    if (bgra.empty())
        return false;

    // Convert BGRA → I420
    int y_size = m_width * m_height;
    int uv_size = (m_width / 2) * (m_height / 2);
    m_yuv_buffer.resize(y_size + uv_size * 2);
    uint8_t *y_plane = m_yuv_buffer.data();
    uint8_t *u_plane = y_plane + y_size;
    uint8_t *v_plane = u_plane + uv_size;
    bgra_to_i420(bgra.data(), m_width, m_height, y_plane, u_plane, v_plane);

    x264_picture_t pic;
    x264_picture_init(&pic);

    pic.i_type = (m_frame_count == 0) ? X264_TYPE_IDR : X264_TYPE_AUTO;
    pic.i_pts = m_frame_count;
    pic.img.i_csp = X264_CSP_I420;
    pic.img.i_plane = 3;
    pic.img.plane[0] = y_plane;
    pic.img.plane[1] = u_plane;
    pic.img.plane[2] = v_plane;
    pic.img.i_stride[0] = m_width;
    pic.img.i_stride[1] = m_width / 2;
    pic.img.i_stride[2] = m_width / 2;

    x264_nal_t *nals;
    int i_nals;
    int frame_size = x264_encoder_encode(m_encoder, &nals, &i_nals, &pic, &m_pic_out);
    if (frame_size < 0)
    {
        logf("[x264] encode error at frame %d", m_frame_count);
        return false;
    }

    if (frame_size > 0)
    {
        // Copy NALs to output buffer (with start codes)
        for (int i = 0; i < i_nals; i++)
        {
            const uint8_t *nal_data = nals[i].p_payload;
            int nal_size = nals[i].i_payload;

            if (nal_size > 0)
            {
                size_t old_sz = out_nal.size();
                out_nal.resize(out_nal.size() + (size_t)nal_size);
                std::memcpy(out_nal.data() + old_sz, nal_data, (size_t)nal_size);
            }
        }
    }

    m_frame_count++;
    return true;
}

void X264Encoder::Flush(std::vector<uint8_t> &out_nal)
{
    if (!m_encoder)
        return;

    x264_nal_t *nals;
    int i_nals;
    while (x264_encoder_encode(m_encoder, &nals, &i_nals, nullptr, &m_pic_out) > 0)
    {
        for (int i = 0; i < i_nals; i++)
        {
            if (nals[i].i_payload > 0)
            {
                size_t old_sz = out_nal.size();
                out_nal.resize(out_nal.size() + (size_t)nals[i].i_payload);
                std::memcpy(out_nal.data() + old_sz, nals[i].p_payload, (size_t)nals[i].i_payload);
            }
        }
    }
}

void X264Encoder::Shutdown()
{
    if (m_encoder)
    {
        x264_encoder_close(m_encoder);
        m_encoder = nullptr;
    }
    m_yuv_buffer.clear();
    m_width = 0;
    m_height = 0;
    m_frame_count = 0;
}
