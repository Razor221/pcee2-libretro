// Bits of the Windows SDK that WIL uses and mingw-w64 does not ship.
// Included from the WIL headers that need them; a no-op everywhere else.
#ifndef __WIL_MINGW_COMPAT_INCLUDED
#define __WIL_MINGW_COMPAT_INCLUDED

#if defined(__MINGW32__)

#include <windows.h>
#include <winerror.h>

// Win32 error codes missing from mingw-w64's winerror.h. Values taken from the
// Windows SDK and cross-checked against Wine's winerror.h.
#ifndef ERROR_UNHANDLED_EXCEPTION
#define ERROR_UNHANDLED_EXCEPTION 574L
#endif
#ifndef ERROR_ILLEGAL_CHARACTER
#define ERROR_ILLEGAL_CHARACTER 582L
#endif
#ifndef ERROR_UNDEFINED_CHARACTER
#define ERROR_UNDEFINED_CHARACTER 583L
#endif
#ifndef ERROR_NO_MORE_MATCHES
#define ERROR_NO_MORE_MATCHES 626L
#endif
#ifndef ERROR_ASSERTION_FAILURE
#define ERROR_ASSERTION_FAILURE 668L
#endif
#ifndef ERROR_IMPLEMENTATION_LIMIT
#define ERROR_IMPLEMENTATION_LIMIT 1292L
#endif

#ifndef E_NOT_SET
#define E_NOT_SET HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

// WinRT agile references - mingw-w64 declares no AgileReferenceOptions. WIL only
// uses it as a default argument of the com_agile_* helpers.
#ifndef __WIL_MINGW_AGILEREFERENCE_OPTIONS
#define __WIL_MINGW_AGILEREFERENCE_OPTIONS
typedef enum AgileReferenceOptions
{
    AGILEREFERENCE_DEFAULT = 0,
    AGILEREFERENCE_DELAYEDMARSHAL = 1
} AgileReferenceOptions;
#endif

// __fastfail() is an MSVC intrinsic. Emit the instruction it compiles to, so the
// failfast paths keep terminating the process rather than returning.
#ifndef __WIL_MINGW_FASTFAIL
#define __WIL_MINGW_FASTFAIL
__attribute__((noreturn)) inline void __fastfail(unsigned int code)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("int $0x29" : : "c"(code));
#else
    (void)code;
#endif
    __builtin_trap();
}
#endif

#endif // __MINGW32__

#endif // __WIL_MINGW_COMPAT_INCLUDED
