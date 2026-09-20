// ============================================================================
//  PlatformCompat.cpp -- implementation of the CreateThread/WaitForSingleObject
//  trio declared in PlatformCompat.h. Compiled only on the Linux build (see
//  CMakeLists.txt) -- this translation unit is never part of the Windows build.
// ============================================================================
#include "PlatformCompat.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <dlfcn.h>
#include <iconv.h>
#include <vector>
#include <string>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace
{
	// Win32 HANDLEs are opaque and shared across object kinds; WaitForSingleObject
	// and CloseHandle accept any of them. Model that with a common polymorphic
	// base so the two calls can dispatch without the caller telling them which.
	struct SWaitable
	{
		virtual ~SWaitable() {}
		virtual DWORD Wait( DWORD dwMilliseconds ) = 0;
	};

	struct SThreadHandle : SWaitable
	{
		std::thread thread;
		explicit SThreadHandle( std::thread &&t ) : thread( std::move( t ) ) {}
		DWORD Wait( DWORD ) override
		{
			if ( thread.joinable() )
				thread.join();
			return WAIT_OBJECT_0;
		}
	};

	struct SEventHandle : SWaitable
	{
		std::mutex mutex;
		std::condition_variable cond;
		bool bSignalled;
		bool bManualReset;
		SEventHandle( bool _bManualReset, bool _bInitialState )
			: bSignalled( _bInitialState ), bManualReset( _bManualReset ) {}
		DWORD Wait( DWORD dwMilliseconds ) override
		{
			std::unique_lock<std::mutex> lock( mutex );
			if ( dwMilliseconds == 0 )
			{
				if ( !bSignalled )
					return WAIT_TIMEOUT;
			}
			else if ( dwMilliseconds == INFINITE )
				cond.wait( lock, [this]() { return bSignalled; } );
			else if ( !cond.wait_for( lock, std::chrono::milliseconds( dwMilliseconds ),
			                          [this]() { return bSignalled; } ) )
				return WAIT_TIMEOUT;
			if ( !bManualReset )
				bSignalled = false;   // auto-reset: one waiter consumes the signal
			return WAIT_OBJECT_0;
		}
	};

	// CP_ACP on the retail build was CP1251 (Russian system locale) -- the game
	// data, script strings and the editor UI are all CP1251 byte strings.
	const char *CodePageName( UINT nCodePage )
	{
		switch ( nCodePage )
		{
			case CP_UTF8: return "UTF-8";
			case CP_ACP:
			default:      return "CP1251";
		}
	}

	// wchar_t is 4 bytes and native-endian on Linux. Unconvertible input is
	// skipped rather than aborting the whole string, matching the Win32 calls'
	// best-effort behaviour (they substitute a default char).
	int IconvConvert( const char *pszFrom, const char *pszTo,
	                  const char *pSrc, size_t nSrcBytes,
	                  char *pDst, size_t nDstBytes )
	{
		iconv_t cd = iconv_open( pszTo, pszFrom );
		if ( cd == (iconv_t)-1 )
			return 0;
		char *pIn = const_cast<char *>( pSrc );
		char *pOut = pDst;
		size_t nInLeft = nSrcBytes, nOutLeft = nDstBytes;
		while ( nInLeft > 0 )
		{
			size_t nRes = iconv( cd, &pIn, &nInLeft, &pOut, &nOutLeft );
			if ( nRes == (size_t)-1 )
			{
				if ( errno == EILSEQ || errno == EINVAL )
				{
					// unconvertible / truncated input -- skip one input unit and go on
					size_t nUnit = strcmp( pszFrom, "WCHAR_T" ) == 0 ? sizeof( wchar_t ) : 1;
					if ( nInLeft < nUnit )
						break;
					pIn += nUnit;
					nInLeft -= nUnit;
					continue;
				}
				break;  // E2BIG -- destination full
			}
		}
		iconv_close( cd );
		return (int)( pOut - pDst );
	}
}

