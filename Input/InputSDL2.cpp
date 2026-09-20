// ============================================================================
//  InputSDL2.cpp -- SDL2-backed NInput:: for the Linux build.
//
//  Input.cpp (the real DirectInput8 implementation) is Windows-only; this
//  provides the same interface on top of SDL2. Bind.cpp is shared unchanged --
//  it is platform-neutral and drives everything through the SMessage queue and
//  the control IDs handed out here.
//
//  THE CONTRACT THIS HAS TO HONOUR (learned from Input.cpp / Bind.cpp):
//
//  * Control names. cfg/input.cfg binds by NAME ("ESC", "MOUSE_AXIS_X",
//    "LCTRL"). GetControlID maps a name to an action id; Bind.cpp stores that
//    id and later matches incoming SMessage::nAction against it. The name list
//    below is transcribed from kiKeyInfoList in Input.cpp so the SAME config
//    file binds identically on both platforms.
//
//  * Action ids. Input.cpp builds them as INPUT_KEYID(deviceId, objectOffset)
//    -- device index in the top byte, DirectInput object offset in the low
//    bits. The ids are never serialised (config stores names), but keeping the
//    same construction means a binding resolves to the same number as retail,
//    which makes the two builds directly comparable when debugging.
//
//  * Axes are RELATIVE. SMessage::nParam for CT_AXIS is a delta, not a
//    position (Input.cpp: `dwData - dwLastValue`). The cursor integrates these
//    deltas (Main/Cursor.cpp: vCursorPos += bindX.GetDelta() * 250), so mouse
//    motion must be reported as movement, not as coordinates.
//
//  * CT_KEY carries bState (down/up) and nParam is unused; CT_WIN_CHAR /
//    CT_WIN_KEY come in from the windowing layer via AddWinMessage.
//
//  Events arrive from Game/WinFrameSDL2.cpp, which owns the SDL event pump --
//  Input has no window of its own, exactly as the DirectInput version receives
//  its window handle from the caller.
//
//  Compiled INSTEAD OF Input.cpp on non-Windows builds (see CMakeLists.txt);
//  Input.cpp itself is untouched.
// ============================================================================
#include "StdAfx.h"
#include "Input.h"
#include <SDL2/SDL.h>
#include <deque>
#include <unordered_map>

