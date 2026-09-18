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

void GlobalMemoryStatus( MEMORYSTATUS *pStatus )
{
	if ( !pStatus )
		return;
	memset( pStatus, 0, sizeof( *pStatus ) );
	pStatus->dwLength = sizeof( MEMORYSTATUS );
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