int WideCharToMultiByte( UINT nCodePage, DWORD, const wchar_t *pWide, int nWideLen,
                         char *pDst, int nDstBytes, const char *, BOOL *pbUsedDefault )
{
	if ( pbUsedDefault )
		*pbUsedDefault = FALSE;
	if ( !pWide )
		return 0;
	if ( nWideLen < 0 )
		nWideLen = (int)wcslen( pWide ) + 1;   // Win32: -1 means "including the terminator"
	if ( !pDst || nDstBytes <= 0 )
		return nWideLen * 4;                   // size query -- worst-case byte count
	return IconvConvert( "WCHAR_T", CodePageName( nCodePage ),
	                     (const char *)pWide, (size_t)nWideLen * sizeof( wchar_t ),
	                     pDst, (size_t)nDstBytes );
}

int MultiByteToWideChar( UINT nCodePage, DWORD, const char *pSrc, int nSrcLen,
                         wchar_t *pDst, int nDstChars )
{
	if ( !pSrc )
		return 0;
	if ( nSrcLen < 0 )
		nSrcLen = (int)strlen( pSrc ) + 1;
	if ( !pDst || nDstChars <= 0 )
		return nSrcLen;
	int nBytes = IconvConvert( CodePageName( nCodePage ), "WCHAR_T",
	                           pSrc, (size_t)nSrcLen,
	                           (char *)pDst, (size_t)nDstChars * sizeof( wchar_t ) );
	return nBytes / (int)sizeof( wchar_t );
}

// ----------------------------------------------------------------------------
//  Directory enumeration -- opendir/readdir + fnmatch behind the Win32 Find*
//  API. See the header for the behavioural notes (case-insensitive matching,
//  backslash-tolerant patterns, only the fields the engine reads are filled).
// ----------------------------------------------------------------------------
namespace
{
	struct SFindHandle
	{
		DIR *pDir;
		std::string szDir;      // directory being walked, with a trailing '/'
		std::string szMask;     // the filename mask, e.g. "*.*"
	};

	// Win32 FILETIME is 100ns units since 1601-01-01; the engine only ever
	// compares two of these for equality, but keep the real encoding anyway.
	FILETIME TimeToFileTime( time_t t )
	{
		uint64_t n = ( (uint64_t)t + 11644473600ull ) * 10000000ull;
		FILETIME ft;
		ft.dwLowDateTime = (DWORD)( n & 0xFFFFFFFFull );
		ft.dwHighDateTime = (DWORD)( n >> 32 );
		return ft;
	}

	bool FillFindData( SFindHandle *pHandle, struct dirent *pEntry, WIN32_FIND_DATAA *pFindData )
	{
		std::string szFull = pHandle->szDir + pEntry->d_name;
		struct stat st;
		if ( stat( szFull.c_str(), &st ) != 0 )
			return false;
		memset( pFindData, 0, sizeof( *pFindData ) );
		pFindData->dwFileAttributes = S_ISDIR( st.st_mode ) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
		if ( pEntry->d_name[0] == '.' && pEntry->d_name[1] != 0 )
			pFindData->dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
		pFindData->nFileSizeLow = (DWORD)( (uint64_t)st.st_size & 0xFFFFFFFFull );
		pFindData->nFileSizeHigh = (DWORD)( (uint64_t)st.st_size >> 32 );
		pFindData->ftLastWriteTime = TimeToFileTime( st.st_mtime );
		pFindData->ftCreationTime = pFindData->ftLastWriteTime;
		pFindData->ftLastAccessTime = TimeToFileTime( st.st_atime );
		strncpy( pFindData->cFileName, pEntry->d_name, MAX_PATH - 1 );
		pFindData->cFileName[MAX_PATH - 1] = 0;
		return true;
	}

	BOOL FindNextIn( SFindHandle *pHandle, WIN32_FIND_DATAA *pFindData )
	{
		for ( struct dirent *pEntry = readdir( pHandle->pDir ); pEntry; pEntry = readdir( pHandle->pDir ) )
		{
			if ( fnmatch( pHandle->szMask.c_str(), pEntry->d_name, FNM_CASEFOLD ) != 0 )
				continue;
			if ( FillFindData( pHandle, pEntry, pFindData ) )
				return TRUE;
		}
		return FALSE;
	}
}

