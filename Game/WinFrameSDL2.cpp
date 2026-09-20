// ============================================================================
//  WinFrameSDL2.cpp -- SDL2-backed NWinFrame:: for the Linux build.
//
//  WinFrame.cpp (the real Win32 window) is Windows-only. This implements the
//  same interface (WinFrame.h -- shared, unchanged) with an SDL2 window
//  instead of a Win32 one, so callers need no changes:
//    - Main/WinInputConv.cpp (the WM_KEYDOWN/WM_CHAR -> NInput bridge) only
//      ever calls NWinFrame::GetMessage(), already platform-neutral.
//    - Game/MainLinux.cpp (the WinMain equivalent, next file) drives the
//      same InitApplication/PumpMessages/IsAppActive/IsExit loop shape as
//      the Windows entry point.
//
//  GetWnd() returns the SDL_Window* disguised as an HWND (PlatformCompat.h's
//  opaque HWND is just a tagged pointer) -- the render port (dxvk-native
//  wiring, still ahead) will reinterpret_cast it back to SDL_Window* to build
//  the swapchain.
//
//  Compiled INSTEAD OF WinFrame.cpp on non-Windows builds; WinFrame.cpp is
//  untouched.
// ============================================================================
#include "StdAfx.h"
#include "WinFrame.h"
#include "../Misc/HPTimer.h"
#include <SDL2/SDL.h>
#include <cstdlib>   // atexit
#include <csignal>
#include <list>

// Linux-only addition to the NWinFrame interface; WinFrame.h is shared with the
// Windows build, which needs no such call (the OS restores the mode for it).
namespace NWinFrame { void DoneApplication(); }

// Linux-only entry points on Input/InputSDL2.cpp -- the SDL2 counterpart of the
// DirectInput device poll. Declared here rather than in Input.h, which is shared
// with the Windows build (where DirectInput reads the devices itself).
namespace NInput
{
	void NotifyKey( int nScancode, bool bDown );
	void NotifyMouseButton( int nButton, bool bDown );
	void NotifyMouseMotion( int nDeltaX, int nDeltaY );
	void NotifyMouseWheel( int nNotches );
}

using namespace NWinFrame;

namespace
{
	SDL_Window *pWindow = 0;
	volatile bool bExit = false;
	volatile bool bActive = true;
	std::list<SWindowsMsg> msgList;


	// --- Win32 shim hooks -------------------------------------------------------
	// Misc/ deliberately does not depend on SDL2, so PlatformCompat.h asks the
	// windowing layer for anything only a window can answer. Installed in
	// InitApplication, below.

	void CursorPosHook( long *pnScreenX, long *pnScreenY, long *pnClientX, long *pnClientY )
	{
		int nGlobalX = 0, nGlobalY = 0;
		SDL_GetGlobalMouseState( &nGlobalX, &nGlobalY );
		if ( pnScreenX ) *pnScreenX = nGlobalX;
		if ( pnScreenY ) *pnScreenY = nGlobalY;

		// Client coords: SDL reports these relative to the focused window already.
		int nWinX = 0, nWinY = 0;
		SDL_GetMouseState( &nWinX, &nWinY );
		if ( pnClientX ) *pnClientX = nWinX;
		if ( pnClientY ) *pnClientY = nWinY;
	}

	short GetKeyStateHook( int nVirtKey )
	{
		const Uint8 *pKeys = SDL_GetKeyboardState( 0 );
		if ( !pKeys )
			return 0;
		bool bDown = false;
		switch ( nVirtKey )
		{
		case VK_CONTROL: bDown = pKeys[SDL_SCANCODE_LCTRL]  || pKeys[SDL_SCANCODE_RCTRL];  break;
		case VK_HOME:    bDown = pKeys[SDL_SCANCODE_HOME];   break;
		case VK_END:     bDown = pKeys[SDL_SCANCODE_END];    break;
		case VK_LEFT:    bDown = pKeys[SDL_SCANCODE_LEFT];   break;
		case VK_RIGHT:   bDown = pKeys[SDL_SCANCODE_RIGHT];  break;
		case VK_UP:      bDown = pKeys[SDL_SCANCODE_UP];     break;
		case VK_DOWN:    bDown = pKeys[SDL_SCANCODE_DOWN];   break;
		case VK_BACK:    bDown = pKeys[SDL_SCANCODE_BACKSPACE]; break;
		case VK_DELETE:  bDown = pKeys[SDL_SCANCODE_DELETE]; break;
		case VK_TAB:     bDown = pKeys[SDL_SCANCODE_TAB];    break;
		case VK_RETURN:  bDown = pKeys[SDL_SCANCODE_RETURN] || pKeys[SDL_SCANCODE_KP_ENTER]; break;
		default:         return 0;   // not a key the engine asks about
		}
		return bDown ? (short)0x8000 : 0;   // Win32 puts "is down" in the high bit
	}

