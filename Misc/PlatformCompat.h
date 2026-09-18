// ============================================================================
//  PlatformCompat.h -- minimal Win32-API shim for non-Windows (Linux) builds.
//
//  Included in place of <windows.h> from each module's StdAfx.h when NOT
//  compiling with MSVC/Windows (see the "#ifdef _WIN32 ... #else ..." guard
//  added around the `#include <windows.h>` line in every StdAfx.h). Provides
//  just enough of the Win32 surface that the engine's base modules (Misc,
//  FileIO, MiscDll, Main) actually call -- not a general Win32 reimplementation.
//
//  This header is expected to grow on demand: as more of the codebase is
//  compiled on Linux, more of its call sites will surface Win32 symbols not
//  yet covered here. Add them here rather than scattering platform #ifdefs
//  through engine code.
// ============================================================================
#ifndef __PLATFORMCOMPAT_H__
#define __PLATFORMCOMPAT_H__
#ifdef _WIN32
#error "PlatformCompat.h is only for non-Windows builds -- windows.h is used directly on Windows."
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

// ----------------------------------------------------------------------------
//  MSVC-only keywords, neutralised on GCC/Clang.
// ----------------------------------------------------------------------------
#define __fastcall
#define __stdcall
#define __cdecl
#define __declspec(x)
#define __debugbreak() __builtin_trap()

// ----------------------------------------------------------------------------
//  Basic Win32 typedefs used throughout the engine's headers.
// ----------------------------------------------------------------------------
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef unsigned char BYTE;
typedef unsigned char byte;
typedef int BOOL;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef void *HANDLE;
typedef DWORD *LPDWORD;
typedef void *LPVOID;
typedef const void *LPCVOID;
struct HWND__;
typedef HWND__ *HWND;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

// ----------------------------------------------------------------------------
//  ASSERT's release-build message box (see each StdAfx.h) -- headless stand-in
//  that logs to stderr instead of popping a dialog.
// ----------------------------------------------------------------------------
#define MB_OK 0
inline int MessageBoxA( HWND, const char *pszText, const char *pszCaption, UINT )
{
	fprintf( stderr, "[%s] %s\n", pszCaption ? pszCaption : "", pszText ? pszText : "" );
	return 0;
}
inline int MessageBox( HWND h, const char *pszText, const char *pszCaption, UINT u )
{
	return MessageBoxA( h, pszText, pszCaption, u );
}
#define OutputDebugString OutputDebugStringA
inline void OutputDebugStringA( const char *psz ) { if ( psz ) fputs( psz, stderr ); }

// ----------------------------------------------------------------------------
//  Timing.
// ----------------------------------------------------------------------------
inline DWORD GetTickCount()
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (DWORD)( (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull );
}
inline void Sleep( DWORD dwMs )
{
	struct timespec ts;
	ts.tv_sec = (time_t)( dwMs / 1000 );
	ts.tv_nsec = (long)( ( dwMs % 1000 ) * 1000000 );
	nanosleep( &ts, 0 );
}

// ----------------------------------------------------------------------------
//  Critical sections -- std::mutex-backed.
// ----------------------------------------------------------------------------
struct CRITICAL_SECTION { std::mutex m; };
inline void InitializeCriticalSection( CRITICAL_SECTION * ) {}
inline void DeleteCriticalSection( CRITICAL_SECTION * ) {}
inline void EnterCriticalSection( CRITICAL_SECTION *cs ) { cs->m.lock(); }
inline void LeaveCriticalSection( CRITICAL_SECTION *cs ) { cs->m.unlock(); }

// ----------------------------------------------------------------------------
//  Threads and interlocked ops -- declared here, implemented in
//  PlatformCompat.cpp (compiled only on the Linux build; see CMakeLists.txt).
// ----------------------------------------------------------------------------
typedef DWORD ( *LPTHREAD_START_ROUTINE )( LPVOID );
#define WAIT_OBJECT_0 0u
#define INFINITE 0xFFFFFFFFu

HANDLE CreateThread( void *pSecAttr, size_t nStackSize, LPTHREAD_START_ROUTINE pStartRoutine,
                     LPVOID pParam, DWORD dwFlags, LPDWORD pThreadId );
DWORD WaitForSingleObject( HANDLE hHandle, DWORD dwMilliseconds );
BOOL CloseHandle( HANDLE hObject );

inline LONG InterlockedIncrement( volatile LONG *p ) { return __sync_add_and_fetch( p, 1 ); }
inline LONG InterlockedDecrement( volatile LONG *p ) { return __sync_sub_and_fetch( p, 1 ); }

#endif // __PLATFORMCOMPAT_H__