HANDLE FindFirstFileA( const char *pszPattern, WIN32_FIND_DATAA *pFindData )
{
	if ( !pszPattern || !pFindData )
		return INVALID_HANDLE_VALUE;
	std::string szPattern( pszPattern );
	for ( size_t i = 0; i < szPattern.size(); ++i )
		if ( szPattern[i] == '\\' )
			szPattern[i] = '/';
	size_t nSlash = szPattern.rfind( '/' );
	std::string szDir = nSlash == std::string::npos ? std::string( "." ) : szPattern.substr( 0, nSlash );
	std::string szMask = nSlash == std::string::npos ? szPattern : szPattern.substr( nSlash + 1 );
	if ( szDir.empty() )
		szDir = "/";
	// Win32's "*.*" means "everything", including names without a dot.
	if ( szMask == "*.*" )
		szMask = "*";
	DIR *pDir = opendir( szDir.c_str() );
	if ( !pDir )
		return INVALID_HANDLE_VALUE;
	SFindHandle *pHandle = new SFindHandle;
	pHandle->pDir = pDir;
	pHandle->szDir = szDir + "/";
	pHandle->szMask = szMask;
	if ( !FindNextIn( pHandle, pFindData ) )
	{
		closedir( pDir );
		delete pHandle;
		return INVALID_HANDLE_VALUE;
	}
	return pHandle;
}

BOOL FindNextFileA( HANDLE hFind, WIN32_FIND_DATAA *pFindData )
{
	if ( hFind == INVALID_HANDLE_VALUE || !hFind || !pFindData )
		return FALSE;
	return FindNextIn( static_cast<SFindHandle *>( hFind ), pFindData );
}

BOOL FindClose( HANDLE hFind )
{
	if ( hFind == INVALID_HANDLE_VALUE || !hFind )
		return FALSE;
	SFindHandle *pHandle = static_cast<SFindHandle *>( hFind );
	closedir( pHandle->pDir );
	delete pHandle;
	return TRUE;
}

HANDLE CreateThread( void *, size_t, LPTHREAD_START_ROUTINE pStartRoutine,
                     LPVOID pParam, DWORD, LPDWORD pThreadId )
{
	std::thread t( [pStartRoutine, pParam]() { pStartRoutine( pParam ); } );
	if ( pThreadId )
		*pThreadId = 0;
	return new SThreadHandle( std::move( t ) );
}

namespace
{
	// Forward declarations for the file-handle helpers defined with the file I/O
	// section below -- CloseHandle (above them) has to dispatch on handle kind.
	bool IsFileHandle( HANDLE h );
	int HandleToFd( HANDLE h );
}

DWORD WaitForSingleObject( HANDLE hHandle, DWORD dwMilliseconds )
{
	SWaitable *pHandle = static_cast<SWaitable *>( hHandle );
	if ( !pHandle || hHandle == INVALID_HANDLE_VALUE )
		return WAIT_OBJECT_0;
	return pHandle->Wait( dwMilliseconds );
}

BOOL CloseHandle( HANDLE hObject )
{
	if ( !hObject || hObject == INVALID_HANDLE_VALUE )
		return FALSE;
	if ( IsFileHandle( hObject ) )   // a tagged fd, not a heap object -- see the file I/O section
	{
		close( HandleToFd( hObject ) );
		return TRUE;
	}
	delete static_cast<SWaitable *>( hObject );
	return TRUE;
}

HANDLE CreateEvent( void *, BOOL bManualReset, BOOL bInitialState, const char * )
{
	return new SEventHandle( bManualReset != FALSE, bInitialState != FALSE );
}

BOOL SetEvent( HANDLE hEvent )
{
	SEventHandle *pEvent = dynamic_cast<SEventHandle *>( static_cast<SWaitable *>( hEvent ) );
	if ( !pEvent )
		return FALSE;
	{
		std::lock_guard<std::mutex> lock( pEvent->mutex );
		pEvent->bSignalled = true;
	}
	// manual-reset releases every waiter, auto-reset exactly one
	if ( pEvent->bManualReset )
		pEvent->cond.notify_all();
	else
		pEvent->cond.notify_one();
	return TRUE;
}

