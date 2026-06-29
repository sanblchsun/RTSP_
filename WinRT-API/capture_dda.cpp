#include "capture_dda.h"
#include <cstring>

extern void logf(const char *fmt, ...);

DDACapture::DDACapture() {}

DDACapture::~DDACapture()
{
    Shutdown();
}

bool DDACapture::Initialize(int fps)
{
    if (m_initialized)
        return true;

    m_fps = (fps > 0) ? fps : 15;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);
    if (FAILED(hr))
    {
        logf("[dda] D3D11CreateDevice HARDWARE failed: 0x%08X, trying WARP...", hr);
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_device, nullptr, &m_context);
        if (FAILED(hr))
        {
            logf("[dda] D3D11CreateDevice WARP also failed: 0x%08X", hr);
            return false;
        }
        //logf("[dda] D3D11CreateDevice WARP OK");
    }

    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&m_dxgi_device);
    if (FAILED(hr))
    {
        logf("[dda] QueryInterface IDXGIDevice failed: 0x%08X", hr);
        m_context->Release(); m_context = nullptr;
        m_device->Release(); m_device = nullptr;
        return false;
    }

    hr = m_dxgi_device->GetAdapter(&m_adapter);
    if (FAILED(hr))
    {
        logf("[dda] GetAdapter failed: 0x%08X", hr);
        m_dxgi_device->Release(); m_dxgi_device = nullptr;
        m_context->Release(); m_context = nullptr;
        m_device->Release(); m_device = nullptr;
        return false;
    }

    if (!EnumerateOutputs())
    {
        Shutdown();
        return false;
    }

    m_initialized = true;
    logf("[dda] initialized: %d outputs, virtual %dx%d",
         (int)m_outputs.size(), m_virtual_w, m_virtual_h);
    return true;
}

bool DDACapture::EnumerateOutputs()
{
    m_outputs.clear();

    // Get virtual desktop bounds
    struct Bbox { int min_x; int min_y; int max_x; int max_y; };
    Bbox bbox = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
            Bbox *b = (Bbox *)lp;
            if (r->left < b->min_x) b->min_x = r->left;
            if (r->top < b->min_y) b->min_y = r->top;
            if (r->right > b->max_x) b->max_x = r->right;
            if (r->bottom > b->max_y) b->max_y = r->bottom;
            return TRUE;
        }, (LPARAM)&bbox);

    if (bbox.min_x == INT_MAX)
    {
        logf("[dda] EnumDisplayMonitors returned no monitors");
        return false;
    }

    m_virtual_w = bbox.max_x - bbox.min_x;
    m_virtual_h = bbox.max_y - bbox.min_y;
    m_virtual_origin_x = bbox.min_x;
    m_virtual_origin_y = bbox.min_y;

    // Enumerate DXGI outputs
    IDXGIOutput *dxgi_output = nullptr;
    int idx = 0;
    while (m_adapter->EnumOutputs(idx, &dxgi_output) == S_OK)
    {
        DXGI_OUTPUT_DESC desc;
        dxgi_output->GetDesc(&desc);

        DDAOutput out;
        out.id = idx;
        out.monitor_x = desc.DesktopCoordinates.left;
        out.monitor_y = desc.DesktopCoordinates.top;
        out.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        out.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        // Create duplication
        IDXGIOutput1 *output1 = nullptr;
        if (SUCCEEDED(dxgi_output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1)))
        {
            HRESULT hr = output1->DuplicateOutput(m_device, &out.dup);
            if (FAILED(hr))
            {
                logf("[dda] DuplicateOutput(%d) failed: 0x%08X, skipping", idx, hr);
            }
            output1->Release();
        }

        // Create staging texture for CPU read
        if (out.dup)
        {
            D3D11_TEXTURE2D_DESC td;
            td.Width = (UINT)out.width;
            td.Height = (UINT)out.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.SampleDesc.Quality = 0;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;

            HRESULT hr2 = m_device->CreateTexture2D(&td, nullptr, &out.staging);
            if (FAILED(hr2))
            {
                logf("[dda] CreateTexture2D staging(%d) failed: 0x%08X", idx, hr2);
                out.dup->Release();
                out.dup = nullptr;
            }
        }

        m_outputs.push_back(out);
        dxgi_output->Release();
        idx++;
    }

    if (m_outputs.empty())
    {
        logf("[dda] no outputs with working duplication");
        return false;
    }

    CreateOutputBuffer();
    return true;
}

