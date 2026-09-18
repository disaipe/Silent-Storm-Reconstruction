// ============================================================================
//  SplashScreenStub.cpp -- no-op implementation of the NSplash:: facade for the
//  Linux build.
//
//  SplashScreen.cpp + SplashScreenDialog.cpp are Windows-only: the splash is a
//  borderless Win32 window built directly on GDI (RegisterClassExA /
//  CreateWindowExA / a DIB + halftone palette blitted from WM_PAINT). None of
//  that has a meaningful stand-in here -- on Linux the window comes from SDL2
//  (Game/WinFrameSDL2.cpp), and a loading splash is cosmetic, so it is simply
//  skipped.
//
//  Compiled INSTEAD OF SplashScreen.cpp + SplashScreenDialog.cpp on non-Windows
//  builds (see CMakeLists.txt); both of those files are untouched.
//
//  Callers treat a false return as "no splash is up", which is exactly the
//  state this reports -- ShowSplashScreen's contract is already "did the window
//  end up live", and the release itself returns false when creation fails.
// ============================================================================
#include "StdAfx.h"
#include "SplashScreen.h"
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace NSplash
{
////////////////////////////////////////////////////////////////////////////////////////////////////
bool ShowSplashScreen( const string &, bool ) { return false; }
bool UpdateSplashScreen() { return false; }
void HideSplashScreen() {}
////////////////////////////////////////////////////////////////////////////////////////////////////
} // namespace NSplash