BOOL ResetEvent( HANDLE hEvent )
{
	SEventHandle *pEvent = dynamic_cast<SEventHandle *>( static_cast<SWaitable *>( hEvent ) );
	if ( !pEvent )
		return FALSE;
	std::lock_guard<std::mutex> lock( pEvent->mutex );
	pEvent->bSignalled = false;
	return TRUE;
}

HMODULE LoadLibraryA( const char *pszFileName )
{
	return pszFileName ? dlopen( pszFileName, RTLD_NOW ) : 0;
}

BOOL FreeLibrary( HMODULE hModule )
{
	return hModule && dlclose( hModule ) == 0 ? TRUE : FALSE;
}

void *GetProcAddress( HMODULE hModule, const char *pszProcName )
{
	return hModule && pszProcName ? dlsym( hModule, pszProcName ) : 0;
}

void GlobalMemoryStatus( S2_MEMORYSTATUS *pStatus )
{
	if ( !pStatus )
		return;
	memset( pStatus, 0, sizeof( *pStatus ) );
	pStatus->dwLength = sizeof( S2_MEMORYSTATUS );
	long nPages = sysconf( _SC_PHYS_PAGES );
	long nAvail = sysconf( _SC_AVPHYS_PAGES );
	long nPageSize = sysconf( _SC_PAGESIZE );
	if ( nPages > 0 && nPageSize > 0 )
	{
		pStatus->dwTotalPhys = (size_t)nPages * (size_t)nPageSize;
		pStatus->dwAvailPhys = nAvail > 0 ? (size_t)nAvail * (size_t)nPageSize : 0;
		pStatus->dwMemoryLoad = pStatus->dwTotalPhys
			? (DWORD)( 100 - ( 100ull * pStatus->dwAvailPhys / pStatus->dwTotalPhys ) ) : 0;
	}
	// The engine only reads dwTotalPhys/dwAvailPhys (texture-budget heuristics);
	// the page-file and virtual figures have no meaningful Linux analogue.
}

// ----------------------------------------------------------------------------
//  File I/O (read path only -- see the header note).
//
//  A file HANDLE is the fd biased by +1 and tagged in the high bit, so it can
//  never collide with a real pointer (the waitable handles) nor with 0 /
//  INVALID_HANDLE_VALUE, and CloseHandle can tell the two kinds apart.
// ----------------------------------------------------------------------------
namespace
{
	const uintptr_t FILE_HANDLE_TAG = (uintptr_t)1 << ( sizeof( uintptr_t ) * 8 - 2 );

	inline HANDLE FdToHandle( int fd ) { return (HANDLE)( FILE_HANDLE_TAG | (uintptr_t)( fd + 1 ) ); }
	bool IsFileHandle( HANDLE h )
	{
		uintptr_t v = (uintptr_t)h;
		return h != INVALID_HANDLE_VALUE && ( v & FILE_HANDLE_TAG ) != 0;
	}
	int HandleToFd( HANDLE h ) { return (int)( ( (uintptr_t)h & ~FILE_HANDLE_TAG ) - 1 ); }
}

HANDLE CreateFileA( const char *pszFileName, DWORD dwAccess, DWORD, void *,
                    DWORD dwCreation, DWORD, HANDLE )
{
	if ( !pszFileName )
		return INVALID_HANDLE_VALUE;
	int nFlags;
	if ( dwAccess & GENERIC_WRITE )
		nFlags = ( dwAccess & GENERIC_READ ) ? O_RDWR : O_WRONLY;
	else
		nFlags = O_RDONLY;
	if ( dwCreation == CREATE_ALWAYS )
		nFlags |= O_CREAT | O_TRUNC;
	int fd = open( pszFileName, nFlags, 0644 );
	return fd < 0 ? INVALID_HANDLE_VALUE : FdToHandle( fd );
}

