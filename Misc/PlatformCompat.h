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
#include <cstdarg>
#include <cwchar>
#include <ctime>
#include <cerrno>
#include <mutex>
#include <unistd.h>
#include <sys/mman.h>

// ----------------------------------------------------------------------------
//  MSVC-only keywords, neutralised on GCC/Clang.
// ----------------------------------------------------------------------------
#define __fastcall
#define __stdcall
#define __cdecl
#define __declspec(x)
#define __debugbreak() __builtin_trap()
#define __forceinline inline __attribute__(( always_inline ))

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
struct HINSTANCE__;
typedef HINSTANCE__ *HINSTANCE;

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
//  Code pages and charset conversion.
//
//  The engine only ever uses the "ANSI" code page (game data and the editor
//  UI are CP1251 -- the retail Windows build ran with a Russian system locale,
//  so CP_ACP *was* CP1251) plus UTF-8 for the log stream. Conversion itself is
//  implemented in PlatformCompat.cpp via iconv(3).
// ----------------------------------------------------------------------------
#define CP_ACP 0
#define CP_UTF8 65001

int WideCharToMultiByte( UINT nCodePage, DWORD dwFlags, const wchar_t *pWide, int nWideLen,
                         char *pDst, int nDstBytes, const char *pszDefaultChar, BOOL *pbUsedDefault );
int MultiByteToWideChar( UINT nCodePage, DWORD dwFlags, const char *pSrc, int nSrcLen,
                         wchar_t *pDst, int nDstChars );

// ----------------------------------------------------------------------------
//  MSVC's non-standard wide-string helpers.
//
//  swprintf()/vswprintf() are declared by <cwchar> with the C99 signature that
//  takes a destination size; MSVC's historical overloads omit it. The engine
//  calls the MSVC form in ~90 places, so provide it as an overload rather than
//  touching every call site. The destination bound is unknown here -- exactly
//  as it was under MSVC -- so a generous cap is used; every current call site
//  writes into a >= 1024-wchar buffer.
// ----------------------------------------------------------------------------
#define PLATFORMCOMPAT_SWPRINTF_CAP 4096

inline int vswprintf( wchar_t *pBuf, const wchar_t *pszFormat, va_list va )
{
	return vswprintf( pBuf, PLATFORMCOMPAT_SWPRINTF_CAP, pszFormat, va );
}
inline int swprintf( wchar_t *pBuf, const wchar_t *pszFormat, ... )
{
	va_list va;
	va_start( va, pszFormat );
	int nRes = vswprintf( pBuf, PLATFORMCOMPAT_SWPRINTF_CAP, pszFormat, va );
	va_end( va );
	return nRes;
}
// MSVC's _itow( value, buffer, radix ); only radix 10 is ever used by the engine.
inline wchar_t *_itow( int nValue, wchar_t *pBuf, int nRadix )
{
	if ( nRadix == 10 )
		swprintf( pBuf, PLATFORMCOMPAT_SWPRINTF_CAP, L"%d", nValue );
	else
	{
		char szBuf[64];
		const char *pszDigits = "0123456789abcdefghijklmnopqrstuvwxyz";
		unsigned int nAbs = nValue < 0 ? (unsigned int)( -(long long)nValue ) : (unsigned int)nValue;
		int n = 0;
		do { szBuf[n++] = pszDigits[ nAbs % (unsigned)nRadix ]; nAbs /= (unsigned)nRadix; } while ( nAbs );
		if ( nValue < 0 ) szBuf[n++] = '-';
		for ( int i = 0; i < n; ++i )
			pBuf[i] = (wchar_t)szBuf[ n - 1 - i ];
		pBuf[n] = 0;
	}
	return pBuf;
}

