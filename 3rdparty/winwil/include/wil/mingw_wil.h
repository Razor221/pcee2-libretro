// Minimal stand-in for the parts of WIL this tree uses, for MinGW only.
//
// WIL supports MSVC and clang; its README lists no GCC, and its unique_any_t
// machinery does not compile there (function pointers as non-type template
// parameters are rejected as non-structural, RoGetAgileReference is missing,
// and so on). Rather than fight that, MinGW gets these small equivalents of the
// handful of types the tree actually names: com_ptr_nothrow,
// CoCreateInstanceNoThrow, unique_any, unique_hfile, unique_hkey,
// unique_variant, unique_cotaskmem_string and unique_couninitialize_call.
#ifndef __WIL_MINGW_WIL_INCLUDED
#define __WIL_MINGW_WIL_INCLUDED

#include <combaseapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <winreg.h>
#include <utility>

namespace wil
{
//! Owning COM pointer. get() returns the raw pointer, put() returns an address
//! to receive a new one (releasing whatever was held first), as in WIL.
template <typename T>
class com_ptr_nothrow
{
public:
    com_ptr_nothrow() = default;
    com_ptr_nothrow(std::nullptr_t) {}
    com_ptr_nothrow(T* p) : m_ptr(p)
    {
        if (m_ptr)
            m_ptr->AddRef();
    }
    com_ptr_nothrow(const com_ptr_nothrow& o) : com_ptr_nothrow(o.m_ptr) {}
    com_ptr_nothrow(com_ptr_nothrow&& o) noexcept : m_ptr(o.m_ptr) { o.m_ptr = nullptr; }

    ~com_ptr_nothrow() { reset(); }

    com_ptr_nothrow& operator=(const com_ptr_nothrow& o)
    {
        if (this != &o)
        {
            reset();
            m_ptr = o.m_ptr;
            if (m_ptr)
                m_ptr->AddRef();
        }
        return *this;
    }
    com_ptr_nothrow& operator=(com_ptr_nothrow&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            m_ptr = o.m_ptr;
            o.m_ptr = nullptr;
        }
        return *this;
    }

    void reset()
    {
        if (m_ptr)
        {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    //! Releases the current pointer and hands out a slot for a new one.
    T** put()
    {
        reset();
        return &m_ptr;
    }
    T** operator&() { return put(); }

    T* release()
    {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    //! Address of the held pointer, without releasing it - for APIs that read
    //! an array of interface pointers.
    T* const* addressof() const { return &m_ptr; }
    T** addressof() { return &m_ptr; }

    //! QueryInterface, returning an empty pointer when the interface is absent.
    template <typename U>
    com_ptr_nothrow<U> try_query() const
    {
        com_ptr_nothrow<U> result;
        if (m_ptr)
        {
            U* raw = nullptr;
            if (SUCCEEDED(m_ptr->QueryInterface(__uuidof(U), reinterpret_cast<void**>(&raw))) && raw)
            {
                result.attach(raw);
            }
        }
        return result;
    }

    template <typename U>
    HRESULT query_to(U** out) const
    {
        return m_ptr ? m_ptr->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out)) : E_POINTER;
    }

    HRESULT copy_to(T** out) const
    {
        if (!out)
            return E_POINTER;
        *out = m_ptr;
        if (m_ptr)
            m_ptr->AddRef();
        return S_OK;
    }

    //! Takes ownership of an already-referenced pointer.
    void attach(T* p)
    {
        reset();
        m_ptr = p;
    }

private:
    T* m_ptr = nullptr;
};

template <typename T>
inline com_ptr_nothrow<T> CoCreateInstanceNoThrow(REFCLSID clsid, DWORD context = CLSCTX_INPROC_SERVER)
{
    com_ptr_nothrow<T> result;
    if (FAILED(CoCreateInstance(clsid, nullptr, context, __uuidof(T), reinterpret_cast<void**>(result.put()))))
        result.reset();
    return result;
}

//! Owns a value released by a free function, e.g.
//! unique_any<GUID*, decltype(&::LocalFree), ::LocalFree>.
template <typename T, typename TFunc, TFunc func>
class unique_any
{
public:
    unique_any() = default;
    explicit unique_any(T value) : m_value(value) {}
    unique_any(const unique_any&) = delete;
    unique_any& operator=(const unique_any&) = delete;
    unique_any(unique_any&& o) noexcept : m_value(o.m_value) { o.m_value = T{}; }

    ~unique_any() { reset(); }

    void reset()
    {
        if (m_value)
        {
            func(m_value);
            m_value = T{};
        }
    }

    T get() const { return m_value; }
    T* put()
    {
        reset();
        return &m_value;
    }
    T* operator&() { return put(); }
    explicit operator bool() const { return m_value != T{}; }

    T release()
    {
        T v = m_value;
        m_value = T{};
        return v;
    }

private:
    T m_value{};
};

namespace details
{
    inline void close_handle(HANDLE h)
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    inline void close_hkey(HKEY h) { RegCloseKey(h); }
    inline void free_cotaskmem_string(PWSTR p) { CoTaskMemFree(p); }
} // namespace details

using unique_hfile = unique_any<HANDLE, decltype(&details::close_handle), details::close_handle>;
using unique_hkey = unique_any<HKEY, decltype(&details::close_hkey), details::close_hkey>;
using unique_cotaskmem_string =
    unique_any<PWSTR, decltype(&details::free_cotaskmem_string), details::free_cotaskmem_string>;

//! VARIANT that clears itself; usable directly as a VARIANT, as WIL's is.
class unique_variant : public VARIANT
{
public:
    unique_variant() { VariantInit(this); }
    unique_variant(const unique_variant&) = delete;
    unique_variant& operator=(const unique_variant&) = delete;
    ~unique_variant() { VariantClear(this); }
};

//! Calls CoUninitialize() on scope exit.
class unique_couninitialize_call
{
public:
    unique_couninitialize_call() = default;
    unique_couninitialize_call(const unique_couninitialize_call&) = delete;
    unique_couninitialize_call& operator=(const unique_couninitialize_call&) = delete;
    ~unique_couninitialize_call()
    {
        if (m_armed)
            CoUninitialize();
    }
    void release() { m_armed = false; }

private:
    bool m_armed = true;
};
} // namespace wil

#endif // __WIL_MINGW_WIL_INCLUDED
