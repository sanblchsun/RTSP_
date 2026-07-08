#pragma once
#include <unknwn.h>
#include <guiddef.h>
#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>

namespace Windows::Graphics::DirectX::Direct3D11
{

struct IDirect3DDxgiInterfaceAccess : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(
        REFIID riid,
        void **ppv) = 0;
};

}

namespace winrt::impl
{
    template <>
    inline constexpr guid guid_v<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
        { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };
}

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice *dxgiDevice,
    IInspectable **graphicsDevice);
