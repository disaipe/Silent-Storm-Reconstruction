// ============================================================================
//  lsstub.cpp -- no-op LifeStudio:Head API for the Linux build.
//
//  LifeStudio:Head is a proprietary 2004 middleware shipped as a pair of
//  Win32 DLLs (LifeStudioHeadAPI.dll, GDPFile.dll). Only the interoperability
//  HEADERS exist in this tree -- the binaries do not, and there is no Linux
//  build of them. This translation unit supplies the handful of factory
//  entry points those headers declare, all returning null.
//
//  Compiled INSTEAD OF src/lsglue.cpp on non-Windows builds (see
//  CMakeLists.txt); lsglue.cpp itself is untouched.
//
//  WHY NULL IS SAFE HERE: the engine already treats a failed Create() as
//  "this head is not morphable" and carries on. Main/LSHead.cpp guards every
//  factory result (`if ( pGDP )`, `if ( pObj && ... )`), tracks success in
//  bMorphOk / bTexRigOk, and wraps the whole rig build in try/catch precisely
//  so a missing rig degrades instead of failing. The visible consequence on
//  Linux is that faces do not morph: heads render with their baked base mesh
//  and no FaceGen deformation or facial animation. Everything else -- units,
//  world, UI -- is unaffected.
//
//  Replacing this with real facial animation means either finding a Linux
//  build of the middleware (unlikely -- it is long discontinued) or
//  reimplementing the morph/sequencer maths against these interfaces.
//  See docs/linux-port.md.
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

IAnimator *IAnimator::Create() { return 0; }
IMMTree *IMMTree::Create() { return 0; }
ISequencer *ISequencer::Create() { return 0; }
ITransformer *ITransformer::Create() { return 0; }
IGDPFile *IGDPFile::Create( const char * ) { return 0; }

// The real Init() hands the DLL a host curve-evaluation callback; with no DLL
// to talk to there is nothing to do.
void Init() {}

}
