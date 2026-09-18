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
#include <list>

using namespace NWinFrame;

namespace
{
	SDL_Window *pWindow = 0;
	volatile bool bExit = false;
	volatile bool bActive = true;
	std::list<SWindowsMsg> msgList;

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
				bActive = true;
			else if ( ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST )
				bActive = false;
			break;
		case SDL_MOUSEMOTION:
			AddMsg( SWindowsMsg::MOUSE_MOVE, ev.motion.x, ev.motion.y, 0 );
			break;
		case SDL_MOUSEBUTTONDOWN:
			AddMsg( ev.button.button == SDL_BUTTON_RIGHT ? SWindowsMsg::RB_DOWN : SWindowsMsg::LB_DOWN,
			        ev.button.x, ev.button.y, 0 );
			break;
		case SDL_MOUSEBUTTONUP:
			AddMsg( ev.button.button == SDL_BUTTON_RIGHT ? SWindowsMsg::RB_UP : SWindowsMsg::LB_UP,
			        ev.button.x, ev.button.y, 0 );
			break;
		case SDL_KEYDOWN:
			// nKey/nRep here carry SDL_Keycode + repeat flag, NOT a Win32 VK_* code -- full
			// DIK_*/VK_* fidelity is out of scope for this minimal boot-to-render-loop pass
			// (see docs/linux-port.md, "Разбор этапа «Ввод»", tracked as follow-up work).
			AddMsg( SWindowsMsg::KEY_DOWN, ev.key.keysym.sym, ev.key.repeat ? 1 : 0, 0 );
			break;
		case SDL_KEYUP:
			AddMsg( SWindowsMsg::KEY_UP, ev.key.keysym.sym, 0, 0 );
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
	return pWindow != 0;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
