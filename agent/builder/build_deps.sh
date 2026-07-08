#!/usr/bin/env bash
# ============================================================
# build_deps.sh — кросс-компиляция зависимостей агента RTSP_
# для Windows x86_64. Собирает:
#   - libx264 (кодирование CPU)
#   - C++/WinRT headers (для WGC capture)
#
# Никакого ffmpeg — только статические библиотеки и заголовки.
#
# Запуск:  sudo bash build_deps.sh
#   или:   sudo bash build_deps.sh /путь/к/проекту
#
# Зависимости (apt): build-essential mingw-w64 git curl cmake nasm
# ============================================================

set -euo pipefail

# ─── Конфигурация ───────────────────────────────────────────
X264_REVISION="stable"
JOBS=${JOBS:-$(nproc)}

# ─── Пути ───────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${1:-$SCRIPT_DIR/..}"
BUILD_DIR="$SCRIPT_DIR/build_deps_env"
PREFIX="$BUILD_DIR/cross_install"

SOURCE_DIR="$BUILD_DIR/sources"
X264_DIR="$SOURCE_DIR/x264"

TOOLCHAIN_PREFIX="x86_64-w64-mingw32-"

# ─── Цветной вывод ──────────────────────────────────────────
info()  { echo -e "\033[1;34m[INFO]\033[0m $*"; }
ok()    { echo -e "\033[1;32m[OK]\033[0m   $*"; }
err()   { echo -e "\033[1;31m[ERROR]\033[0m $*"; }

# ─── Проверка зависимостей ──────────────────────────────────
check_prereqs() {
    local missing=()
    for cmd in curl git make gcc g++ "${TOOLCHAIN_PREFIX}gcc" "${TOOLCHAIN_PREFIX}g++" \
               "${TOOLCHAIN_PREFIX}ar" "${TOOLCHAIN_PREFIX}strip" pkg-config cmake 7z; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "Отсутствуют зависимости: ${missing[*]}"
        info "Установите:  sudo apt install build-essential mingw-w64 git curl pkg-config cmake nasm"
        exit 1
    fi

    local mgw_ver
    mgw_ver=$("${TOOLCHAIN_PREFIX}gcc" -dumpversion 2>/dev/null || true)
    info "MinGW-w64 gcc версия: $mgw_ver"
}

# ─── Подготовка директорий ──────────────────────────────────
setup_dirs() {
    mkdir -p "$SOURCE_DIR" "$PREFIX/lib" "$PREFIX/include"
}

# ─── Загрузка исходников ────────────────────────────────────
download_sources() {
    if [[ ! -d "$X264_DIR" ]]; then
        info "Клонирую x264 ($X264_REVISION) ..."
        git clone --depth=1 --branch "$X264_REVISION" \
            https://code.videolan.org/videolan/x264.git "$X264_DIR"
        ok "x264"
    else
        info "x264 уже скачан в $X264_DIR"
    fi
}

# ─── Сборка libx264 ─────────────────────────────────────────
build_x264() {
    info "Собираю libx264 ..."

    if [[ -f "$PREFIX/lib/libx264.a" ]]; then
        info "libx264 уже собран, пропускаю"
        return
    fi

    pushd "$X264_DIR" >/dev/null

    make clean 2>/dev/null || true

    ./configure \
        --cross-prefix="$TOOLCHAIN_PREFIX" \
        --host="x86_64-w64-mingw32" \
        --enable-static \
        --disable-cli \
        --disable-lavf \
        --disable-swscale \
        --extra-cflags="-D_WIN32_WINNT=0x0602 -Os" \
        --prefix="$PREFIX"

    make -j"$JOBS"

    cp libx264.a "$PREFIX/lib/"
    cp x264.h x264_config.h "$PREFIX/include/"

    popd >/dev/null
    ok "libx264 собран: $PREFIX/lib/libx264.a"
}