// ----------------------------------------------------------------------------
//  Directory enumeration (FindFirstFile / FindNextFile / FindClose).
//
//  Backed by opendir/readdir + fnmatch in PlatformCompat.cpp. The engine only
//  ever passes a "<dir>\<mask>" pattern and reads cFileName, dwFileAttributes,
//  nFileSizeLow and ftLastWriteTime, so only those fields are filled in.
//  Backslashes in the pattern are accepted and treated as '/'.
//
//  NOTE: the Win32 API is case-insensitive while Linux filesystems are not;
//  matching here is case-insensitive (FNM_CASEFOLD) to keep asset lookups
//  working against the original, mixed-case game data.
// ----------------------------------------------------------------------------
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
typedef FILETIME _FILETIME;

#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_SYSTEM    0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL    0x00000080

struct WIN32_FIND_DATAA
{
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	char cFileName[MAX_PATH];
};
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;

HANDLE FindFirstFileA( const char *pszPattern, WIN32_FIND_DATAA *pFindData );
BOOL FindNextFileA( HANDLE hFind, WIN32_FIND_DATAA *pFindData );
BOOL FindClose( HANDLE hFind );
inline HANDLE FindFirstFile( const char *pszPattern, WIN32_FIND_DATAA *pFindData ) { return FindFirstFileA( pszPattern, pFindData ); }
inline BOOL FindNextFile( HANDLE hFind, WIN32_FIND_DATAA *pFindData ) { return FindNextFileA( hFind, pFindData ); }

// MSVC's itoa( value, buffer, radix ) -- the narrow twin of _itow above.
inline char *itoa( int nValue, char *pBuf, int nRadix )
{
	if ( nRadix == 10 )
		sprintf( pBuf, "%d", nValue );
	else
	{
		const char *pszDigits = "0123456789abcdefghijklmnopqrstuvwxyz";
		unsigned int nAbs = nValue < 0 ? (unsigned int)( -(long long)nValue ) : (unsigned int)nValue;
		char szTmp[64];
		int n = 0;
		do { szTmp[n++] = pszDigits[ nAbs % (unsigned)nRadix ]; nAbs /= (unsigned)nRadix; } while ( nAbs );
		if ( nValue < 0 ) szTmp[n++] = '-';
		for ( int i = 0; i < n; ++i )
			pBuf[i] = szTmp[ n - 1 - i ];
		pBuf[n] = 0;
	}
	return pBuf;
}

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

// ----------------------------------------------------------------------------
//  High-resolution timer (Misc/HPTimer.cpp's calibration window) -- the engine
//  only ever treats _LARGE_INTEGER as an opaque 64-bit counter, so a plain
//  int64 typedef keeps HPTimer.cpp's "(_LARGE_INTEGER*)&int64var" casts working
//  unmodified. Backed by CLOCK_MONOTONIC at nanosecond resolution.
// ----------------------------------------------------------------------------
typedef int64_t _LARGE_INTEGER;
inline void QueryPerformanceCounter( _LARGE_INTEGER *p )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	*p = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
inline void QueryPerformanceFrequency( _LARGE_INTEGER *p ) { *p = 1000000000ll; }

// ----------------------------------------------------------------------------
//  IsBadReadPtr -- best-effort "is this pointer readable" probe (used only to
//  skip releasing an already-freed object during save/load teardown -- see
//  Misc/Basic2.cpp's UafCheck). msync() fails ENOMEM on an unmapped page
//  without risking a real fault, which is the same "probe, don't crash"
//  contract the Win32 call has.
// ----------------------------------------------------------------------------
inline bool IsBadReadPtr( const void *ptr, size_t size )
{
	if ( !ptr || size == 0 )
		return ptr == 0;
	long nPageSize = sysconf( _SC_PAGESIZE );
	uintptr_t start = (uintptr_t)ptr & ~(uintptr_t)( nPageSize - 1 );
	uintptr_t end = ( (uintptr_t)ptr + size + nPageSize - 1 ) & ~(uintptr_t)( nPageSize - 1 );
	for ( uintptr_t p = start; p < end; p += nPageSize )
	{
		errno = 0;
		if ( msync( (void *)p, 1, MS_ASYNC ) == -1 && errno == ENOMEM )
			return true;
	}
	return false;
}

#endif // __PLATFORMCOMPAT_H__
