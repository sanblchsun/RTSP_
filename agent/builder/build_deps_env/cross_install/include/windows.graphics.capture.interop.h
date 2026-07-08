#pragma once
#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>
#include <winrt/base.h>

struct IGraphicsCaptureItemInterop : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(
        HWND hwnd,
        REFIID riid,
        void **result) = 0;

    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(
        HMONITOR monitor,
        REFIID riid,
        void **result) = 0;
};

namespace winrt::impl
{
    template <>
    inline constexpr guid guid_v<IGraphicsCaptureItemInterop>
        { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };
}