	std::wstring GetClipboardTextHook()
	{
		if ( !SDL_HasClipboardText() )
			return std::wstring();
		char *pszText = SDL_GetClipboardText();   // UTF-8, SDL-allocated
		if ( !pszText )
			return std::wstring();
		// UTF-8 -> UTF-32/16 via the shim's own converter, so the clipboard obeys
		// the same encoding rules as the rest of the engine's wide strings.
		std::wstring wsResult;
		int nNeeded = MultiByteToWideChar( CP_UTF8, 0, pszText, -1, 0, 0 );
		if ( nNeeded > 1 )
		{
			wsResult.resize( nNeeded - 1 );
			MultiByteToWideChar( CP_UTF8, 0, pszText, -1, &wsResult[0], nNeeded );
		}
		SDL_free( pszText );
		return wsResult;
	}


	// --- window geometry, for Main/Gfx.cpp's backbuffer sizing ------------------
	bool GetClientSizeHook( HWND hWnd, int *pnWidth, int *pnHeight )
	{
		SDL_Window *pWnd = (SDL_Window *)hWnd;
		if ( !pWnd )
			return false;
		int nWidth = 0, nHeight = 0;
		SDL_GetWindowSize( pWnd, &nWidth, &nHeight );
		if ( pnWidth ) *pnWidth = nWidth;
		if ( pnHeight ) *pnHeight = nHeight;
		return true;
	}

	bool IsWindowVisibleHook( HWND hWnd )
	{
		SDL_Window *pWnd = (SDL_Window *)hWnd;
		if ( !pWnd )
			return false;
		const Uint32 dwFlags = SDL_GetWindowFlags( pWnd );
		return ( dwFlags & SDL_WINDOW_SHOWN ) && !( dwFlags & SDL_WINDOW_MINIMIZED );
	}

	void ResizeWindowHook( HWND hWnd, int nWidth, int nHeight )
	{
		SDL_Window *pWnd = (SDL_Window *)hWnd;
		if ( !pWnd )
			return;
		SDL_SetWindowSize( pWnd, nWidth, nHeight );

		// Resizing drops relative mouse mode (the grab is tied to the old surface),
		// which showed up as the system pointer reappearing and drifting away from
		// the in-game cursor after changing resolution in Options. Re-assert it --
		// but only while we have focus, or we would steal a pointer the user is
		// using elsewhere.
		if ( bActive && SDL_GetRelativeMouseMode() == SDL_FALSE )
			SDL_SetRelativeMouseMode( SDL_TRUE );
	}

	// Drop out of fullscreen (which is what actually restores the desktop mode),
	// then tear the window down. Safe to call twice -- atexit and the explicit
	// shutdown both land here.
	// bMinimal: called from a signal handler, where the process is already dying
	// and SDL is not async-signal-safe. Do only the one thing the desktop cannot
	// recover from by itself -- leaving fullscreen, which is what puts the video
	// mode back -- and let the kernel reclaim the window and the X connection.
	// Tearing down further from a handler raced with dxvk's own teardown and
	// occasionally killed the process with a BadRRCrtc X error instead.
	void RestoreDisplayMode( bool bMinimal )
	{
		SDL_SetRelativeMouseMode( SDL_FALSE );   // give the pointer back too
		if ( pWindow )
			SDL_SetWindowFullscreen( pWindow, 0 );
		if ( bMinimal )
			return;

		if ( pWindow )
		{
			SDL_DestroyWindow( pWindow );
			pWindow = 0;
		}
		if ( SDL_WasInit( SDL_INIT_VIDEO ) )
			SDL_QuitSubSystem( SDL_INIT_VIDEO );
	}

