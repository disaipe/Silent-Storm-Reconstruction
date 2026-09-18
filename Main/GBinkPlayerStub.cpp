// ============================================================================
//  GBinkPlayerStub.cpp -- no-op NGScene::IVideoPlayer for the Linux build.
//  GBinkPlayer.cpp (the real Bink-backed implementation) is Windows-only:
//  Bink 1 ("binkw32") has no readily obtainable Linux build of this vintage,
//  and Bink 2 (RAD/Epic's current cross-platform SDK) uses a different API
//  (see docs/linux-port.md, "Разбор этапа «Видео»"). Compiled INSTEAD OF
//  GBinkPlayer.cpp on non-Windows builds (see CMakeLists.txt) --
//  GBinkPlayer.cpp itself is untouched.
//
//  This mirrors CBinkVideoPlayer's own "video failed to open" behavior
//  (hBink == 0: Recalc() leaves pValue unset/invalid and returns), which the
//  real code already tolerates -- so callers (iIntroScreen/iLoading/
//  iCreditsScreen via NUI::CVideoPlayer) need no changes: intro/credits/
//  loading-screen video is simply skipped, matching the docs/linux-port.md
//  "Вариант C: временно заглушить видео" plan.
// ============================================================================
#include "StdAfx.h"
#include "GBinkPlayer.h"

namespace NGScene
{
////////////////////////////////////////////////////////////////////////////////////////////////////
class CNullVideoPlayer: public IVideoPlayer
{
	OBJECT_NOCOPY_METHODS( CNullVideoPlayer );
protected:
	virtual bool NeedUpdate() { return false; }
	virtual void Recalc() {} // leaves pValue unset -- same as a Bink player whose movie never opened
public:
	virtual void Play( bool ) {}
	virtual bool Stop() { return true; }
	virtual bool Pause( bool ) { return false; }
	virtual bool IsPlaying() { return false; }
	virtual int  GetCurrentFrame() { return 0; }
	virtual void SetCurrentFrame( int ) {}
	virtual int  GetLength() { return 0; }
	virtual int  GetNumFrames() { return 0; }
	virtual void GetSize( CTPoint<int> *pSize ) { if ( pSize ) { pSize->x = 0; pSize->y = 0; } }
};
////////////////////////////////////////////////////////////////////////////////////////////////////
IVideoPlayer* CreateVideoPlayer( const string &, int )
{
	return new CNullVideoPlayer();
}
////////////////////////////////////////////////////////////////////////////////////////////////////
}