namespace NInput
{
////////////////////////////////////////////////////////////////////////////////////////////////////
// Device ids: the top byte of an action id. DirectInput enumerates devices in
// discovery order; here the two devices we support are fixed, which keeps ids
// stable across runs (DirectInput's are not, strictly speaking, either).
static const int DEV_KEYBOARD = 0;
static const int DEV_MOUSE = 1;

// Same layout as Input.cpp's INPUT_KEYID.
#define INPUT_KEYID( vID, vOFFS )   ( ( ( (vID) & 0xFF ) << 24 ) | (vOFFS) )

// DirectInput mouse object offsets (c_dfDIMouse): axes at 0/4/8, buttons from 12.
static const int DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON_BASE = 12;

////////////////////////////////////////////////////////////////////////////////////////////////////
struct SControlInfo
{
	const char *pszName;
	int nDevice;
	int nOffset;          // DirectInput object offset -- see above
	EControlType cType;
	SDL_Scancode scancode;   // SDL_SCANCODE_UNKNOWN for mouse entries and for
	                         // keys with no SDL equivalent (JP/OEM extras): they
	                         // stay bindable by name, they just never fire here.
};

// Transcribed from kiKeyInfoList (Input.cpp). The numeric offsets are the DIK_*
// / DIMOFS_* values, which are fixed by the DirectInput ABI (DIK_* are PS/2
// set-1 scancodes), so they can be spelled out without dinput.h.
static const SControlInfo controlList[] =
{
	{ "ESC", DEV_KEYBOARD, 0x01, CT_KEY, SDL_SCANCODE_ESCAPE },
	{ "1", DEV_KEYBOARD, 0x02, CT_KEY, SDL_SCANCODE_1 },
	{ "2", DEV_KEYBOARD, 0x03, CT_KEY, SDL_SCANCODE_2 },
	{ "3", DEV_KEYBOARD, 0x04, CT_KEY, SDL_SCANCODE_3 },
	{ "4", DEV_KEYBOARD, 0x05, CT_KEY, SDL_SCANCODE_4 },
	{ "5", DEV_KEYBOARD, 0x06, CT_KEY, SDL_SCANCODE_5 },
	{ "6", DEV_KEYBOARD, 0x07, CT_KEY, SDL_SCANCODE_6 },
	{ "7", DEV_KEYBOARD, 0x08, CT_KEY, SDL_SCANCODE_7 },
	{ "8", DEV_KEYBOARD, 0x09, CT_KEY, SDL_SCANCODE_8 },
	{ "9", DEV_KEYBOARD, 0x0A, CT_KEY, SDL_SCANCODE_9 },
	{ "0", DEV_KEYBOARD, 0x0B, CT_KEY, SDL_SCANCODE_0 },
	{ "-", DEV_KEYBOARD, 0x0C, CT_KEY, SDL_SCANCODE_MINUS },
	{ "=", DEV_KEYBOARD, 0x0D, CT_KEY, SDL_SCANCODE_EQUALS },
	{ "BACKSPACE", DEV_KEYBOARD, 0x0E, CT_KEY, SDL_SCANCODE_BACKSPACE },
	{ "TAB", DEV_KEYBOARD, 0x0F, CT_KEY, SDL_SCANCODE_TAB },
	{ "Q", DEV_KEYBOARD, 0x10, CT_KEY, SDL_SCANCODE_Q },
	{ "W", DEV_KEYBOARD, 0x11, CT_KEY, SDL_SCANCODE_W },
	{ "E", DEV_KEYBOARD, 0x12, CT_KEY, SDL_SCANCODE_E },
	{ "R", DEV_KEYBOARD, 0x13, CT_KEY, SDL_SCANCODE_R },
	{ "T", DEV_KEYBOARD, 0x14, CT_KEY, SDL_SCANCODE_T },
	{ "Y", DEV_KEYBOARD, 0x15, CT_KEY, SDL_SCANCODE_Y },
	{ "U", DEV_KEYBOARD, 0x16, CT_KEY, SDL_SCANCODE_U },
	{ "I", DEV_KEYBOARD, 0x17, CT_KEY, SDL_SCANCODE_I },
	{ "O", DEV_KEYBOARD, 0x18, CT_KEY, SDL_SCANCODE_O },
	{ "P", DEV_KEYBOARD, 0x19, CT_KEY, SDL_SCANCODE_P },
	{ "[", DEV_KEYBOARD, 0x1A, CT_KEY, SDL_SCANCODE_LEFTBRACKET },
	{ "]", DEV_KEYBOARD, 0x1B, CT_KEY, SDL_SCANCODE_RIGHTBRACKET },
	{ "ENTER", DEV_KEYBOARD, 0x1C, CT_KEY, SDL_SCANCODE_RETURN },
	{ "LCTRL", DEV_KEYBOARD, 0x1D, CT_KEY, SDL_SCANCODE_LCTRL },
	{ "A", DEV_KEYBOARD, 0x1E, CT_KEY, SDL_SCANCODE_A },
	{ "S", DEV_KEYBOARD, 0x1F, CT_KEY, SDL_SCANCODE_S },
	{ "D", DEV_KEYBOARD, 0x20, CT_KEY, SDL_SCANCODE_D },
	{ "F", DEV_KEYBOARD, 0x21, CT_KEY, SDL_SCANCODE_F },
	{ "G", DEV_KEYBOARD, 0x22, CT_KEY, SDL_SCANCODE_G },
	{ "H", DEV_KEYBOARD, 0x23, CT_KEY, SDL_SCANCODE_H },
	{ "J", DEV_KEYBOARD, 0x24, CT_KEY, SDL_SCANCODE_J },
	{ "K", DEV_KEYBOARD, 0x25, CT_KEY, SDL_SCANCODE_K },
	{ "L", DEV_KEYBOARD, 0x26, CT_KEY, SDL_SCANCODE_L },
	{ ";", DEV_KEYBOARD, 0x27, CT_KEY, SDL_SCANCODE_SEMICOLON },
	{ "'", DEV_KEYBOARD, 0x28, CT_KEY, SDL_SCANCODE_APOSTROPHE },
	{ "`", DEV_KEYBOARD, 0x29, CT_KEY, SDL_SCANCODE_GRAVE },
	{ "LSHIFT", DEV_KEYBOARD, 0x2A, CT_KEY, SDL_SCANCODE_LSHIFT },
	{ "\\", DEV_KEYBOARD, 0x2B, CT_KEY, SDL_SCANCODE_BACKSLASH },
	{ "Z", DEV_KEYBOARD, 0x2C, CT_KEY, SDL_SCANCODE_Z },
	{ "X", DEV_KEYBOARD, 0x2D, CT_KEY, SDL_SCANCODE_X },
	{ "C", DEV_KEYBOARD, 0x2E, CT_KEY, SDL_SCANCODE_C },
	{ "V", DEV_KEYBOARD, 0x2F, CT_KEY, SDL_SCANCODE_V },
	{ "B", DEV_KEYBOARD, 0x30, CT_KEY, SDL_SCANCODE_B },
	{ "N", DEV_KEYBOARD, 0x31, CT_KEY, SDL_SCANCODE_N },
	{ "M", DEV_KEYBOARD, 0x32, CT_KEY, SDL_SCANCODE_M },
	{ ",", DEV_KEYBOARD, 0x33, CT_KEY, SDL_SCANCODE_COMMA },
	{ ".", DEV_KEYBOARD, 0x34, CT_KEY, SDL_SCANCODE_PERIOD },
	{ "/", DEV_KEYBOARD, 0x35, CT_KEY, SDL_SCANCODE_SLASH },
	{ "RSHIFT", DEV_KEYBOARD, 0x36, CT_KEY, SDL_SCANCODE_RSHIFT },
	{ "NUM_MULTIPLY", DEV_KEYBOARD, 0x37, CT_KEY, SDL_SCANCODE_KP_MULTIPLY },
	{ "LALT", DEV_KEYBOARD, 0x38, CT_KEY, SDL_SCANCODE_LALT },
	{ "SPACE", DEV_KEYBOARD, 0x39, CT_KEY, SDL_SCANCODE_SPACE },
	{ "CAPITAL", DEV_KEYBOARD, 0x3A, CT_KEY, SDL_SCANCODE_CAPSLOCK },
	{ "F1", DEV_KEYBOARD, 0x3B, CT_KEY, SDL_SCANCODE_F1 },
	{ "F2", DEV_KEYBOARD, 0x3C, CT_KEY, SDL_SCANCODE_F2 },
	{ "F3", DEV_KEYBOARD, 0x3D, CT_KEY, SDL_SCANCODE_F3 },
	{ "F4", DEV_KEYBOARD, 0x3E, CT_KEY, SDL_SCANCODE_F4 },
	{ "F5", DEV_KEYBOARD, 0x3F, CT_KEY, SDL_SCANCODE_F5 },
	{ "F6", DEV_KEYBOARD, 0x40, CT_KEY, SDL_SCANCODE_F6 },
	{ "F7", DEV_KEYBOARD, 0x41, CT_KEY, SDL_SCANCODE_F7 },
	{ "F8", DEV_KEYBOARD, 0x42, CT_KEY, SDL_SCANCODE_F8 },
	{ "F9", DEV_KEYBOARD, 0x43, CT_KEY, SDL_SCANCODE_F9 },
	{ "F10", DEV_KEYBOARD, 0x44, CT_KEY, SDL_SCANCODE_F10 },
	{ "NUM", DEV_KEYBOARD, 0x45, CT_KEY, SDL_SCANCODE_NUMLOCKCLEAR },
	{ "SCROLL", DEV_KEYBOARD, 0x46, CT_KEY, SDL_SCANCODE_SCROLLLOCK },
	{ "NUM_7", DEV_KEYBOARD, 0x47, CT_KEY, SDL_SCANCODE_KP_7 },
	{ "NUM_8", DEV_KEYBOARD, 0x48, CT_KEY, SDL_SCANCODE_KP_8 },
	{ "NUM_9", DEV_KEYBOARD, 0x49, CT_KEY, SDL_SCANCODE_KP_9 },
	{ "NUM_MINUS", DEV_KEYBOARD, 0x4A, CT_KEY, SDL_SCANCODE_KP_MINUS },
	{ "NUM_4", DEV_KEYBOARD, 0x4B, CT_KEY, SDL_SCANCODE_KP_4 },
	{ "NUM_5", DEV_KEYBOARD, 0x4C, CT_KEY, SDL_SCANCODE_KP_5 },
	{ "NUM_6", DEV_KEYBOARD, 0x4D, CT_KEY, SDL_SCANCODE_KP_6 },
	{ "NUM_PLUS", DEV_KEYBOARD, 0x4E, CT_KEY, SDL_SCANCODE_KP_PLUS },
	{ "NUM_1", DEV_KEYBOARD, 0x4F, CT_KEY, SDL_SCANCODE_KP_1 },
	{ "NUM_2", DEV_KEYBOARD, 0x50, CT_KEY, SDL_SCANCODE_KP_2 },
	{ "NUM_3", DEV_KEYBOARD, 0x51, CT_KEY, SDL_SCANCODE_KP_3 },
	{ "NUM_0", DEV_KEYBOARD, 0x52, CT_KEY, SDL_SCANCODE_KP_0 },
	{ "NUM_PERIOD", DEV_KEYBOARD, 0x53, CT_KEY, SDL_SCANCODE_KP_PERIOD },
	{ "OEM_102", DEV_KEYBOARD, 0x56, CT_KEY, SDL_SCANCODE_NONUSBACKSLASH },
	{ "F11", DEV_KEYBOARD, 0x57, CT_KEY, SDL_SCANCODE_F11 },
	{ "F12", DEV_KEYBOARD, 0x58, CT_KEY, SDL_SCANCODE_F12 },
	{ "F13", DEV_KEYBOARD, 0x64, CT_KEY, SDL_SCANCODE_F13 },
	{ "F14", DEV_KEYBOARD, 0x65, CT_KEY, SDL_SCANCODE_F14 },
	{ "F15", DEV_KEYBOARD, 0x66, CT_KEY, SDL_SCANCODE_F15 },
	{ "KANA", DEV_KEYBOARD, 0x70, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "ABNT_C1", DEV_KEYBOARD, 0x73, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "CONVERT", DEV_KEYBOARD, 0x79, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "NOCONVERT", DEV_KEYBOARD, 0x7B, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "YEN", DEV_KEYBOARD, 0x7D, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "ABNT_C2", DEV_KEYBOARD, 0x7E, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "NUM_EQUALS", DEV_KEYBOARD, 0x8D, CT_KEY, SDL_SCANCODE_KP_EQUALS },
	{ "PREV_TRACK", DEV_KEYBOARD, 0x90, CT_KEY, SDL_SCANCODE_AUDIOPREV },
	{ "AT", DEV_KEYBOARD, 0x91, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "COLON", DEV_KEYBOARD, 0x92, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "UNDERLINE", DEV_KEYBOARD, 0x93, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "KANJI", DEV_KEYBOARD, 0x94, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "STOP", DEV_KEYBOARD, 0x95, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "AX", DEV_KEYBOARD, 0x96, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "UNLABELED", DEV_KEYBOARD, 0x97, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "NEXT_TRACK", DEV_KEYBOARD, 0x99, CT_KEY, SDL_SCANCODE_AUDIONEXT },
	{ "NUM_ENTER", DEV_KEYBOARD, 0x9C, CT_KEY, SDL_SCANCODE_KP_ENTER },
	{ "RCTRL", DEV_KEYBOARD, 0x9D, CT_KEY, SDL_SCANCODE_RCTRL },
	{ "MUTE", DEV_KEYBOARD, 0xA0, CT_KEY, SDL_SCANCODE_AUDIOMUTE },
	{ "CALCULATOR", DEV_KEYBOARD, 0xA1, CT_KEY, SDL_SCANCODE_CALCULATOR },
	{ "PLAY", DEV_KEYBOARD, 0xA2, CT_KEY, SDL_SCANCODE_AUDIOPLAY },
	{ "MEDIA_STOP", DEV_KEYBOARD, 0xA4, CT_KEY, SDL_SCANCODE_AUDIOSTOP },
	{ "VOL_DOWN", DEV_KEYBOARD, 0xAE, CT_KEY, SDL_SCANCODE_VOLUMEDOWN },
	{ "VOL_UP", DEV_KEYBOARD, 0xB0, CT_KEY, SDL_SCANCODE_VOLUMEUP },
	{ "WEB_HOME", DEV_KEYBOARD, 0xB2, CT_KEY, SDL_SCANCODE_AC_HOME },
	{ "NUM_COMMA", DEV_KEYBOARD, 0xB3, CT_KEY, SDL_SCANCODE_KP_COMMA },
	{ "NUM_DIVIDE", DEV_KEYBOARD, 0xB5, CT_KEY, SDL_SCANCODE_KP_DIVIDE },
	{ "SYSRQ", DEV_KEYBOARD, 0xB7, CT_KEY, SDL_SCANCODE_PRINTSCREEN },
	{ "RALT", DEV_KEYBOARD, 0xB8, CT_KEY, SDL_SCANCODE_RALT },
	{ "PAUSE", DEV_KEYBOARD, 0xC5, CT_KEY, SDL_SCANCODE_PAUSE },
	{ "HOME", DEV_KEYBOARD, 0xC7, CT_KEY, SDL_SCANCODE_HOME },
	{ "UP", DEV_KEYBOARD, 0xC8, CT_KEY, SDL_SCANCODE_UP },
	{ "PG_UP", DEV_KEYBOARD, 0xC9, CT_KEY, SDL_SCANCODE_PAGEUP },
	{ "LEFT", DEV_KEYBOARD, 0xCB, CT_KEY, SDL_SCANCODE_LEFT },
	{ "RIGHT", DEV_KEYBOARD, 0xCD, CT_KEY, SDL_SCANCODE_RIGHT },
	{ "END", DEV_KEYBOARD, 0xCF, CT_KEY, SDL_SCANCODE_END },
	{ "DOWN", DEV_KEYBOARD, 0xD0, CT_KEY, SDL_SCANCODE_DOWN },
	{ "PG_DOWN", DEV_KEYBOARD, 0xD1, CT_KEY, SDL_SCANCODE_PAGEDOWN },
	{ "INSERT", DEV_KEYBOARD, 0xD2, CT_KEY, SDL_SCANCODE_INSERT },
	{ "DELETE", DEV_KEYBOARD, 0xD3, CT_KEY, SDL_SCANCODE_DELETE },
	{ "LWIN", DEV_KEYBOARD, 0xDB, CT_KEY, SDL_SCANCODE_LGUI },
	{ "RWIN", DEV_KEYBOARD, 0xDC, CT_KEY, SDL_SCANCODE_RGUI },
	{ "APP_MENU", DEV_KEYBOARD, 0xDD, CT_KEY, SDL_SCANCODE_APPLICATION },
	{ "POWER", DEV_KEYBOARD, 0xDE, CT_KEY, SDL_SCANCODE_POWER },
	{ "SLEEP", DEV_KEYBOARD, 0xDF, CT_KEY, SDL_SCANCODE_SLEEP },
	{ "WAKE", DEV_KEYBOARD, 0xE3, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "WEB_SEARCH", DEV_KEYBOARD, 0xE5, CT_KEY, SDL_SCANCODE_AC_SEARCH },
	{ "WEB_FAVOR", DEV_KEYBOARD, 0xE6, CT_KEY, SDL_SCANCODE_AC_BOOKMARKS },
	{ "WEB_REFRESH", DEV_KEYBOARD, 0xE7, CT_KEY, SDL_SCANCODE_AC_REFRESH },
	{ "WEB_STOP", DEV_KEYBOARD, 0xE8, CT_KEY, SDL_SCANCODE_AC_STOP },
	{ "WEB_FORWARD", DEV_KEYBOARD, 0xE9, CT_KEY, SDL_SCANCODE_AC_FORWARD },
	{ "WEB_BACK", DEV_KEYBOARD, 0xEA, CT_KEY, SDL_SCANCODE_AC_BACK },
	{ "MYCOMPUTER", DEV_KEYBOARD, 0xEB, CT_KEY, SDL_SCANCODE_COMPUTER },
	{ "MAIL", DEV_KEYBOARD, 0xEC, CT_KEY, SDL_SCANCODE_MAIL },
	{ "MEDIA_SELECT", DEV_KEYBOARD, 0xED, CT_KEY, SDL_SCANCODE_MEDIASELECT },
	{ "MOUSE_AXIS_X", DEV_MOUSE,  0, CT_AXIS, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_AXIS_Y", DEV_MOUSE,  4, CT_AXIS, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_AXIS_Z", DEV_MOUSE,  8, CT_AXIS, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON0", DEV_MOUSE, 12, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON1", DEV_MOUSE, 13, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON2", DEV_MOUSE, 14, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON3", DEV_MOUSE, 15, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON4", DEV_MOUSE, 16, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON5", DEV_MOUSE, 17, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON6", DEV_MOUSE, 18, CT_KEY, SDL_SCANCODE_UNKNOWN },
	{ "MOUSE_BUTTON7", DEV_MOUSE, 19, CT_KEY, SDL_SCANCODE_UNKNOWN },
};
static const int N_CONTROLS = sizeof( controlList ) / sizeof( controlList[0] );

////////////////////////////////////////////////////////////////////////////////////////////////////
static std::deque<SMessage> messages;
static std::unordered_map<std::string, int> nameIDs;        // "ESC" -> action id
static std::unordered_map<int, const SControlInfo*> actionInfos;
static const SControlInfo *scancodeControls[SDL_NUM_SCANCODES];
static bool bInitialized = false;
static bool bHasFocus = true;

////////////////////////////////////////////////////////////////////////////////////////////////////
static int MakeActionID( const SControlInfo &info )
{
	return INPUT_KEYID( info.nDevice, info.nOffset );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// Temporary instrumentation (S2_INPUT_TRACE=1) -- prints what reaches the
// binding layer. Not compiled out; costs one getenv on first use.
static bool IsTracing()
{
	static int nTrace = -1;
	if ( nTrace < 0 )
	{
		const char *psz = getenv( "S2_INPUT_TRACE" );
		nTrace = ( psz && *psz == '1' ) ? 1 : 0;
	}
	return nTrace != 0;
}

static void PostMessage( EControlType cType, int nAction, int nParam, bool bState )
{
	if ( IsTracing() )
		printf( "INPUT: type=%d action=0x%08X param=%d state=%d\n", (int)cType, nAction, nParam, (int)bState );
	SMessage sMessage;
	sMessage.nAction = nAction;
	sMessage.ePOVAxis = PA_UNKNOWN;
	sMessage.cType = cType;
	sMessage.nParam = nParam;
	sMessage.bState = bState;
	sMessage.tTime = GetTickCount();
	messages.push_back( sMessage );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool InitInput( HWND, bool, int )
{
	if ( bInitialized )
		return true;

	nameIDs.clear();
	actionInfos.clear();
	for ( int i = 0; i < SDL_NUM_SCANCODES; ++i )
		scancodeControls[i] = 0;

	for ( int i = 0; i < N_CONTROLS; ++i )
	{
		const SControlInfo &info = controlList[i];
		const int nAction = MakeActionID( info );
		nameIDs[info.pszName] = nAction;
		actionInfos[nAction] = &info;
		if ( info.scancode != SDL_SCANCODE_UNKNOWN )
			scancodeControls[info.scancode] = &info;
	}

	bInitialized = true;
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool DoneInput()
{
	messages.clear();
	nameIDs.clear();
	actionInfos.clear();
	bInitialized = false;
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void PumpMessages( bool bFocus )
{
	// SDL's event queue is drained by the windowing layer (WinFrameSDL2.cpp),
	// which forwards what we need through the Notify* entry points below. This
	// only tracks focus, the way the DirectInput version re-acquires devices.
	bHasFocus = bFocus;
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
	PostMessage( cType, -1, nParam, true );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//  Entry points for the windowing layer (Game/WinFrameSDL2.cpp).
////////////////////////////////////////////////////////////////////////////////////////////////////
void NotifyKey( int nScancode, bool bDown )
{
	if ( !bInitialized || nScancode < 0 || nScancode >= SDL_NUM_SCANCODES )
		return;
	const SControlInfo *pInfo = scancodeControls[nScancode];
	if ( !pInfo )
		return;   // key the engine has no name for -- nothing can be bound to it
	PostMessage( CT_KEY, MakeActionID( *pInfo ), 0, bDown );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NotifyMouseButton( int nButton, bool bDown )
{
	if ( !bInitialized || nButton < 0 || nButton > 7 )
		return;
	PostMessage( CT_KEY, INPUT_KEYID( DEV_MOUSE, DIMOFS_BUTTON_BASE + nButton ), 0, bDown );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// nDeltaX/nDeltaY are relative motion, which is what CT_AXIS means (see the
// header note). Zero deltas are dropped rather than queued as no-ops.
void NotifyMouseMotion( int nDeltaX, int nDeltaY )
{
	if ( !bInitialized )
		return;
	if ( nDeltaX )
		PostMessage( CT_AXIS, INPUT_KEYID( DEV_MOUSE, DIMOFS_X ), nDeltaX, true );
	if ( nDeltaY )
		PostMessage( CT_AXIS, INPUT_KEYID( DEV_MOUSE, DIMOFS_Y ), nDeltaY, true );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// DirectInput reports the wheel in WHEEL_DELTA (120) units per notch; SDL
// reports notches, so scale to keep bindings behaving the same.
void NotifyMouseWheel( int nNotches )
{
	if ( !bInitialized || !nNotches )
		return;
	PostMessage( CT_AXIS, INPUT_KEYID( DEV_MOUSE, DIMOFS_Z ), nNotches * 120, true );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool GetCharForKey( int nVirtualKey, WCHAR *pwcChar )
{
	// Used by the key-binding UI to label a key. SDL names keys by keycode;
	// single-character names are exactly the printable ones.
	if ( !pwcChar )
		return false;
	const SDL_Keycode key = SDL_GetKeyFromScancode( (SDL_Scancode)nVirtualKey );
	if ( key == SDLK_UNKNOWN )
		return false;
	const char *pszName = SDL_GetKeyName( key );
	if ( !pszName || !pszName[0] || pszName[1] )
		return false;
	*pwcChar = (WCHAR)(unsigned char)pszName[0];
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool GetKeyForMessage( const SMessage &mMsg, int *pnVirtualKey )
{
	if ( !pnVirtualKey )
		return false;
	std::unordered_map<int, const SControlInfo*>::const_iterator i = actionInfos.find( mMsg.nAction );
	if ( i == actionInfos.end() || i->second->scancode == SDL_SCANCODE_UNKNOWN )
		return false;
	*pnVirtualKey = i->second->scancode;
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
int GetControlID( const string &sCommand )
{
	std::unordered_map<std::string, int>::const_iterator i = nameIDs.find( sCommand );
	return i == nameIDs.end() ? -1 : i->second;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void GetControlInfo( int nAction, EControlType *pcType, float *pfGranularity )
{
	std::unordered_map<int, const SControlInfo*>::const_iterator i = actionInfos.find( nAction );
	if ( i == actionInfos.end() )
	{
		*pcType = CT_UNKNOWN;
		*pfGranularity = 1.0f;
		return;
	}
	*pcType = i->second->cType;
	// Input.cpp asks the device for DIPROP_GRANULARITY and gets 1 for mouse axes
	// (the wheel reports 120, but it divides by granularity and we already scale
	// notches by 120 above, so 1 keeps one notch == one unit either way).
	*pfGranularity = 1.0f;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// Input recording/playback -- a debug facility in retail, not used by the game.
void StartSaveInput( CDataStream * ) {}
void StopSaveInput() {}
void StartEmulateInput( CDataStream * ) {}
void StopEmulateInput() {}
////////////////////////////////////////////////////////////////////////////////////////////////////
}
