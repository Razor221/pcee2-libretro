// Force-included into every translation unit of the MinGW build (see
// cmake/BuildParameters.cmake).
//
// mingw-w64's headers declare a handful of one-line helpers - GetCurrentFiber(),
// NtCurrentTeb(), the Tp* and SH* families - extern and then define them as
// FORCEINLINE, which resolves to Pcsx2Defs.h's __forceinline. That macro omits
// the inline keyword on purpose (__forceinline_odr is the variant that has it),
// so in C an inline definition of an already-extern function is an external
// definition and every object ended up carrying one: thousands of "multiple
// definition" errors at link time.
//
// Defining FORCEINLINE before any Windows header is reached wins because
// winnt.h guards it with #ifndef. Weak rather than static: the extern
// declaration that precedes these definitions rules static out.
#ifndef FORCEINLINE
#define FORCEINLINE __inline__ __attribute__((__weak__))
#endif
