// ============================================================================
//  PlatformCompat.cpp -- implementation of the CreateThread/WaitForSingleObject
//  trio declared in PlatformCompat.h. Compiled only on the Linux build (see
//  CMakeLists.txt) -- this translation unit is never part of the Windows build.
// ============================================================================
#include "PlatformCompat.h"
#include <thread>

namespace
{
	struct SThreadHandle
	{
		std::thread thread;
		explicit SThreadHandle( std::thread &&t ) : thread( std::move( t ) ) {}
	};
}

HANDLE CreateThread( void *, size_t, LPTHREAD_START_ROUTINE pStartRoutine,
                     LPVOID pParam, DWORD, LPDWORD pThreadId )
{
	std::thread t( [pStartRoutine, pParam]() { pStartRoutine( pParam ); } );
	if ( pThreadId )
		*pThreadId = 0;
	return new SThreadHandle( std::move( t ) );
}

DWORD WaitForSingleObject( HANDLE hHandle, DWORD )
{
	SThreadHandle *pHandle = static_cast<SThreadHandle *>( hHandle );
	if ( pHandle && pHandle->thread.joinable() )
		pHandle->thread.join();
	return WAIT_OBJECT_0;
}

BOOL CloseHandle( HANDLE hObject )
{
	delete static_cast<SThreadHandle *>( hObject );
	return TRUE;
}
