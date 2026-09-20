// ============================================================================
//  lsstub.cpp -- no-op LifeStudio:Head API for the Linux build.
//
//  LifeStudio:Head is a proprietary 2004 middleware shipped as a pair of
//  Win32 DLLs (LifeStudioHeadAPI.dll, GDPFile.dll). Only the interoperability
//  HEADERS exist in this tree -- the binaries do not, and there is no Linux
//  build of them. This translation unit supplies the factory entry points
//  those headers declare.
//
//  Compiled INSTEAD OF src/lsglue.cpp on non-Windows builds (see
//  CMakeLists.txt); lsglue.cpp itself is untouched.
//
//  WHY OBJECTS AND NOT NULL: this stub first returned null from every
//  Create(). That is NOT safe. The engine checks the result in some places but
//  dereferences it immediately in others, e.g. Main/LSHead.cpp:68
//      pValue->pLSAnimators[i] = IAnimator::Create();
//      pValue->pLSAnimators[i]->Load( ... );          // <- straight through
//  which segfaulted as soon as the FaceGen character screen built a head. The
//  try/catch around that block catches C++ exceptions, not a null dereference,
//  so it did not help.
//
//  So every factory hands back a shared do-nothing instance instead: calls
//  land somewhere harmless, loads report failure, counts come back zero, and
//  the engine's "this head has no rig" paths take over. The instances are
//  stateless namespace-scope objects, so lifetime and threading are non-issues
//  and Destroy() is deliberately a no-op -- the engine may call it more than
//  once for what it believes are distinct objects.
//
//  VISIBLE CONSEQUENCE: faces do not morph. Heads render with their baked base
//  mesh -- no FaceGen deformation, no facial animation. Everything else is
//  unaffected. Replacing this with real facial animation means finding a Linux
//  build of the middleware (unlikely -- long discontinued) or reimplementing
//  the morph/sequencer maths against these interfaces. See docs/linux-port.md.
// ============================================================================
// LIFESTUDIOHEADAPI_EXPORTS_LIB (makes LIFESTUDIOHEADAPI_API expand to nothing)
// is set by CMake for every consumer of lifestudio_shim, so the declarations
// here and in Main agree. __stdcall is neutralised by the Win32 shim, which
// StdAfx.h pulls in -- included directly here since this file has no StdAfx.
#include "../../../Misc/PlatformCompat.h"
#include "LifeStudioHeadAPI.h"
#include "LifeStudioHeadAPIMMTS.h"
#include "LifeStudioHeadAPITransform.h"
#include "LifeStudioHeadAPIGDP.h"
#include "LifeStudioHeadAPIInit.h"

