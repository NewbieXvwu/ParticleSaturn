#pragma once

// Minimal ATL shim for projects that use DiligentCore without installing
// the Visual Studio "C++ ATL" workload.
//
// DiligentCore uses <atlbase.h> primarily for CComPtr.
// This header provides a small CComPtr implementation that supports:
// - default construction, copy/move
// - operator->, implicit T* conversion
// - operator& (commonly used with IID_PPV_ARGS / CoCreateInstance)
//
// Note: This is NOT a full ATL replacement.

#include <Unknwn.h> // IUnknown
#include <utility>

template <class T>
class CComPtr
{
public:
    using InterfaceType = T;

    CComPtr() noexcept = default;

    CComPtr(std::nullptr_t) noexcept {}

    explicit CComPtr(T* ptr) noexcept :
        m_Ptr{ptr}
    {
        InternalAddRef();
    }

    CComPtr(const CComPtr& other) noexcept :
        m_Ptr{other.m_Ptr}
    {
        InternalAddRef();
    }

    CComPtr(CComPtr&& other) noexcept :
        m_Ptr{other.m_Ptr}
    {
        other.m_Ptr = nullptr;
    }

    ~CComPtr()
    {
        InternalRelease();
    }

    CComPtr& operator=(const CComPtr& other) noexcept
    {
        if (this != &other)
        {
            InternalRelease();
            m_Ptr = other.m_Ptr;
            InternalAddRef();
        }
        return *this;
    }

    CComPtr& operator=(CComPtr&& other) noexcept
    {
        if (this != &other)
        {
            InternalRelease();
            m_Ptr       = other.m_Ptr;
            other.m_Ptr = nullptr;
        }
        return *this;
    }

    CComPtr& operator=(T* ptr) noexcept
    {
        if (m_Ptr != ptr)
        {
            InternalRelease();
            m_Ptr = ptr;
            InternalAddRef();
        }
        return *this;
    }

    void Release() noexcept
    {
        InternalRelease();
    }

    void Attach(T* ptr) noexcept
    {
        InternalRelease();
        m_Ptr = ptr;
    }

    T* Detach() noexcept
    {
        T* ptr = m_Ptr;
        m_Ptr  = nullptr;
        return ptr;
    }

    T* Get() const noexcept { return m_Ptr; }

    operator T*() const noexcept { return m_Ptr; }

    T* operator->() const noexcept { return m_Ptr; }

    T** operator&() noexcept
    {
        // ATL's CComPtr asserts that the pointer is null before returning &p.
        // To keep common patterns working, we proactively release to avoid leaks.
        InternalRelease();
        return &m_Ptr;
    }

private:
    void InternalAddRef() noexcept
    {
        if (m_Ptr != nullptr)
            m_Ptr->AddRef();
    }

    void InternalRelease() noexcept
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->Release();
            m_Ptr = nullptr;
        }
    }

    T* m_Ptr = nullptr;
};