DWORD GetFileSize( HANDLE hFile, LPDWORD pHigh )
{
	if ( !IsFileHandle( hFile ) )
		return INVALID_FILE_SIZE;
	struct stat st;
	if ( fstat( HandleToFd( hFile ), &st ) != 0 )
		return INVALID_FILE_SIZE;
	if ( pHigh )
		*pHigh = (DWORD)( (uint64_t)st.st_size >> 32 );
	return (DWORD)( (uint64_t)st.st_size & 0xFFFFFFFFull );
}

BOOL ReadFile( HANDLE hFile, LPVOID pBuffer, DWORD nToRead, LPDWORD pnRead, void * )
{
	if ( pnRead )
		*pnRead = 0;
	if ( !IsFileHandle( hFile ) || !pBuffer )
		return FALSE;
	int fd = HandleToFd( hFile );
	char *p = (char *)pBuffer;
	DWORD nDone = 0;
	while ( nDone < nToRead )
	{
		ssize_t n = read( fd, p + nDone, (size_t)( nToRead - nDone ) );
		if ( n < 0 )
		{
			if ( errno == EINTR )
				continue;
			return FALSE;
		}
		if ( n == 0 )
			break;   // EOF -- Win32 reports success with a short count
		nDone += (DWORD)n;
	}
	if ( pnRead )
		*pnRead = nDone;
	return TRUE;
}

// ----------------------------------------------------------------------------
//  Cursor / system parameters (see the header note).
// ----------------------------------------------------------------------------
namespace
{
	TGetCursorPosHook g_pCursorPosHook = 0;
}

void SetCursorPosHook( TGetCursorPosHook pHook ) { g_pCursorPosHook = pHook; }

// ----------------------------------------------------------------------------
//  Keyboard state and clipboard -- both answered by the windowing layer.
// ----------------------------------------------------------------------------
namespace
{
	TGetKeyStateHook g_pGetKeyStateHook = 0;
	TGetClipboardTextHook g_pGetClipboardTextHook = 0;

	// Owns the text handed out by GetClipboardData until CloseClipboard. Not
	// thread-safe, matching Win32: the clipboard is a UI-thread affair, and the
	// engine only touches it from the paste handler.
	std::wstring g_wsClipboard;
	bool g_bClipboardOpen = false;
}

void SetGetKeyStateHook( TGetKeyStateHook pHook ) { g_pGetKeyStateHook = pHook; }
void SetGetClipboardTextHook( TGetClipboardTextHook pHook ) { g_pGetClipboardTextHook = pHook; }

// Win32 reports "currently down" in the high bit, which is the only bit the
// engine tests. No hook -> nothing is pressed.
short GetKeyState( int nVirtKey )
{
	return g_pGetKeyStateHook ? g_pGetKeyStateHook( nVirtKey ) : 0;
}

BOOL OpenClipboard( HWND )
{
	if ( g_bClipboardOpen )
		return FALSE;
	g_wsClipboard = g_pGetClipboardTextHook ? g_pGetClipboardTextHook() : std::wstring();
	g_bClipboardOpen = true;
	return TRUE;
}

BOOL CloseClipboard()
{
	g_bClipboardOpen = false;
	g_wsClipboard.clear();
	return TRUE;
}

// The "handle" is just the buffer itself; GlobalLock/GlobalUnlock are no-ops
// over it. Empty clipboard returns null, like Win32 with no matching format.
HANDLE GetClipboardData( UINT nFormat )
{
	if ( !g_bClipboardOpen || nFormat != CF_UNICODETEXT || g_wsClipboard.empty() )
		return 0;
	return (HANDLE)g_wsClipboard.c_str();
}

// ----------------------------------------------------------------------------
//  Window geometry -- answered by the windowing layer (see the header).
// ----------------------------------------------------------------------------
namespace
{
	SWindowGeometryHooks g_windowHooks = { 0, 0, 0 };
}

void SetWindowGeometryHooks( const SWindowGeometryHooks *pHooks )
{
	if ( pHooks )
		g_windowHooks = *pHooks;
	else
		g_windowHooks = SWindowGeometryHooks{ 0, 0, 0 };
}

