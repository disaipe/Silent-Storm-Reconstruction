// ============================================================================
//  MainLinux.cpp -- Linux entry point (main()), mirroring Game/Main.cpp's
//  WinMain structure and init order. Windows-only harness/diagnostic features
//  (crash backtraces via dbghelp, the frame-polled harness command channel,
//  the extended -loadslot/-harness flags) are dropped -- everything else
//  (DB load, window/render/sound/input init, config, the main loop, shutdown)
//  mirrors WinMain's shape closely so behavior stays recognisable.
//
//  Compiled INSTEAD OF Main.cpp + WinFrame.cpp on non-Windows builds (see
//  CMakeLists.txt); Main.cpp/WinFrame.cpp are untouched.
// ============================================================================
#include "StdAfx.h"
#include "../Main/GInit.h"
#include "WinFrame.h"
// Implemented in WinFrameSDL2.cpp. Declared here rather than in WinFrame.h,
// which is shared with the Windows build and has no such entry point.
namespace NWinFrame { void DoneApplication(); }
#include "../Main/iMain.h"
#include "../Input/Bind.h"
#include "../ADOImport/BasicDB.h"
#include "../DBFormat/DataMap.h"
#include "../Misc/StrProc.h"
#include "../MiscDll/Commands.h"
#include "../Main/GResource.h"
#include "../Main/iInterMission.h"
#include "../Main/iLoading.h"
#include "../Misc/HPTimer.h"
#include "../Main/iSaveManager.h"
#include "../Main/Sound.h"
#include "../Main/WinInputConv.h"
#include "../FileIO/BasicChunk1.h"
////////////////////////////////////////////////////////////////////////////////////////////////////
int main( int argc, char *argv[] )
{
	srand( GetTickCount() );

	NGScene::AddResourceDir( "./res" );
	NGScene::RunResourceLoadingThread();

	// load game database
	try
	{
		CFileStream f;
		f.OpenRead( "game.db" );
		NDatabase::Serialize( f, CStructureSaver::READ );
	}
	catch (...)
	{
		fprintf( stderr, "File game.db not found\n" );
		return 0;
	}

	// init subsystems
	if ( !NWinFrame::InitApplication( 0, "Silent Storm", "Silent Storm" ) )
		return 0;
	if ( !NGfx::Init3D( NWinFrame::GetWnd() ) )
	{
		fprintf( stderr, "Failed to initialize the renderer\n" );
		return 0;
	}
	if ( !NSound::InitSound( NWinFrame::GetWnd() ) )
	{
		fprintf( stderr, "Failed to initialize sound\n" );
		return 0;
	}
	if ( !NInput::InitInput( NWinFrame::GetWnd() ) )
	{
		fprintf( stderr, "Failed to initialize input\n" );
		return 0;
	}

	// load config & process params
	NGlobal::LoadConfig( "./cfg/autoexec.cfg" );

	bool bDoLoad = false;
	string szLoadSlot;
	string szCfg( "start.cfg" );
	for ( int i = 1; i < argc; ++i )
	{
		string sArg( argv[i] );
		if ( sArg == "-fullscreen" )
			NGlobal::SetVar( "gfx_fullscreen", 1 );
		else if ( sArg == "-windowed" )
			NGlobal::SetVar( "gfx_fullscreen", 0 );
		else if ( sArg == "-nosound" )
		{
			NGlobal::SetVar( "sound_mode", 0 );
			NGlobal::SetVar( "sound_init", 0 );
		}
		else if ( sArg == "-noai" )
			NGlobal::SetVar( "game_noai", 1 );
		else if ( sArg == "-load" )
			bDoLoad = true;
		else if ( sArg == "-cfg" && i + 1 < argc )
			szCfg = argv[++i];
	}

	if ( !NGScene::SetModeFromConfig() )
	{
		fprintf( stderr, "Failed to set display mode\n" );
		return 0;
	}
	if ( !NSound::SetModeFromConfig() )
	{
		fprintf( stderr, "Failed to set sound mode\n" );
		return 0;
	}

	NGame::InitLoadingScreen();
	if ( bDoLoad )
		NMainLoop::Command( new NMainLoop::CICLoad( szLoadSlot.empty() ? NMainLoop::GetQuickSaveSlot( true ) : szLoadSlot ) );
	else
		NMainLoop::Command( new CICInterMission( szCfg ) );

	SWinToInputMessageConverter sWinInputConv;
	for (;;)
	{
		NWinFrame::PumpMessages();
		bool bActive = NWinFrame::IsAppActive();
		NInput::PumpMessages( bActive );
		sWinInputConv.Do();
		if ( NWinFrame::IsExit() )
			break;
		if ( !NMainLoop::StepApp( bActive, bActive ) )
			break;
		NHPTimer::UpdateHPTimerFrequency();
		if ( !bActive )
			Sleep( 40 );
	}

	NGlobal::SaveConfig( "./cfg/config.cfg" );
	NMainLoop::DoneInterface();
	NGfx::Done3D();
	NInput::DoneInput();
	NSound::DoneSound();
	// Linux-only: hands the display mode back. WinFrame.h has no counterpart
	// because Windows does this for us when the process exits.
	NWinFrame::DoneApplication();
	return 0;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
