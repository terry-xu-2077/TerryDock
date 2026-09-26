#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commdlg.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace ld
{

/// Minimal intrusive COM smart pointer.
/// Deliberately dependency free so the project never needs WRL or ATL.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;

    ComPtr(std::nullptr_t) noexcept {}

    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {}

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_)
    {
        if (ptr_)
        {
            ptr_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    ~ComPtr()
    {
        Reset();
    }

    ComPtr& operator=(const ComPtr& other) noexcept
    {
        if (this != std::addressof(other))
        {
            Reset(other.ptr_);
            if (ptr_)
            {
                ptr_->AddRef();
            }
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    ComPtr& operator=(std::nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    void Reset() noexcept
    {
        if (ptr_)
        {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    void Reset(T* ptr) noexcept
    {
        if (ptr_ != ptr)
        {
            Reset();
            ptr_ = ptr;
        }
    }

    T* Get() const noexcept { return ptr_; }

    T* Detach() noexcept
    {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

    T** AddressOf() noexcept
    {
        Reset();
        return &ptr_;
    }

    /// Compatible with IID_PPV_ARGS(&comPtr). Always releases first.
    T** operator&() noexcept { return AddressOf(); }

    T* operator->() const noexcept { return ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }

private:
    T* ptr_ = nullptr;
};

/// HR helper used as `if (FAILED_LD(...))` free code, kept tiny on purpose.
inline bool Failed(HRESULT hr) noexcept { return hr < 0; }
inline bool Succeeded(HRESULT hr) noexcept { return hr >= 0; }

constexpr float kPi = 3.14159265358979323846f;

inline float ClampF(float v, float lo, float hi) noexcept
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

} // namespace ld