bool DDACapture::RecreateDevice()
{
    logf("[dda] RecreateDevice: releasing all DDA resources");
    for (auto &out : m_outputs)
    {
        if (out.staging) { out.staging->Release(); out.staging = nullptr; }
        if (out.dup) { out.dup->Release(); out.dup = nullptr; }
    }
    m_outputs.clear();
    m_buffer.clear();
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_dxgi_device) { m_dxgi_device->Release(); m_dxgi_device = nullptr; }
    if (m_adapter) { m_adapter->Release(); m_adapter = nullptr; }
    if (m_device)
    {
        HRESULT reason = m_device->GetDeviceRemovedReason();
        logf("[dda] device removed reason: 0x%08X", reason);
        m_device->Release();
        m_device = nullptr;
    }

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);
    if (FAILED(hr))
    {
        logf("[dda] RecreateDevice HARDWARE failed: 0x%08X, trying WARP...", hr);
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_device, nullptr, &m_context);
        if (FAILED(hr))
        {
            logf("[dda] RecreateDevice WARP also failed: 0x%08X", hr);
            return false;
        }
    }

    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&m_dxgi_device);
    if (FAILED(hr))
    {
        logf("[dda] RecreateDevice QueryInterface IDXGIDevice failed: 0x%08X", hr);
        m_context->Release(); m_context = nullptr;
        m_device->Release(); m_device = nullptr;
        return false;
    }

    hr = m_dxgi_device->GetAdapter(&m_adapter);
    if (FAILED(hr))
    {
        logf("[dda] RecreateDevice GetAdapter failed: 0x%08X", hr);
        m_dxgi_device->Release(); m_dxgi_device = nullptr;
        m_context->Release(); m_context = nullptr;
        m_device->Release(); m_device = nullptr;
        return false;
    }

    if (!EnumerateOutputs())
    {
        logf("[dda] RecreateDevice EnumerateOutputs failed");
        return false;
    }

    logf("[dda] device recreated: %d outputs, virtual %dx%d",
         (int)m_outputs.size(), m_virtual_w, m_virtual_h);
    return true;
}

void DDACapture::CreateOutputBuffer()
{
    size_t sz = (size_t)m_virtual_w * m_virtual_h * 4;
    m_buffer.assign(sz, 0);
}