// Win32 GetClientRect always reports left/top as 0 -- the client area is
// measured from its own origin -- so right/bottom ARE the width/height. The
// renderer relies on that (pp.BackBufferWidth = windowPos.right).
BOOL GetClientRect( HWND hWnd, RECT *pRect )
{
	if ( !pRect )
		return FALSE;
	pRect->left = pRect->top = pRect->right = pRect->bottom = 0;
	int nWidth = 0, nHeight = 0;
	if ( !g_windowHooks.pGetClientSize || !g_windowHooks.pGetClientSize( hWnd, &nWidth, &nHeight ) )
		return FALSE;
	pRect->right = nWidth;
	pRect->bottom = nHeight;
	return TRUE;
}

// No hook -> report visible: the renderer treats "not visible" as "skip the
// frame", and silently skipping every frame would be the worse failure.
BOOL IsWindowVisible( HWND hWnd )
{
	if ( !g_windowHooks.pIsVisible )
		return TRUE;
	return g_windowHooks.pIsVisible( hWnd ) ? TRUE : FALSE;
}

// Position and z-order are ignored on purpose (see the header); only the size
// is meaningful for an SDL window here.
BOOL SetWindowPos( HWND hWnd, HWND, int, int, int nWidth, int nHeight, UINT )
{
	if ( !g_windowHooks.pResize || nWidth <= 0 || nHeight <= 0 )
		return FALSE;
	g_windowHooks.pResize( hWnd, nWidth, nHeight );
	return TRUE;
}

void *GlobalLock( HANDLE hMem ) { return hMem; }
BOOL GlobalUnlock( HANDLE ) { return TRUE; }

BOOL SystemParametersInfoA( UINT uiAction, UINT, void *pvParam, UINT )
{
	if ( uiAction == SPI_GETMOUSE && pvParam )
	{
		// {threshold1, threshold2, acceleration-enabled} -- all zero: SDL hands
		// us motion the compositor has already accelerated, so the engine must
		// not accelerate it a second time.
		DWORD *pParams = (DWORD *)pvParam;
		pParams[0] = pParams[1] = pParams[2] = 0;
		return TRUE;
	}
	return FALSE;
}

BOOL GetCursorPos( POINT *pPoint )
{
	if ( !pPoint )
		return FALSE;
	pPoint->x = pPoint->y = 0;
	if ( !g_pCursorPosHook )
		return FALSE;
	long nScreenX = 0, nScreenY = 0, nWindowX = 0, nWindowY = 0;
	g_pCursorPosHook( &nScreenX, &nScreenY, &nWindowX, &nWindowY );
	pPoint->x = nScreenX;
	pPoint->y = nScreenY;
	return TRUE;
}

BOOL ScreenToClient( HWND, POINT *pPoint )
{
	if ( !pPoint )
		return FALSE;
	if ( !g_pCursorPosHook )
		return FALSE;
	long nScreenX = 0, nScreenY = 0, nWindowX = 0, nWindowY = 0;
	g_pCursorPosHook( &nScreenX, &nScreenY, &nWindowX, &nWindowY );
	// The hook reports where the window's client origin sits on screen.
	pPoint->x -= nWindowX;
	pPoint->y -= nWindowY;
	return TRUE;
}

// ----------------------------------------------------------------------------
//  <io.h> directory walk + file attribute helpers (see the header note).
// ----------------------------------------------------------------------------
namespace
{
	unsigned FindAttribToIoAttrib( DWORD dwAttributes )
	{
		unsigned n = 0;
		if ( dwAttributes & FILE_ATTRIBUTE_DIRECTORY ) n |= _A_SUBDIR;
		if ( dwAttributes & FILE_ATTRIBUTE_READONLY )  n |= _A_RDONLY;
		if ( dwAttributes & FILE_ATTRIBUTE_HIDDEN )    n |= _A_HIDDEN;
		if ( dwAttributes & FILE_ATTRIBUTE_SYSTEM )    n |= _A_SYSTEM;
		return n;
	}

