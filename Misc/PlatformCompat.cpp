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