bool DDACapture::CaptureFrame(int monitor_id,
    std::vector<uint8_t> &out_bgra, int &out_w, int &out_h)
{
    (void)monitor_id;
    if (!m_initialized)
        return false;

    out_w = m_virtual_w;
    out_h = m_virtual_h;
    if (out_w <= 0 || out_h <= 0)
        return false;

    bool any_data = false;
    bool device_lost = false;

    for (auto &out : m_outputs)
    {
        if (!out.dup)
            continue;

        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = nullptr;
        // Minimal timeout (1ms) to keep DWM composition pipeline active
        HRESULT hr = out.dup->AcquireNextFrame(1, &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            continue;  // No new frame on this output — normal
        }

        if (hr == DXGI_ERROR_ACCESS_LOST)
        {
            logf("[dda] output %d: access lost, will retry", out.id);
            out.dup->ReleaseFrame();
            out.dup->Release();
            out.dup = nullptr;
            if (out.staging) { out.staging->Release(); out.staging = nullptr; }

            IDXGIOutput *dxgi_output = nullptr;
            if (SUCCEEDED(m_adapter->EnumOutputs((UINT)out.id, &dxgi_output)))
            {
                IDXGIOutput1 *output1 = nullptr;
                if (SUCCEEDED(dxgi_output->QueryInterface(
                        __uuidof(IDXGIOutput1), (void **)&output1)))
                {
                    if (SUCCEEDED(output1->DuplicateOutput(m_device, &out.dup)))
                    {
                        D3D11_TEXTURE2D_DESC td{};
                        td.Width = (UINT)out.width;
                        td.Height = (UINT)out.height;
                        td.MipLevels = 1;
                        td.ArraySize = 1;
                        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        td.SampleDesc.Count = 1;
                        td.Usage = D3D11_USAGE_STAGING;
                        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        HRESULT hr2 = m_device->CreateTexture2D(&td, nullptr, &out.staging);
                        if (FAILED(hr2))
                        {
                            logf("[dda] CreateTexture2D staging(%d) after ACCESS_LOST failed: 0x%08X", out.id, hr2);
                            out.dup->Release();
                            out.dup = nullptr;
                        }
                    }
                    output1->Release();
                }
                dxgi_output->Release();
            }
            continue;
        }

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            logf("[dda] output %d: device removed/reset (0x%08X), recreating all", out.id, hr);
            device_lost = true;
            break;
        }

        if (FAILED(hr))
        {
            logf("[dda] output %d AcquireNextFrame error: 0x%08X", out.id, hr);
            continue;
        }

        ID3D11Texture2D *frame_tex = nullptr;
        hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&frame_tex);
        if (SUCCEEDED(hr) && frame_tex)
        {
            // Copy to staging for CPU read
            m_context->CopyResource(out.staging, frame_tex);

            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = m_context->Map(out.staging, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                int dst_pitch = m_virtual_w * 4;

                // Compute buffer-relative position (virtual origin offset)
                int dst_x = out.monitor_x - m_virtual_origin_x;
                int dst_y = out.monitor_y - m_virtual_origin_y;

                // Clamp to virtual desktop, handling negative origin
                int copy_w = out.width;
                int copy_h = out.height;
                int src_off_x = 0;
                int src_off_y = 0;

                if (dst_x < 0) { src_off_x = -dst_x; copy_w += dst_x; dst_x = 0; }
                if (dst_y < 0) { src_off_y = -dst_y; copy_h += dst_y; dst_y = 0; }
                if (copy_w > m_virtual_w - dst_x) copy_w = m_virtual_w - dst_x;
                if (copy_h > m_virtual_h - dst_y) copy_h = m_virtual_h - dst_y;
                if (copy_w <= 0 || copy_h <= 0) { m_context->Unmap(out.staging, 0); continue; }

                uint8_t *dst = m_buffer.data() + (size_t)dst_y * dst_pitch + (size_t)dst_x * 4;
                uint8_t *src = (uint8_t *)mapped.pData
                             + (size_t)src_off_y * mapped.RowPitch
                             + (size_t)src_off_x * 4;
                for (int y = 0; y < copy_h; y++)
                {
                    std::memcpy(dst + (size_t)y * dst_pitch,
                                src + (size_t)y * mapped.RowPitch,
                                (size_t)copy_w * 4);
                }

                m_context->Unmap(out.staging, 0);
                any_data = true;
            }

            frame_tex->Release();
        }

        res->Release();
        out.dup->ReleaseFrame();
    }

    if (device_lost)
    {
        logf("[dda] device lost, recreating all DDA resources");
        if (!RecreateDevice())
        {
            logf("[dda] device recreation failed, marking uninitialized");
            m_initialized = false;
        }
        return false;
    }

    if (!any_data)
        return false;

    out_bgra = m_buffer;
    return true;
}

bool DDACapture::GetMonitorInfo(int monitor_id,
    int &out_w, int &out_h, int &out_x, int &out_y) const
{
    if (monitor_id < 0 || monitor_id >= (int)m_outputs.size())
        return false;
    const auto &out = m_outputs[monitor_id];
    out_w = out.width;
    out_h = out.height;
    out_x = out.monitor_x;
    out_y = out.monitor_y;
    return true;
}

void DDACapture::Shutdown()
{
    for (auto &out : m_outputs)
    {
        if (out.staging)
            out.staging->Release();
        if (out.dup)
            out.dup->Release();
    }
    m_outputs.clear();
    m_buffer.clear();

    if (m_adapter) m_adapter->Release();
    if (m_dxgi_device) m_dxgi_device->Release();
    if (m_context) m_context->Release();
    if (m_device) m_device->Release();

    m_device = nullptr;
    m_context = nullptr;
    m_adapter = nullptr;
    m_dxgi_device = nullptr;
    m_initialized = false;
}
