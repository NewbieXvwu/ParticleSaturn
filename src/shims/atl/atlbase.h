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
#include <guiddef.h>
#include <memory>
#include <utility>

template <class T> class CComPtr {
  public:
    using InterfaceType = T;

    CComPtr() noexcept = default;

    CComPtr(std::nullptr_t) noexcept {}

    explicit CComPtr(T* ptr) noexcept : p{ptr} { InternalAddRef(); }

    CComPtr(const CComPtr& other) noexcept : p{other.p} { InternalAddRef(); }

    CComPtr(CComPtr&& other) noexcept : p{other.p} { other.p = nullptr; }

    ~CComPtr() { InternalRelease(); }

    CComPtr& operator=(const CComPtr& other) noexcept {
        if (this != std::addressof(other)) {
            InternalRelease();
            p = other.p;
            InternalAddRef();
        }
        return *this;
    }

    CComPtr& operator=(CComPtr&& other) noexcept {
        if (this != std::addressof(other)) {
            InternalRelease();
            p       = other.p;
            other.p = nullptr;
        }
        return *this;
    }

    CComPtr& operator=(T* ptr) noexcept {
        if (p != ptr) {
            InternalRelease();
            p = ptr;
            InternalAddRef();
        }
        return *this;
    }

    void Release() noexcept { InternalRelease(); }

    void Attach(T* ptr) noexcept {
        InternalRelease();
        p = ptr;
    }

    T* Detach() noexcept {
        T* ptr = p;
        p      = nullptr;
        return ptr;
    }

    T* Get() const noexcept { return p; }

    operator T*() const noexcept { return p; }

    T* operator->() const noexcept { return p; }

    T** operator&() noexcept {
        // ATL's CComPtr asserts that the pointer is null before returning &p.
        // To keep common patterns working, we proactively release to avoid leaks.
        InternalRelease();
        return &p;
    }

    // ATL exposes the raw pointer as a public member named 'p'.
    // DiligentCore relies on this in a few places (e.g. passing pLog.p).
    T* p = nullptr;

    template <class Q> HRESULT QueryInterface(Q** pp) const noexcept {
        if (pp == nullptr) {
            return E_POINTER;
        }

        *pp = nullptr;
        if (p == nullptr) {
            return E_NOINTERFACE;
        }

        return p->QueryInterface(__uuidof(Q), reinterpret_cast<void**>(pp));
    }

  private:
    void InternalAddRef() noexcept {
        if (p != nullptr) {
            p->AddRef();
        }
    }

    void InternalRelease() noexcept {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
};
