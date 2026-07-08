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
