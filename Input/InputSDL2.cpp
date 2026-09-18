// ============================================================================
//  InputSDL2.cpp -- minimal non-Windows counterpart of Input.cpp.
//
//  Input.cpp (the real DirectInput8-backed implementation) is Windows-only.
//  This is NOT the full SDL2 port described in docs/linux-port.md ("Разбор
//  этапа «Ввод»", 3-7 days) -- it's the minimal slice needed for Game.exe to
//  boot and reach the render loop on Linux: the message queue GetMessage()/
//  AddWinMessage() drains (which Bind.cpp -- unchanged, platform-neutral --
//  depends on), with device enumeration/binding left for that follow-up work.
//  No control names get registered (nameIDs stays empty), so GetControlID()
//  always reports "unknown command" (-1) -- the same fallback path the real
//  code already takes for any unrecognised command.
//
//  Compiled INSTEAD OF Input.cpp on non-Windows builds (see CMakeLists.txt);
//  Bind.cpp is shared unchanged (see file header there -- zero DirectInput
//  dependency). Input.cpp itself is untouched.
// ============================================================================
#include "StdAfx.h"
#include "Input.h"
#include <deque>

namespace NInput
{
////////////////////////////////////////////////////////////////////////////////////////////////////
static std::deque<SMessage> messages;
////////////////////////////////////////////////////////////////////////////////////////////////////
bool InitInput( HWND, bool, int )
{
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool DoneInput()
{
	messages.clear();
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void PumpMessages( bool )
{
	// No devices enumerated in this minimal build -- nothing to poll. The actual
	// SDL event pump (window close, keyboard/mouse) lives in the Linux entry point
	// (Game/MainLinux.cpp), which bridges events in via AddWinMessage() below, the
	// same way the Windows entry point bridges WM_KEYDOWN/WM_CHAR.
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool GetMessage( SMessage *pMsg )
{
	ASSERT( pMsg );
	if ( messages.empty() )
	{
		pMsg->cType = CT_TIME;
		pMsg->tTime = GetTickCount();
		return false;
	}
	*pMsg = messages.front();
	messages.pop_front();
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void AddWinMessage( EControlType cType, int nParam )
{
	SMessage sMessage;
	sMessage.nAction = -1;
	sMessage.ePOVAxis = PA_UNKNOWN;
	sMessage.cType = cType;
	sMessage.nParam = nParam;
	sMessage.bState = true;
	sMessage.tTime = GetTickCount();
	messages.push_back( sMessage );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool GetCharForKey( int, WCHAR * )
{
	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool GetKeyForMessage( const SMessage &, int * )
{
	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
int GetControlID( const string & )
{
	return -1;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void GetControlInfo( int, EControlType *pcType, float *pfGranularity )
{
	*pcType = CT_UNKNOWN;
	*pfGranularity = 1.0f;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void StartSaveInput( CDataStream * ) {}
void StopSaveInput() {}
void StartEmulateInput( CDataStream * ) {}
void StopEmulateInput() {}
////////////////////////////////////////////////////////////////////////////////////////////////////
}