# ─── Установка C++/WinRT SDK заголовков ─────────────────────
# Собирает cppwinrt из исходников (нативный Linux), скачивает
# Microsoft.Windows.SDK.Contracts (winmd), генерирует winrt/ заголовки.
install_winsdk() {
    local winrt_dir="$PREFIX/include/winrt"
    if [[ -f "$winrt_dir/Windows.Graphics.Capture.h" ]]; then
        info "C++/WinRT headers already installed, skipping"
        return
    fi

    info "Building cppwinrt natively for Linux..."

    local cppwinrt_src="$BUILD_DIR/cppwinrt-src"
    local cppwinrt_bin="$cppwinrt_src/build/cppwinrt"
    local winmd_dir="$BUILD_DIR/winsdk-contracts"

    # Clone cppwinrt if needed
    if [[ ! -d "$cppwinrt_src" ]]; then
        git clone --depth=1 https://github.com/microsoft/cppwinrt.git "$cppwinrt_src"
    fi

    # Build cppwinrt natively (cmake + g++)
    if [[ ! -f "$cppwinrt_bin" ]]; then
        mkdir -p "$cppwinrt_src/build"
        cmake -B "$cppwinrt_src/build" -S "$cppwinrt_src" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCPPWINRT_BUILD_VERSION=2.0.240405.15
        cmake --build "$cppwinrt_src/build" -j"$JOBS"
    fi

    # Download SDK.Contracts NuGet package
    if [[ ! -d "$winmd_dir" ]]; then
        info "Downloading Microsoft.Windows.SDK.Contracts..."
        mkdir -p "$winmd_dir"
        curl -sL -o "$winmd_dir/contracts.nupkg" \
            "https://www.nuget.org/api/v2/package/Microsoft.Windows.SDK.Contracts/10.0.19041.1"
        (cd "$winmd_dir" && 7z x contracts.nupkg -ocontracts -y > /dev/null)
    fi

    # Generate C++/WinRT headers from contract winmd files
    info "Generating C++/WinRT headers..."
    rm -rf "$winrt_dir"
    mkdir -p "$winrt_dir"
    "$cppwinrt_bin" -input "$winmd_dir/contracts/ref/netstandard2.0" -output "$winrt_dir"

    # Generate Numerics fixup header (MinGW lacks windowsnumerics.impl.h)
    cat > "$winrt_dir/winrt/wgc_numerics_fixup.h" << 'SHIMEOF'
#pragma once
#define WINRT_IMPL_IUNKNOWN_DEFINED
#include <guiddef.h>
#include <winrt/base.h>

namespace winrt::Windows::Foundation::Numerics
{
    struct float2 { float x, y; float2() noexcept = default; constexpr float2(float x, float y) noexcept : x(x), y(y) {} };
    struct float3 { float x, y, z; float3() noexcept = default; constexpr float3(float x, float y, float z) noexcept : x(x), y(y), z(z) {} };
    struct float4 { float x, y, z, w; float4() noexcept = default; constexpr float4(float x, float y, float z, float w) noexcept : x(x), y(y), z(z), w(w) {} };
    struct float3x2 { float m11, m12, m21, m22, m31, m32; float3x2() noexcept = default; };
    struct float4x4 { float m11,m12,m13,m14,m21,m22,m23,m24,m31,m32,m33,m34,m41,m42,m43,m44; float4x4() noexcept = default; };
    struct quaternion { float x, y, z, w; quaternion() noexcept = default; constexpr quaternion(float x, float y, float z, float w) noexcept : x(x), y(y), z(z), w(w) {} };
    struct plane { float3 normal; float d; plane() noexcept = default; constexpr plane(float3 const& n, float d) noexcept : normal(n), d(d) {} };
}

namespace winrt::impl
{
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::float2> = L"Windows.Foundation.Numerics.Vector2";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::float3> = L"Windows.Foundation.Numerics.Vector3";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::float4> = L"Windows.Foundation.Numerics.Vector4";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::float3x2> = L"Windows.Foundation.Numerics.Matrix3x2";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::float4x4> = L"Windows.Foundation.Numerics.Matrix4x4";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::quaternion> = L"Windows.Foundation.Numerics.Quaternion";
    template <> inline constexpr auto& name_v<Windows::Foundation::Numerics::plane> = L"Windows.Foundation.Numerics.Plane";

    template <> struct category<Windows::Foundation::Numerics::float2> { using type = struct_category<float, float>; };
    template <> struct category<Windows::Foundation::Numerics::float3> { using type = struct_category<float, float, float>; };
    template <> struct category<Windows::Foundation::Numerics::float4> { using type = struct_category<float, float, float, float>; };
    template <> struct category<Windows::Foundation::Numerics::float3x2> { using type = struct_category<float, float, float, float, float, float>; };
    template <> struct category<Windows::Foundation::Numerics::float4x4>
    {
        using type = struct_category<float,float,float,float,float,float,float,float,float,float,float,float,float,float,float,float>;
    };
    template <> struct category<Windows::Foundation::Numerics::quaternion> { using type = struct_category<float, float, float, float>; };
    template <> struct category<Windows::Foundation::Numerics::plane> { using type = struct_category<Windows::Foundation::Numerics::float3, float>; };

    template <> struct abi<Windows::Foundation::Numerics::float2> { struct type { float x, y; }; };
    template <> struct abi<Windows::Foundation::Numerics::float3> { struct type { float x, y, z; }; };
    template <> struct abi<Windows::Foundation::Numerics::float4> { struct type { float x, y, z, w; }; };
    template <> struct abi<Windows::Foundation::Numerics::float3x2> { struct type { float m11, m12, m21, m22, m31, m32; }; };
    template <> struct abi<Windows::Foundation::Numerics::float4x4> { struct type { float m[16]; }; };
    template <> struct abi<Windows::Foundation::Numerics::quaternion> { struct type { float x, y, z, w; }; };
    template <> struct abi<Windows::Foundation::Numerics::plane> { struct type { Windows::Foundation::Numerics::float3 n; float d; }; };
}
SHIMEOF
    info "  Numerics fixup header generated"

    # Generate WGC interop headers (part of Windows SDK, not generated by cppwinrt)
    cat > "$PREFIX/include/windows.graphics.capture.interop.h" << 'HEADEREOF'
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
HEADEREOF
    info "  windows.graphics.capture.interop.h generated"

    cat > "$PREFIX/include/windows.graphics.directx.direct3d11.interop.h" << 'HEADEREOF'
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
HEADEREOF
    info "  windows.graphics.directx.direct3d11.interop.h generated"

    # Verify key headers exist
    if [[ -f "$winrt_dir/winrt/Windows.Graphics.Capture.h" ]]; then
        ok "C++/WinRT SDK headers generated: $winrt_dir"
        local h_count
        h_count=$(find "$winrt_dir" -name "*.h" | wc -l)
        local h_size
        h_size=$(du -sh "$winrt_dir" | cut -f1)
        info "  Headers: $h_count, Size: $h_size"
    else
        err "Windows.Graphics.Capture.h not generated — WGC build will fail"
        return 1
    fi
}

# ─── Очистка ────────────────────────────────────────────────
clean() {
    info "Очистка сборочного окружения ..."
    rm -rf "$BUILD_DIR"
    ok "Очищено: $BUILD_DIR"
}

# ─── Точка входа ────────────────────────────────────────────
main() {
    echo ""
    echo "╔══════════════════════════════════════════════════╗"
    echo "║  RTSP_ Agent dependency cross-compiler: Windows ║"
    echo "║  (libx264 + WinRT SDK)                          ║"
    echo "╚══════════════════════════════════════════════════╝"
    echo "  Потоки сборки:  $JOBS"
    echo "  Директория:     $PROJECT_DIR"
    echo ""

    check_prereqs
    setup_dirs
    download_sources
    build_x264
    install_winsdk

    echo ""
    ok "Сборка зависимостей завершена!"
    echo "  Заголовки: $PREFIX/include/"
    echo "  Библиотеки: $PREFIX/lib/"
    echo ""
}

main "$@"