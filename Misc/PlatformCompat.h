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
#include <cwctype>
#include <type_traits>
#include <cmath>
#include <ctime>
#include <cerrno>
#include <mutex>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

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

// Case-insensitive string compares (MSVC spellings).
inline int stricmp( const char *a, const char *b ) { return strcasecmp( a, b ); }
inline int strnicmp( const char *a, const char *b, size_t n ) { return strncasecmp( a, b, n ); }
inline int _stricmp( const char *a, const char *b ) { return strcasecmp( a, b ); }
inline int _strnicmp( const char *a, const char *b, size_t n ) { return strncasecmp( a, b, n ); }

// MSVC's wide-string numeric parsers.
inline double _wtof( const wchar_t *psz ) { return wcstod( psz, 0 ); }
inline int _wtoi( const wchar_t *psz ) { return (int)wcstol( psz, 0, 10 ); }
inline long _wtol( const wchar_t *psz ) { return wcstol( psz, 0, 10 ); }

// Double-click threshold. Win32 reports a user preference; SDL has no portable
// equivalent, so return the Windows default the engine was tuned against.
inline UINT GetDoubleClickTime() { return 500; }

// ----------------------------------------------------------------------------
//  Events and modules.
//
//  Events are condition_variable-backed and share the HANDLE space with
//  threads (see PlatformCompat.cpp -- WaitForSingleObject/CloseHandle dispatch
//  on a common waitable base). Modules map onto dlopen/dlsym; nothing in the
//  Linux build loads a DLL yet, but Win32Helper.h's CDLLHandle needs the
//  symbols to compile.
// ----------------------------------------------------------------------------
HANDLE CreateEvent( void *pSecAttr, BOOL bManualReset, BOOL bInitialState, const char *pszName );
BOOL SetEvent( HANDLE hEvent );
BOOL ResetEvent( HANDLE hEvent );

typedef void *HMODULE;
HMODULE LoadLibraryA( const char *pszFileName );
BOOL FreeLibrary( HMODULE hModule );
void *GetProcAddress( HMODULE hModule, const char *pszProcName );
inline HMODULE LoadLibrary( const char *pszFileName ) { return LoadLibraryA( pszFileName ); }

// ----------------------------------------------------------------------------
//  Misc CRT spellings MSVC provides and glibc does not (or names differently).
// ----------------------------------------------------------------------------
typedef int64_t __int64;
#define __assume(x) ((void)0)
#define ZeroMemory( p, n ) memset( (p), 0, (n) )
#define CopyMemory( d, s, n ) memcpy( (d), (s), (n) )
inline int sprintf_s( char *pBuf, size_t nSize, const char *pszFormat, ... )
{
	va_list va;
	va_start( va, pszFormat );
	int nRes = vsnprintf( pBuf, nSize, pszFormat, va );
	va_end( va );
	return nRes;
}
// MSVC also offers a template overload that deduces the bound of a char array,
// so callers write sprintf_s( buf, "..." ) with no size argument. Reproduced
// here -- the engine uses that form in several places.
template <size_t N>
inline int sprintf_s( char ( &buf )[N], const char *pszFormat, ... )
{
	va_list va;
	va_start( va, pszFormat );
	int nRes = vsnprintf( buf, N, pszFormat, va );
	va_end( va );
	return nRes;
}

// ----------------------------------------------------------------------------
//  Virtual-key codes. Only the ones the engine actually names are listed; the
//  values are the Win32 ones so saved key bindings keep their meaning.
// ----------------------------------------------------------------------------
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_UP      0x26
#define VK_DOWN    0x28
#define VK_DELETE  0x2E