	void RestoreDisplayModeAtExit() { RestoreDisplayMode( false ); }

	// Restore, then let the signal do what it would have done. Only
	// async-signal-safe work happens before re-raising: SDL's mode switch is not
	// strictly in that class, but a stuck display is the worse outcome, and by
	// this point the process is going away regardless.
	void FatalSignalHandler( int nSignal )
	{
		RestoreDisplayMode( true );   // minimal: see the note above
		signal( nSignal, SIG_DFL );
		raise( nSignal );
	}

	// DirectInput numbers mouse buttons 0=left, 1=right, 2=middle; SDL uses
	// 1=left, 2=middle, 3=right. cfg/input.cfg binds MOUSE_BUTTON0/1 for
	// left/right, so the swap matters.
	int SdlButtonToIndex( Uint8 nSdlButton )
	{
		switch ( nSdlButton )
		{
		case SDL_BUTTON_LEFT:   return 0;
		case SDL_BUTTON_RIGHT:  return 1;
		case SDL_BUTTON_MIDDLE: return 2;
		default:                return nSdlButton - 1;   // X1/X2 -> 3/4
		}
	}

	void AddMsg( SWindowsMsg::EMsg msg, int x, int y, DWORD dwFlags )
	{
		SWindowsMsg m;
		NHPTimer::GetTime( &m.time );
		m.msg = msg;
		m.x = x;
		m.y = y;
		m.dwFlags = dwFlags;
		msgList.push_back( m );
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NWinFrame::GetMessage( SWindowsMsg *pRes )
{
	if ( !msgList.empty() )
	{
		*pRes = msgList.front();
		msgList.pop_front();
		return true;
	}
	pRes->msg = SWindowsMsg::TIME;
	NHPTimer::GetTime( &pRes->time );
	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NWinFrame::IsAppActive() { return bActive; }
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NWinFrame::IsExit() { return bExit; }
////////////////////////////////////////////////////////////////////////////////////////////////////
void NWinFrame::Exit()
{
	bExit = true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
HWND NWinFrame::GetWnd()
{
	return reinterpret_cast<HWND>( pWindow );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NWinFrame::PumpMessages()
{
	SDL_Event ev;
	while ( SDL_PollEvent( &ev ) )
	{
		switch ( ev.type )
		{
		case SDL_QUIT:
			bExit = true;
			break;
		case SDL_WINDOWEVENT:
			if ( ev.window.event == SDL_WINDOWEVENT_CLOSE )
				bExit = true;
			else if ( ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED )
			{
				bActive = true;
				SDL_SetRelativeMouseMode( SDL_TRUE );
			}
			else if ( ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST )
			{
				bActive = false;
				// Hand the pointer back, or alt-tabbing away leaves it captured.
				SDL_SetRelativeMouseMode( SDL_FALSE );
			}
			else if ( ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED )
			{
				// Backstop for the same problem ResizeWindowHook handles: any route
				// that resizes the window drops the mouse grab. Catching the event
				// covers resizes we did not initiate ourselves.
				if ( bActive && SDL_GetRelativeMouseMode() == SDL_FALSE )
					SDL_SetRelativeMouseMode( SDL_TRUE );
			}
			break;
		case SDL_MOUSEMOTION:
			AddMsg( SWindowsMsg::MOUSE_MOVE, ev.motion.x, ev.motion.y, 0 );
			// NInput wants RELATIVE motion: the cursor integrates deltas, it is not
			// positioned absolutely (Main/Cursor.cpp). SDL gives us both.
			NInput::NotifyMouseMotion( ev.motion.xrel, ev.motion.yrel );
			break;
		case SDL_MOUSEBUTTONDOWN:
			AddMsg( ev.button.button == SDL_BUTTON_RIGHT ? SWindowsMsg::RB_DOWN : SWindowsMsg::LB_DOWN,
			        ev.button.x, ev.button.y, 0 );
			NInput::NotifyMouseButton( SdlButtonToIndex( ev.button.button ), true );
			break;
		case SDL_MOUSEBUTTONUP:
			AddMsg( ev.button.button == SDL_BUTTON_RIGHT ? SWindowsMsg::RB_UP : SWindowsMsg::LB_UP,
			        ev.button.x, ev.button.y, 0 );
			NInput::NotifyMouseButton( SdlButtonToIndex( ev.button.button ), false );
			break;
		case SDL_MOUSEWHEEL:
			NInput::NotifyMouseWheel( ev.wheel.y );
			break;
		case SDL_KEYDOWN:
			// nKey/nRep here carry SDL_Keycode + repeat flag, NOT a Win32 VK_* code -- full
			// DIK_*/VK_* fidelity is out of scope for this minimal boot-to-render-loop pass
			// (see docs/linux-port.md, "Разбор этапа «Ввод»", tracked as follow-up work).
			AddMsg( SWindowsMsg::KEY_DOWN, ev.key.keysym.sym, ev.key.repeat ? 1 : 0, 0 );
			// Auto-repeat must not reach the bindings: DirectInput reports physical
			// transitions only, and a held key would re-fire every EVENT binding.
			if ( !ev.key.repeat )
				NInput::NotifyKey( ev.key.keysym.scancode, true );
			break;
		case SDL_KEYUP:
			AddMsg( SWindowsMsg::KEY_UP, ev.key.keysym.sym, 0, 0 );
			NInput::NotifyKey( ev.key.keysym.scancode, false );
			break;
		case SDL_TEXTINPUT:
			if ( ev.text.text[0] )
				AddMsg( SWindowsMsg::CHAR, (unsigned char)ev.text.text[0], 1, 0 );
			break;
		}
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NWinFrame::InitApplication( HINSTANCE, const char *pszAppName, const char * )
{
	if ( SDL_WasInit( SDL_INIT_VIDEO ) == 0 && SDL_InitSubSystem( SDL_INIT_VIDEO ) != 0 )
		return false;
	pWindow = SDL_CreateWindow( pszAppName, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                             100, 100, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE );
	if ( !pWindow )
		return false;

	// Hand the Win32 shim the things only the windowing layer can answer.
	SetCursorPosHook( CursorPosHook );
	SetGetKeyStateHook( GetKeyStateHook );
	SetGetClipboardTextHook( GetClipboardTextHook );

	static const SWindowGeometryHooks geometryHooks =
		{ GetClientSizeHook, IsWindowVisibleHook, ResizeWindowHook };
	SetWindowGeometryHooks( &geometryHooks );

	// Relative mouse mode: hides the system pointer, confines it to the window and
	// reports pure deltas. That is what the engine expects -- it draws its OWN
	// cursor, integrating mouse deltas with its own acceleration curve
	// (Main/Cursor.cpp, SPI_GETMOUSE thresholds), so a visible system pointer is a
	// second cursor moving at a different speed. The retail build gets the same
	// effect from DirectInput's exclusive cooperative level.
	SDL_SetRelativeMouseMode( SDL_TRUE );

	// Leaving the desktop stuck at 1024x768 is far worse than anything these
	// handlers cost, so cover every way out:
	//   - atexit: the early-return paths in MainLinux.cpp (sound/input failures)
	//     and any exit() that skips DoneApplication.
	//   - signals: atexit does NOT run on a signal, which is exactly how the game
	//     ends when it crashes or is killed from a terminal. Each handler restores
	//     the mode, then re-raises with the default action so the exit status and
	//     any core dump stay truthful.
	atexit( RestoreDisplayModeAtExit );
	static const int anFatalSignals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
	for ( size_t i = 0; i < sizeof( anFatalSignals ) / sizeof( anFatalSignals[0] ); ++i )
		signal( anFatalSignals[i], FatalSignalHandler );
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// Windows restores the display mode by itself when a fullscreen-exclusive app
// exits; X11 does not -- whatever mode the game set stays on the desktop. dxvk
// switches the mode through SDL, so it has to be handed back here.
void NWinFrame::DoneApplication()
{
	RestoreDisplayMode( false );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
