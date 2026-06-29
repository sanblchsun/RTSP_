#include "capture_wgc.h"

#ifdef GetMonitorInfo
#undef GetMonitorInfo
#endif

#include <cstring>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Foundation;

extern void logf(const char *fmt, ...);

WGCCapture::WGCCapture() {}
WGCCapture::~WGCCapture() { Shutdown(); }


bool WGCCapture::IsWGCSupported()
{
    return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
}

bool WGCCapture::InitInterop()
{
    if (!IsWGCSupported())
    {
        logf("[wgc] WGC не поддерживается, требуется Windows 10 1903+");
        return false;
    }

    try
    {
        CreateD3D11Device();
        //logf("[wgc] Инициализация D3D11 успешна");
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        logf("[wgc] Ошибка D3D11: 0x%08X %hs", e.code().value, e.message().c_str());
        return false;
    }
}

void WGCCapture::CreateD3D11Device()
{
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selectedLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        device.put(),
        &selectedLevel,
        context.put());

    if (FAILED(hr))
        throw winrt::hresult_error(hr, L"Не удалось создать D3D11 устройство");

    m_d3dDevice = device;
    m_d3dContext = context;

    winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();

    winrt::Windows::Foundation::IInspectable temp{ nullptr };
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(temp))));

    m_winrtDevice = temp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}


static bool CreateCaptureItemFromMonitor(HMONITOR hmon, winrt::Windows::Graphics::Capture::GraphicsCaptureItem &out)
{
    try
    {
        auto interop = get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        auto iidItem = winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
        HRESULT hr = interop->CreateForMonitor(hmon, iidItem, reinterpret_cast<void**>(winrt::put_abi(item)));
        if (SUCCEEDED(hr) && item)
        {
            out = item;
            return true;
        }
    }
    catch (const winrt::hresult_error& e)
    {
        logf("[wgc] CreateCaptureItemFromMonitor failed: 0x%08X", e.code().value);
    }
    return false;
}

bool WGCCapture::EnumerateOutputs()
{
    m_outputs.clear();

    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR hmon, HDC, LPRECT r, LPARAM lp) -> BOOL {
            auto *self = (WGCCapture *)lp;
            WGCOutput out;
            out.monitor_x = r->left;
            out.monitor_y = r->top;
            out.width = r->right - r->left;
            out.height = r->bottom - r->top;

            if (!CreateCaptureItemFromMonitor(hmon, out.item))
            {
                return TRUE;
            }
            if (!out.item)
            {
                logf("[wgc] CreateForMonitor returned null for %dx%d @ (%d,%d)",
                     out.width, out.height, out.monitor_x, out.monitor_y);
                return TRUE;
            }

            try {
                out.pool = Direct3D11CaptureFramePool::Create(
                    self->m_winrtDevice,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    out.item.Size());
            } catch (winrt::hresult_error const &e) {
                logf("[wgc] CreateFramePool failed: code=0x%08X msg=%hs",
                     e.code().value, e.message().c_str());
                return TRUE;
            }

            try {
                out.session = out.pool.CreateCaptureSession(out.item);
                out.session.IsCursorCaptureEnabled(false);
                out.session.StartCapture();
            } catch (winrt::hresult_error const &e) {
                logf("[wgc] StartCapture failed: code=0x%08X msg=%hs",
                     e.code().value, e.message().c_str());
                out.pool.Close();
                return TRUE;
            }

            D3D11_TEXTURE2D_DESC td{};
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

            HRESULT hr2 = self->m_d3dDevice->CreateTexture2D(&td, nullptr, &out.staging);
            if (FAILED(hr2))
            {
                logf("[wgc] CreateTexture2D staging failed: 0x%08X", hr2);
                out.session.Close();
                out.pool.Close();
                return TRUE;
            }

            self->m_outputs.push_back(out);
            return TRUE;
        }, (LPARAM)this);

    if (m_outputs.empty())
    {
        logf("[wgc] no usable monitors found");
        return false;
    }

    //logf("[wgc] enumerated %d outputs", (int)m_outputs.size());
    return true;
}

bool WGCCapture::Initialize(int fps)
{
    if (m_initialized)
        return true;

    m_fps = (fps > 0) ? fps : 15;

    try {
        winrt::init_apartment();
    } catch (...) {
        logf("[wgc] init_apartment failed");
        return false;
    }

    if (!InitInterop())
        return false;

    if (!EnumerateOutputs())
    {
        Shutdown();
        return false;
    }

    m_initialized = true;
    //logf("[wgc] initialized: %d outputs", (int)m_outputs.size());
    return true;
}

bool WGCCapture::CaptureFrame(int monitor_id,
    std::vector<uint8_t> &out_bgra, int &out_w, int &out_h)
{
    if (!m_initialized)
        return false;
    if (monitor_id < 0 || monitor_id >= (int)m_outputs.size())
        return false;

    auto &out = m_outputs[monitor_id];
    if (!out.pool)
        return false;

    out_w = out.width;
    out_h = out.height;
    if (out_w <= 0 || out_h <= 0)
        return false;

    // TryGetNextFrame — неблокирующий, возвращает nullptr если нет нового кадра
    Direct3D11CaptureFrame frame{nullptr};
    try {
        frame = out.pool.TryGetNextFrame();
    } catch (...) {
        return false;
    }
    if (!frame)
        return false;

    // Извлекаем ID3D11Texture2D из WinRT-кадра
    auto surface = frame.Surface();
    if (!surface)
        return false;

    ID3D11Texture2D *frame_tex = nullptr;
    try {
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frame_tex)));
    } catch (...) {
        return false;
    }
    if (!frame_tex)
        return false;

    // Копируем в staging для CPU-доступа
    m_d3dContext->CopyResource(out.staging, frame_tex);
    frame_tex->Release();

    // Map + читаем пиксели напрямую в out_bgra (без m_buffer)
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_d3dContext->Map(out.staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
        return false;

    size_t row_size = (size_t)out_w * 4;
    out_bgra.resize((size_t)out_h * out_w * 4);
    uint8_t *src = (uint8_t *)mapped.pData;
    uint8_t *dst = out_bgra.data();
    for (int y = 0; y < out_h; y++)
    {
        std::memcpy(dst + (size_t)y * row_size,
                    src + (size_t)y * mapped.RowPitch,
                    row_size);
    }

    m_d3dContext->Unmap(out.staging, 0);
    return true;
}

bool WGCCapture::GetMonitorInfo(int monitor_id,
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

void WGCCapture::Shutdown()
{
    for (auto &out : m_outputs)
    {
        if (out.session)
        {
            try { out.session.Close(); } catch (...) {}
            out.session = nullptr;
        }
        if (out.pool)
        {
            try { out.pool.Close(); } catch (...) {}
            out.pool = nullptr;
        }
        if (out.item)
            out.item = nullptr;
        if (out.staging)
        {
            out.staging->Release();
            out.staging = nullptr;
        }
    }
    m_outputs.clear();

    m_winrtDevice = nullptr;

    m_d3dContext = nullptr;
    m_d3dDevice = nullptr;

    m_initialized = false;
}