namespace LifeStudioHeadAPI
{
namespace
{

struct CNullAnimator : public IAnimator
{
	bool Load( const char *, int ) { return false; }
	int SaveBufferSize() { return 0; }
	bool Save( char * ) { return false; }
	IMuscle *MuscleByName( const char * ) { return 0; }
	IMuscle *Muscle( int ) { return 0; }
	int MusclesCount() const { return 0; }
	IBone *BoneByName( const char * ) { return 0; }
	IBone *Bone( int ) { return 0; }
	IBone *BoneByType( unsigned long, IBone * ) { return 0; }
	int BonesCount() const { return 0; }
	void FillUnused( bool ) {}
	bool FillUnused() const { return false; }
	// false = "nothing was morphed", leaving the caller's vertex array as the
	// base mesh it already holds.
	bool Process( float *, int ) { return false; }
	int VerticesCount() const { return 0; }
	void ClearAllMacroMuscles() {}
	void AddMacroMuscle( IMacroMuscle *, float ) {}
	void MultMacroMuscle( IMacroMuscle *, float ) {}
	void ComputePhysics() {}
	void RegisterMacroMuscle( IMacroMuscle * ) {}
	void UnregisterMacroMuscle( IMacroMuscle * ) {}
	void ClearAllRegistration() {}
	void CollectUserItems( bool ) {}
	bool CollectUserItems() const { return false; }
	UserID UserItem( const char * ) { return 0; }
	int UserValuesCount( UserID ) { return 0; }
	float UserValue( UserID, int ) { return 0.0f; }
	void ClearUserItems() {}
	void ComputeBonesHierarchy() {}
	bool HasNeck() const { return false; }
	void NeckProcessing2( bool ) {}
	bool NeckProcessing2() const { return false; }
	IAnimator *Clone();
	void Destroy() {}
};

struct CNullTransformer : public ITransformer
{
	// --- IAnimator ---
	bool Load( const char *, int ) { return false; }
	int SaveBufferSize() { return 0; }
	bool Save( char * ) { return false; }
	IMuscle *MuscleByName( const char * ) { return 0; }
	IMuscle *Muscle( int ) { return 0; }
	int MusclesCount() const { return 0; }
	IBone *BoneByName( const char * ) { return 0; }
	IBone *Bone( int ) { return 0; }
	IBone *BoneByType( unsigned long, IBone * ) { return 0; }
	int BonesCount() const { return 0; }
	void FillUnused( bool ) {}
	bool FillUnused() const { return false; }
	bool Process( float *, int ) { return false; }
	int VerticesCount() const { return 0; }
	void ClearAllMacroMuscles() {}
	void AddMacroMuscle( IMacroMuscle *, float ) {}
	void MultMacroMuscle( IMacroMuscle *, float ) {}
	void ComputePhysics() {}
	void RegisterMacroMuscle( IMacroMuscle * ) {}
	void UnregisterMacroMuscle( IMacroMuscle * ) {}
	void ClearAllRegistration() {}
	void CollectUserItems( bool ) {}
	bool CollectUserItems() const { return false; }
	UserID UserItem( const char * ) { return 0; }
	int UserValuesCount( UserID ) { return 0; }
	float UserValue( UserID, int ) { return 0.0f; }
	void ClearUserItems() {}
	void ComputeBonesHierarchy() {}
	bool HasNeck() const { return false; }
	void NeckProcessing2( bool ) {}
	bool NeckProcessing2() const { return false; }
	IAnimator *Clone();
	void Destroy() {}
	// --- ITransformer ---
	// false here is what makes LSHead.cpp's
	// `if ( pTexTransformer && pTexAnimator && pTexTransformer->Load( pObj ) )`
	// take the "no rig" branch instead of building one.
	bool Load( ITransformerInput * ) { return false; }
	void OutputAnimator( IAnimator * ) {}
	IAnimator *OutputAnimator() const;
	void Generate() {}
};

struct CNullMMTree : public IMMTree
{
	bool Load( const char * ) { return false; }
	bool Load( const char *, int ) { return false; }
	IMacroMuscle *RootMacroMuscle() const { return 0; }
	IMacroMuscle *FindMacroMuscle( const char * ) { return 0; }
	void Destroy() {}
};

struct CNullSequencer : public ISequencer
{
	bool Load( const char * ) { return false; }
	bool Load( const char *, int ) { return false; }
	IMMTree *RegisterMMTree( IMMTree * ) { return 0; }
	int SequenceTime() const { return 0; }
	int TracksCount() const { return 0; }
	int EnumerateMacroMuscles( MUSCLE_CB, void * ) { return 0; }
	int EnumerateMacroMuscles( int, MUSCLE_EXPR_CB, void * ) { return 0; }
	int EnumerateMacroMuscles( MUSCLE_NAME_CB, void * ) { return 0; }
	int EnumerateMacroMuscles( int, MUSCLE_NAME_EXPR_CB, void * ) { return 0; }
	int EnumerateSounds( SOUND_CB, void * ) { return 0; }
	int EnumerateSounds( int, SOUND_TIME_CB, void * ) { return 0; }
	void RenderMacroMuscles( IAnimator *, int ) {}
	void Destroy() {}
};

// ObjectsCount() == 0 means the engine's object loop never runs, so no
// IGDPObject is ever handed out and none needs implementing.
struct CNullGDPFile : public IGDPFile
{
	int ObjectsCount() const { return 0; }
	const char *ObjectName( int ) const { return ""; }
	IGDPObject *Object( int ) { return 0; }
	void Destroy() {}
};

// Stateless singletons -- see the banner.
CNullAnimator g_animator;
CNullTransformer g_transformer;
CNullMMTree g_mmTree;
CNullSequencer g_sequencer;
CNullGDPFile g_gdpFile;

IAnimator *CNullAnimator::Clone() { return &g_animator; }
IAnimator *CNullTransformer::Clone() { return &g_animator; }
IAnimator *CNullTransformer::OutputAnimator() const { return &g_animator; }

}   // anonymous namespace

IAnimator *IAnimator::Create() { return &g_animator; }
IMMTree *IMMTree::Create() { return &g_mmTree; }
ISequencer *ISequencer::Create() { return &g_sequencer; }
ITransformer *ITransformer::Create() { return &g_transformer; }
IGDPFile *IGDPFile::Create( const char * ) { return &g_gdpFile; }

// The real Init() hands the DLL a host curve-evaluation callback; with no DLL
// to talk to there is nothing to do.
void Init() {}

}