// ----------------------------------------------------------------------------
//  Local time and memory status.
// ----------------------------------------------------------------------------
struct SYSTEMTIME
{
	WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
inline void GetLocalTime( SYSTEMTIME *pTime )
{
	struct timespec ts;
	clock_gettime( CLOCK_REALTIME, &ts );
	struct tm tmv;
	localtime_r( &ts.tv_sec, &tmv );
	pTime->wYear = (WORD)( tmv.tm_year + 1900 );
	pTime->wMonth = (WORD)( tmv.tm_mon + 1 );
	pTime->wDayOfWeek = (WORD)tmv.tm_wday;
	pTime->wDay = (WORD)tmv.tm_mday;
	pTime->wHour = (WORD)tmv.tm_hour;
	pTime->wMinute = (WORD)tmv.tm_min;
	pTime->wSecond = (WORD)tmv.tm_sec;
	pTime->wMilliseconds = (WORD)( ts.tv_nsec / 1000000 );
}

struct MEMORYSTATUS
{
	DWORD dwLength, dwMemoryLoad;
	size_t dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
};
void GlobalMemoryStatus( MEMORYSTATUS *pStatus );

// ----------------------------------------------------------------------------
//  File I/O.
//
//  Only the read path is provided -- that is all the engine uses these Win32
//  calls for (GResource.cpp, ModManager.cpp); everything else goes through
//  FileIO's own streams. Backed by open/read/fstat, with the handle carried as
//  an fd so CloseHandle can tell it apart from the heap-allocated waitables.
// ----------------------------------------------------------------------------
#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define FILE_SHARE_READ  0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define CREATE_ALWAYS   2
#define OPEN_EXISTING   3
#define INVALID_FILE_SIZE 0xFFFFFFFFu

HANDLE CreateFileA( const char *pszFileName, DWORD dwAccess, DWORD dwShare, void *pSecAttr,
                    DWORD dwCreation, DWORD dwFlags, HANDLE hTemplate );
inline HANDLE CreateFile( const char *pszFileName, DWORD dwAccess, DWORD dwShare, void *pSecAttr,
                          DWORD dwCreation, DWORD dwFlags, HANDLE hTemplate )
{
	return CreateFileA( pszFileName, dwAccess, dwShare, pSecAttr, dwCreation, dwFlags, hTemplate );
}
DWORD GetFileSize( HANDLE hFile, LPDWORD pHigh );
BOOL ReadFile( HANDLE hFile, LPVOID pBuffer, DWORD nToRead, LPDWORD pnRead, void *pOverlapped );

// ----------------------------------------------------------------------------
//  Mouse cursor position and the pointer-acceleration query.
//
//  SPI_GETMOUSE reports the two acceleration thresholds plus the enable flag.
//  SDL delivers already-accelerated motion, so acceleration is reported OFF
//  (0,0,0) -- the engine then applies none of its own, which is what we want.
//  GetCursorPos/ScreenToClient are answered from the SDL window (implemented
//  in PlatformCompat.cpp via the hooks Game/WinFrameSDL2.cpp installs); with
//  no window up yet they report the origin rather than failing.
// ----------------------------------------------------------------------------
#define SPI_GETMOUSE 0x0003
struct POINT { LONG x, y; };

BOOL SystemParametersInfoA( UINT uiAction, UINT uiParam, void *pvParam, UINT fWinIni );
inline BOOL SystemParametersInfo( UINT uiAction, UINT uiParam, void *pvParam, UINT fWinIni )
{
	return SystemParametersInfoA( uiAction, uiParam, pvParam, fWinIni );
}
BOOL GetCursorPos( POINT *pPoint );
BOOL ScreenToClient( HWND hWnd, POINT *pPoint );

// Installed by the windowing layer (Game/WinFrameSDL2.cpp) so the two calls
// above can answer from the live SDL window. Misc must not depend on SDL2
// itself, hence the hook rather than a direct call.
typedef void ( *TGetCursorPosHook )( long *pnScreenX, long *pnScreenY,
                                     long *pnWindowX, long *pnWindowY );
void SetCursorPosHook( TGetCursorPosHook pHook );

// ----------------------------------------------------------------------------
//  BMP file structures (screenshot writing -- bmpfile.cpp, iMain.cpp).
//
//  These are written to disk verbatim, so the layout is what matters: exactly
//  14 and 40 bytes, packed, little-endian fields. BITMAPFILEHEADER is packed to
//  1 because its 4-byte bfSize sits at offset 2 and must NOT be padded -- that
//  is why the Win32 header packs it too.
// ----------------------------------------------------------------------------
#define BI_RGB 0

#pragma pack( push, 1 )
struct BITMAPFILEHEADER
{
	WORD  bfType;
	DWORD bfSize;
	WORD  bfReserved1;
	WORD  bfReserved2;
	DWORD bfOffBits;
};
struct BITMAPINFOHEADER
{
	DWORD biSize;
	int32_t biWidth;
	int32_t biHeight;
	WORD  biPlanes;
	WORD  biBitCount;
	DWORD biCompression;
	DWORD biSizeImage;
	int32_t biXPelsPerMeter;
	int32_t biYPelsPerMeter;
	DWORD biClrUsed;
	DWORD biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
#pragma pack( pop )

static_assert( sizeof( BITMAPFILEHEADER ) == 14, "BMP file header must stay 14 bytes on disk" );
static_assert( sizeof( BITMAPINFOHEADER ) == 40, "BMP info header must stay 40 bytes on disk" );

// ----------------------------------------------------------------------------
//  windef.h's min/max, as functions.
//
//  The engine calls lowercase min()/max() with MIXED argument types
//  (min(float, int), max(int, float), min(int, size_t)). On Windows those are
//  windows.h's macros, which do not care about types; std::min/std::max are
//  templates that need BOTH arguments to be the same type, so those calls do
//  not compile here.
//
//  Reproducing the macros would wreck libstdc++ -- StdAfx.h includes
//  <algorithm> after this header, and the macro would eat std::min's own
//  declaration. So these are overloads instead, deliberately constrained to
//  the case the macros were covering: two DIFFERENT types. Same-type calls
//  still resolve to std::min/std::max (StdAfx.h does `using namespace std`),
//  and the result type is the ternary's common type, exactly as the macro
//  produced.
//
//  The engine's own capitalised Max()/Min() templates in Misc/tools.h are a
//  separate thing and unaffected.
// ----------------------------------------------------------------------------
template <class TA, class TB>
inline typename std::enable_if< !std::is_same<TA, TB>::value,
                                typename std::common_type<TA, TB>::type >::type
min( TA a, TB b )
{
	typedef typename std::common_type<TA, TB>::type TR;
	return (TR)a < (TR)b ? (TR)a : (TR)b;
}
template <class TA, class TB>
inline typename std::enable_if< !std::is_same<TA, TB>::value,
                                typename std::common_type<TA, TB>::type >::type
max( TA a, TB b )
{
	typedef typename std::common_type<TA, TB>::type TR;
	return (TR)a > (TR)b ? (TR)a : (TR)b;
}

// ----------------------------------------------------------------------------
//  MSVC's <io.h> directory walk (_findfirst / _findnext / _findclose) and the
//  file-attribute helpers, used by the save-game manager. Layered on the Find*
//  API above, so the same case-insensitive matching applies.
// ----------------------------------------------------------------------------
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH   0x20

struct _finddata_t
{
	unsigned attrib;
	time_t   time_create;
	time_t   time_access;
	time_t   time_write;
	long     size;
	char     name[MAX_PATH];
};

intptr_t _findfirst( const char *pszPattern, _finddata_t *pFindData );
int _findnext( intptr_t hFind, _finddata_t *pFindData );
int _findclose( intptr_t hFind );

BOOL SetFileAttributesA( const char *pszFileName, DWORD dwAttributes );
BOOL DeleteFileA( const char *pszFileName );
BOOL CreateDirectoryA( const char *pszPath, void *pSecAttr );
BOOL RemoveDirectoryA( const char *pszPath );
BOOL CopyFileA( const char *pszFrom, const char *pszTo, BOOL bFailIfExists );
inline BOOL SetFileAttributes( const char *pszFileName, DWORD dwAttributes )
{
	return SetFileAttributesA( pszFileName, dwAttributes );
}
inline BOOL DeleteFile( const char *pszFileName ) { return DeleteFileA( pszFileName ); }
inline BOOL CreateDirectory( const char *pszPath, void *pSecAttr ) { return CreateDirectoryA( pszPath, pSecAttr ); }
inline BOOL RemoveDirectory( const char *pszPath ) { return RemoveDirectoryA( pszPath ); }
inline BOOL CopyFile( const char *pszFrom, const char *pszTo, BOOL bFailIfExists )
{
	return CopyFileA( pszFrom, pszTo, bFailIfExists );
}

// Date-format query. Only LOCALE_IDATE is asked for (the save browser picks a
// d/m/y ordering); "1" is Windows' day-month-year, matching the Russian locale
// the game shipped under.
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_IDATE 0x00000021
int GetLocaleInfoA( DWORD dwLocale, DWORD dwType, char *pszData, int nSize );

inline int _isnan( double d ) { return std::isnan( d ) ? 1 : 0; }

// MSVC spellings of the POSIX stat family (<sys/stat.h> is included above).
#define _stat stat
#define _fstat fstat
#define _access access

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
#define WAIT_TIMEOUT 0x102u
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