	void FindDataToIoData( const WIN32_FIND_DATAA &ff, _finddata_t *pDst )
	{
		pDst->attrib = FindAttribToIoAttrib( ff.dwFileAttributes );
		pDst->size = (long)ff.nFileSizeLow;
		pDst->time_create = pDst->time_access = pDst->time_write = 0;
		strncpy( pDst->name, ff.cFileName, MAX_PATH - 1 );
		pDst->name[MAX_PATH - 1] = 0;
	}
}

intptr_t _findfirst( const char *pszPattern, _finddata_t *pFindData )
{
	if ( !pFindData )
		return -1;
	WIN32_FIND_DATAA ff;
	HANDLE h = FindFirstFileA( pszPattern, &ff );
	if ( h == INVALID_HANDLE_VALUE )
		return -1;
	FindDataToIoData( ff, pFindData );
	return (intptr_t)h;
}

int _findnext( intptr_t hFind, _finddata_t *pFindData )
{
	if ( hFind == -1 || !pFindData )
		return -1;
	WIN32_FIND_DATAA ff;
	if ( !FindNextFileA( (HANDLE)hFind, &ff ) )
		return -1;
	FindDataToIoData( ff, pFindData );
	return 0;
}

int _findclose( intptr_t hFind )
{
	if ( hFind == -1 )
		return -1;
	return FindClose( (HANDLE)hFind ) ? 0 : -1;
}

BOOL SetFileAttributesA( const char *pszFileName, DWORD dwAttributes )
{
	if ( !pszFileName )
		return FALSE;
	struct stat st;
	if ( stat( pszFileName, &st ) != 0 )
		return FALSE;
	// Only the read-only bit maps onto a POSIX mode; the rest have no analogue.
	mode_t mode = st.st_mode;
	if ( dwAttributes & FILE_ATTRIBUTE_READONLY )
		mode &= ~(mode_t)( S_IWUSR | S_IWGRP | S_IWOTH );
	else
		mode |= S_IWUSR;
	return chmod( pszFileName, mode ) == 0 ? TRUE : FALSE;
}

BOOL DeleteFileA( const char *pszFileName )
{
	return pszFileName && unlink( pszFileName ) == 0 ? TRUE : FALSE;
}

BOOL CreateDirectoryA( const char *pszPath, void * )
{
	return pszPath && mkdir( pszPath, 0755 ) == 0 ? TRUE : FALSE;
}

BOOL RemoveDirectoryA( const char *pszPath )
{
	return pszPath && rmdir( pszPath ) == 0 ? TRUE : FALSE;
}

BOOL CopyFileA( const char *pszFrom, const char *pszTo, BOOL bFailIfExists )
{
	if ( !pszFrom || !pszTo )
		return FALSE;
	int fdIn = open( pszFrom, O_RDONLY );
	if ( fdIn < 0 )
		return FALSE;
	int nFlags = O_WRONLY | O_CREAT | ( bFailIfExists ? O_EXCL : O_TRUNC );
	int fdOut = open( pszTo, nFlags, 0644 );
	if ( fdOut < 0 )
	{
		close( fdIn );
		return FALSE;
	}
	char buf[64 * 1024];
	bool bOk = true;
	for ( ;; )
	{
		ssize_t nRead = read( fdIn, buf, sizeof( buf ) );
		if ( nRead < 0 )
		{
			if ( errno == EINTR )
				continue;
			bOk = false;
			break;
		}
		if ( nRead == 0 )
			break;
		ssize_t nDone = 0;
		while ( nDone < nRead )
		{
			ssize_t nWritten = write( fdOut, buf + nDone, (size_t)( nRead - nDone ) );
			if ( nWritten < 0 )
			{
				if ( errno == EINTR )
					continue;
				bOk = false;
				break;
			}
			nDone += nWritten;
		}
		if ( !bOk )
			break;
	}
	close( fdIn );
	close( fdOut );
	return bOk ? TRUE : FALSE;
}

int GetLocaleInfoA( DWORD, DWORD dwType, char *pszData, int nSize )
{
	if ( dwType != LOCALE_IDATE || !pszData || nSize < 2 )
		return 0;
	// "1" == day-month-year, the ordering the game shipped with.
	pszData[0] = '1';
	pszData[1] = 0;
	return 2;
}
