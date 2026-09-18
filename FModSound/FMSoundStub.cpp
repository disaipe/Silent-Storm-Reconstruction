// ============================================================================
//  FMSoundStub.cpp -- no-op implementation of the NFMSound:: API for the Linux
//  build. FMSound.cpp (the real FMOD-backed implementation) is Windows-only:
//  FMOD 3.70 ("fmodvc") has no readily obtainable Linux build of this vintage
//  (see docs/linux-port.md, "Разбор этапа «Звук»"). Compiled INSTEAD OF
//  FMSound.cpp on non-Windows builds (see CMakeLists.txt) -- FMSound.cpp
//  itself is untouched.
//
//  The class hierarchy (CSample2D/CSample3D/CSound2D/CSound3D/CStream) mirrors
//  FMSound.cpp's real one closely enough that CObj<>/CPtr<>/CPtrFuncBase<>
//  callers elsewhere (Main/Sound.cpp, SoundFormat.cpp, GBinkPlayer.cpp) work
//  unchanged -- they only ever go through the NFMSound:: functions below, never
//  touch class members directly.
// ============================================================================
#include "StdAfx.h"
#include "FMsound.h"

namespace NFMSound
{
////////////////////////////////////////////////////////////////////////////////////////////////////
CDriversInfo drivers;
////////////////////////////////////////////////////////////////////////////////////////////////////
class CSample2D: public CObjectBase
{
	OBJECT_BASIC_METHODS( CSample2D );
};
////////////////////////////////////////////////////////////////////////////////////////////////////
class CSample3D: public CObjectBase
{
	OBJECT_BASIC_METHODS( CSample3D );
};
////////////////////////////////////////////////////////////////////////////////////////////////////
class CSound2D: public CObjectBase
{
	OBJECT_BASIC_METHODS( CSound2D );
};
////////////////////////////////////////////////////////////////////////////////////////////////////
class CSound3D: public CObjectBase, public ISound3D
{
	OBJECT_BASIC_METHODS( CSound3D );
public:
	virtual void SetPosition( const CVec3 & ) {}
};
////////////////////////////////////////////////////////////////////////////////////////////////////
class CStream: public CObjectBase
{
	OBJECT_NOCOPY_METHODS( CStream );
};
////////////////////////////////////////////////////////////////////////////////////////////////////
BASIC_REGISTER_CLASS( CSample2D )
BASIC_REGISTER_CLASS( CSample3D )
BASIC_REGISTER_CLASS( CSound2D )
BASIC_REGISTER_CLASS( CSound3D )
BASIC_REGISTER_CLASS( CStream )
////////////////////////////////////////////////////////////////////////////////////////////////////
bool SearchDevices() { return true; }
bool Init( const SStartInfo & ) { return true; }
void Done() {}
bool IsInitialized() { return false; }
void* GetSoundAPI() { return 0; }
void Update( const SListener & ) {}
////////////////////////////////////////////////////////////////////////////////////////////////////
CSample2D* LoadSample2D( const void *, int ) { return new CSample2D(); }
CSample3D* LoadSample3D( const void *, int, float, float, int, int, int ) { return new CSample3D(); }
CSample3D* GetDefault3DSound() { return new CSample3D(); }
////////////////////////////////////////////////////////////////////////////////////////////////////
CSound2D* PlaySound( CSample2D *, int, int, int, bool ) { return 0; }
CSound3D* Play3DSound( const SPlayParams & ) { return 0; }
CStream* PlayStream( const char *, bool, int, bool, float ) { return 0; }
CStream* SwitchStream( CStream *, const char *, bool, float ) { return 0; }
////////////////////////////////////////////////////////////////////////////////////////////////////
bool IsPlaying( CStream * ) { return false; }
unsigned long GetStreamTime( CStream * ) { return 0xFFFFFFFFu; }
bool IsPlaying( CSound2D * ) { return false; }
bool IsPlaying( CSound3D * ) { return false; }
void Pause( CSound2D *, bool ) {}
void Pause( CSound3D *, bool ) {}
void FadeOut( CStream *, float ) {}
void CancelFadeOut( CStream * ) {}
////////////////////////////////////////////////////////////////////////////////////////////////////
void SetSFXMasterVolume( int ) {}
void SetMusicMasterVolume( int ) {}
void SetSpeakerType( ESpeakerType ) {}
ESpeakerType GetSpeakerType() { return SOUND_SM_STEREO; }
////////////////////////////////////////////////////////////////////////////////////////////////////
}
