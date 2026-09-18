# ============================================================================
#  Silent Storm engine -- CMake build
#
#  Windows (Win32/x86, MSVC): the original, complete target.
#    Configure with:            cmake -S . -B build -A Win32
#    Build with:                cmake --build build --config Release
#
#  Linux (experimental, in progress -- see docs/linux-port.md / docs/linux-build.md):
#    Configure with:            cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
#    Build with:                cmake --build build-linux
#    Only a subset of modules build so far (Misc/FileIO/MiscDll/Image/ADOImport/
#    FModSound/Input/Script/DBFormat) -- Main/Game are gated behind
#    S2_LINUX_FULL_ENGINE (default OFF) because Main/Gfx*.cpp still has raw D3D9
#    calls that need the dxvk-native port (docs/linux-port.md, render section)
#    before they'll compile. Sound and video are no-op stubs on Linux for now.
#
#  Replaces the old A5new.sln. Third-party code lives under third_party/.
# ============================================================================
cmake_minimum_required(VERSION 3.21)
if(WIN32)
  project(A5 CXX C RC)
else()
  project(A5 CXX C)
endif()

if(WIN32)
  if(NOT MSVC)
    message(FATAL_ERROR "MSVC toolset required on Windows.")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Win32/x86 only -- configure with:  cmake -S . -B build -A Win32")
  endif()
elseif(NOT UNIX)
  message(FATAL_ERROR "Only Windows (MSVC) and Linux are supported.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(root "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
  # Dynamic CRT (/MD, /MDd) -- matches the original projects and the committed import libs.
  cmake_policy(SET CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

  # Engine-wide compile settings: __fastcall default (/Gr), big translation units,
  # C++ exceptions, parallel compile, and the common WIN32 defines. externA5 is
  # libpng's API-decoration macro -- 'extern' here (we build libpng statically).
  add_compile_options(/Gr /Zm300 $<$<COMPILE_LANGUAGE:CXX>:/EHsc> /wd4996)  # no /MP: ADOImport uses #import
  add_compile_definitions(
    WIN32 _WINDOWS externA5=extern
    $<$<CONFIG:Debug>:_DEBUG FAST_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

  # The engine's .rc files #include "afxres.h" (an MFC header), but the engine isn't
  # MFC; redirect it to the SDK's non-MFC winres.h via a generated shim on the RC path.
  file(WRITE "${CMAKE_BINARY_DIR}/rc_compat/afxres.h" "#include <winres.h>\n")
else()
  # externA5 stays a no-op here too (Image/libpng linkage doesn't need dllimport
  # decoration on Linux -- everything links statically into one binary).
  add_compile_definitions(
    externA5=extern
    $<$<CONFIG:Debug>:_DEBUG FAST_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
  # This codebase leans on several MSVC-only language extensions (anonymous
  # struct/union members with non-trivial types in Misc/Geom.h; explicit member-
  # template specialization inside a class body in FileIO/Streams.h). GCC/Clang's
  # -fms-extensions permits both without touching the source.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>)
  find_package(SDL2 REQUIRED)
  find_package(Threads REQUIRED)
  option(S2_LINUX_FULL_ENGINE
    "Also configure Main/Game on Linux (needs the dxvk-native render port -- not done yet, will not compile)"
    OFF)
endif()

# ----------------------------------------------------------------------------
#  third_party
# ----------------------------------------------------------------------------
# zlib -- Windows: SHARED, since the engine's zconf.h defaults consumers to
# __declspec(dllimport) (zlib is expected to be a DLL). ZLIB_DLL (this target only)
# = export; consumers see dllimport and resolve against the generated import lib.
# Linux: plain STATIC -- zconf.h's "#if defined(ZLIB_DLL)" branch always expands to
# __declspec(...) regardless of platform (this vendored copy isn't platform-guarded),
# which GCC/Clang don't understand as a bare keyword -- so ZLIB_DLL must stay undefined
# here, which naturally selects zconf.h's "#else define ZEXTERN extern" branch instead.
file(GLOB zlib_src "${root}/third_party/zlib/*.c")
if(WIN32)
  add_library(zlib SHARED ${zlib_src})
  target_compile_definitions(zlib PRIVATE ZLIB_DLL _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
  add_library(zlib STATIC ${zlib_src})
endif()
target_include_directories(zlib PUBLIC "${root}/third_party/zlib")

# libpng (static; the MMX-asm variants are dropped in favour of the C path)
file(GLOB png_src "${root}/third_party/libpng/*.c")
list(FILTER png_src EXCLUDE REGEX "(pnggccrd|pngvcrd)\\.c$")
add_library(libpng STATIC ${png_src})
target_include_directories(libpng PUBLIC "${root}/third_party/libpng")
target_link_libraries(libpng PUBLIC zlib)
target_compile_definitions(libpng PRIVATE _CRT_SECURE_NO_WARNINGS PNG_NO_MMX_CODE)

# libsquish (DXT compression) -- built from source
file(GLOB squish_src "${root}/third_party/squish/squish/*.cpp")
add_library(squish STATIC ${squish_src})
target_include_directories(squish PUBLIC "${root}/third_party/squish" PRIVATE "${root}/third_party/squish/squish")

# Proprietary-DLL interop: Windows-only (the fmod/bink/LifeStudio DLLs, and the .lib/.def
# import-lib generation itself via MSVC's lib.exe, don't apply on Linux -- FModSound and
# Bink are no-op stubs there instead; see FModSound/FMSoundStub.cpp, Main/GBinkPlayerStub.cpp).
if(WIN32)
set(_implib_dir "${CMAKE_BINARY_DIR}/_implibs")
get_filename_component(_vcbin "${CMAKE_C_COMPILER}" DIRECTORY)

# fmod/bink export decorated __stdcall names (_Name@bytes) that `lib /def` can't
# reproduce, so compile a tiny stub DLL whose dllexport __stdcall stubs carry the same
# names and keep the import lib the linker emits. The stub DLL is routed to a side dir
# (NEVER next to Game.exe) so it can never shadow the real runtime DLL.
function(s2_stub_implib target dllname deffile)
  file(STRINGS "${deffile}" _ex REGEX "_[A-Za-z0-9_]+@[0-9]+")
  set(_c "/* generated -- mints the import lib for ${dllname}.dll (do not edit) */\n")
  foreach(_e IN LISTS _ex)
    if(_e MATCHES "_([A-Za-z0-9_]+)@([0-9]+)")
      set(_nm "${CMAKE_MATCH_1}")
      math(EXPR _n "${CMAKE_MATCH_2} / 4")
      set(_p "void")
      if(_n GREATER 0)
        set(_p "")
        foreach(_i RANGE 1 ${_n})
          string(APPEND _p "int a${_i},")
        endforeach()
        string(REGEX REPLACE ",$" "" _p "${_p}")
      endif()
      string(APPEND _c "__declspec(dllexport) int __stdcall ${_nm}(${_p}){return 0;}\n")
    endif()
  endforeach()
  set(_src "${CMAKE_BINARY_DIR}/_implib_gen/${target}.c")
  file(WRITE "${_src}" "${_c}")
  add_library(${target} SHARED "${_src}")
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${dllname}")
  foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY${_cfg} "${_implib_dir}"
      ARCHIVE_OUTPUT_DIRECTORY${_cfg} "${_implib_dir}")
  endforeach()
endfunction()

s2_stub_implib(fmod_stub fmod    "${root}/third_party/fmod/def/fmod.def")
s2_stub_implib(bink_stub binkw32 "${root}/third_party/bink/def/bink.def")

# LifeStudio exports C++ mangled names, which `lib /def` reproduces directly.
set(_ls_implibs "")
foreach(_d LifeStudioHeadAPI GDPFile)
  add_custom_command(OUTPUT "${_implib_dir}/${_d}.lib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_implib_dir}"
    COMMAND "${_vcbin}/lib.exe" /nologo "/def:${root}/third_party/lifestudio/def/${_d}.def"
            "/out:${_implib_dir}/${_d}.lib" /machine:x86
    DEPENDS "${root}/third_party/lifestudio/def/${_d}.def" VERBATIM
    COMMENT "Generating import lib ${_d}.lib")
  list(APPEND _ls_implibs "${_implib_dir}/${_d}.lib")
endforeach()
add_custom_target(ls_implibs DEPENDS ${_ls_implibs})

# LifeStudio Init/Compute glue is real source -> a normal static lib.
add_library(lifestudio_init STATIC "${root}/third_party/lifestudio/src/lsglue.cpp")
target_include_directories(lifestudio_init PRIVATE "${root}/third_party/lifestudio/include")

# The three interop shims: clean-room include dir + the generated import libs.
add_library(fmod_shim INTERFACE)
target_include_directories(fmod_shim INTERFACE "${root}/third_party/fmod/include")
target_link_libraries(fmod_shim INTERFACE fmod_stub)

add_library(bink_shim INTERFACE)
target_include_directories(bink_shim INTERFACE "${root}/third_party/bink/include")
target_link_libraries(bink_shim INTERFACE bink_stub)          # GBinkPlayer.cpp #pragma resolves via this import lib
target_link_directories(bink_shim INTERFACE "${_implib_dir}") # ...and its #pragma comment(lib,"binkw32.lib") by name

add_library(lifestudio_shim INTERFACE)
target_include_directories(lifestudio_shim INTERFACE "${root}/third_party/lifestudio/include")
target_link_libraries(lifestudio_shim INTERFACE
  lifestudio_init "${_implib_dir}/LifeStudioHeadAPI.lib" "${_implib_dir}/GDPFile.lib")
endif() # WIN32

# ----------------------------------------------------------------------------
#  engine modules  (each is a directory of *.cpp + a StdAfx.h PCH)
# ----------------------------------------------------------------------------
# Per-project source lists, baked from the original .vcxproj manifests (the projects
# compiled an explicit list; several dirs also hold dead alternative .cpp, so we never
# glob). Defines <Project>_SRC, paths relative to the project directory.
include("${root}/sources.cmake")

function(s2_module name type)
  # SRC overrides the default ${name}_SRC list (from sources.cmake) -- used on Linux to
  # swap in platform-specific translation units (e.g. InputSDL2.cpp instead of Input.cpp)
  # without touching sources.cmake, which sources.cmake's own header says stays baked from
  # the original .vcxproj manifests.
  cmake_parse_arguments(M "" "" "LINK;DEFINES;WHOLE;SRC" ${ARGN})
  set(src "")
  if(M_SRC)
    set(_names ${M_SRC})
  else()
    set(_names ${${name}_SRC})
  endif()
  # sources.cmake is baked from the original .vcxproj manifests, where file names were
  # matched case-insensitively (Windows). On a case-sensitive filesystem a few entries
  # therefore miss the real file (e.g. Main's "RPGGlobal.cpp" vs the on-disk
  # "rpgGlobal.cpp"). Resolve those by a case-insensitive lookup rather than editing
  # sources.cmake, whose own header says it stays baked from the manifests.
  file(GLOB _ondisk RELATIVE "${root}/${name}" "${root}/${name}/*")
  set(_lower "")
  foreach(_f IN LISTS _ondisk)
    string(TOLOWER "${_f}" _fl)
    set(_ondisk_by_lower_${_fl} "${_f}")
    list(APPEND _lower "${_fl}")
  endforeach()
  foreach(_rel IN LISTS _names)
    if(NOT EXISTS "${root}/${name}/${_rel}")
      string(TOLOWER "${_rel}" _rell)
      if(DEFINED _ondisk_by_lower_${_rell})
        set(_rel "${_ondisk_by_lower_${_rell}}")
      endif()
    endif()
    get_filename_component(_abs "${root}/${name}/${_rel}" ABSOLUTE)
    list(APPEND src "${_abs}")
  endforeach()
  if(NOT src)
    message(FATAL_ERROR "s2_module(${name}): no sources -- missing from sources.cmake?")
  endif()
  if(type STREQUAL "lib")
    add_library(${name} STATIC ${src})
  elseif(type STREQUAL "win32")
    add_executable(${name} WIN32 ${src})
  else()
    add_executable(${name} ${src})
  endif()
  if(WIN32)
    target_include_directories(${name} PRIVATE "${CMAKE_BINARY_DIR}/rc_compat")  # afxres.h -> winres.h
  endif()
  # A case-sensitive filesystem (Linux) needs the on-disk casing exactly -- some module
  # directories have "StdAfx.h", others "stdafx.h" (both existed already; the Linux
  # case-mismatch cleanup normalised #include references but left each module's own
  # historical file casing as-is).
  if(EXISTS "${root}/${name}/StdAfx.h")
    target_precompile_headers(${name} PRIVATE "${root}/${name}/StdAfx.h")
  elseif(EXISTS "${root}/${name}/stdafx.h")
    target_precompile_headers(${name} PRIVATE "${root}/${name}/stdafx.h")
  endif()
  if(M_LINK)
    target_link_libraries(${name} PUBLIC ${M_LINK})
  endif()
  # Whole-archive: pull in ALL objects of these static libs even if no symbol is
  # directly referenced -- the engine self-registers classes via global-ctor side
  # effects (BASIC_REGISTER_CLASS / REGISTER_SAVELOAD_CLASS), which the linker would
  # otherwise discard. (Matches the VS UseLibraryDependencyInputs=true on Main.)
  if(M_WHOLE)
    list(JOIN M_WHOLE "," _wa)
    target_link_libraries(${name} PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_wa}>")
  endif()
  target_compile_definitions(${name} PRIVATE $<$<STREQUAL:${type},lib>:_USRDLL> ${M_DEFINES})
endfunction()

if(WIN32)
# leaves
s2_module(Misc        lib)
# (Scintilla + OpenDynamix are only used by MapEdit, which isn't built -- omitted.)
# core
s2_module(FileIO    lib LINK Misc)
s2_module(MiscDll   lib LINK FileIO Misc DEFINES MISCDLL_EXPORTS)
s2_module(Image     lib LINK zlib libpng squish)
s2_module(ADOImport lib LINK FileIO Misc DEFINES ADOIMPORT_EXPORTS)
s2_module(FModSound lib LINK FileIO Misc fmod_shim)
s2_module(Input     lib LINK FileIO MiscDll Misc dxguid dinput8)
s2_module(Script    lib LINK FileIO Input MiscDll Misc)
s2_module(DBFormat  lib LINK ADOImport FileIO Misc)
s2_module(Main      lib LINK ADOImport DBFormat FileIO FModSound Input MiscDll Misc Script
                            lifestudio_shim bink_shim d3d9 dxguid)
# the game
s2_module(Game win32 WHOLE Main
                     LINK ADOImport DBFormat FileIO FModSound Input MiscDll Misc Script
                          fmod_shim lifestudio_shim bink_shim
                          dxguid dinput8 odbc32 odbccp32 d3d9 user32 Gdi32)
add_dependencies(Game ls_implibs)   # the lib/def import libs must exist before Game links
# tools
s2_module(DataImport     console LINK DBFormat FileIO Misc odbc32 odbccp32 DEFINES _CONSOLE)
s2_module(PkgBuilder     console LINK FileIO Misc odbc32 odbccp32 DEFINES _CONSOLE)
# ShaderCompiler needs legacy D3DX9 (dropped from the modern Windows SDK). Search
# $(DXSDK_DIR) and the standard DirectX SDK installs (June 2010 / August 2007).
# Built only if d3dx9.lib is found; override with -DD3DX9_LIB=<path>.
set(_dx_roots
  "$ENV{DXSDK_DIR}"
  "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)"
  "C:/Program Files (x86)/Microsoft DirectX SDK (August 2007)")
set(_dx_libdirs "")
foreach(_r IN LISTS _dx_roots)
  if(IS_DIRECTORY "${_r}")
    list(APPEND _dx_libdirs "${_r}/Lib/x86")
  endif()
endforeach()
find_library(D3DX9_LIB d3dx9 PATHS ${_dx_libdirs})
if(D3DX9_LIB)
  # The legacy DirectX SDK's Include dir also ships OLD sal.h / rpcsal.h / specstrings.h
  # that shadow the modern Windows SDK's and break its COM headers (objidl.h etc. use
  # __RPC__*_xcount, undefined by the old rpcsal.h). So expose ONLY the DirectX-unique
  # headers to the compiler -- shared headers then resolve against the Windows SDK.
  get_filename_component(_dxroot "${D3DX9_LIB}/../../.." ABSOLUTE)   # .../Lib/x86/d3dx9.lib -> SDK root
  set(_dxinc "${CMAKE_BINARY_DIR}/dxsdk_d3dx9_include")
  file(GLOB _dxhdrs "${_dxroot}/Include/d3dx9*.h" "${_dxroot}/Include/rmxf*.h"
                    "${_dxroot}/Include/dxfile.h" "${_dxroot}/Include/dxerr*.h"
                    "${_dxroot}/Include/d3dx9*.inl")
  file(COPY ${_dxhdrs} DESTINATION "${_dxinc}")
  s2_module(ShaderCompiler console LINK FileIO Misc odbc32 odbccp32 "${D3DX9_LIB}" DEFINES _CONSOLE)
  target_include_directories(ShaderCompiler PRIVATE "${_dxinc}")
  message(STATUS "ShaderCompiler: D3DX9 = ${D3DX9_LIB}")
else()
  message(STATUS "ShaderCompiler skipped: D3DX9 not found (install the DirectX SDK or set DXSDK_DIR env var).")
endif()
s2_module(FontGen        console LINK FileIO Image libpng Misc zlib squish odbc32 odbccp32 DEFINES _CONSOLE)
s2_module(TexConv        console LINK FileIO Image libpng Misc zlib squish DEFINES _CONSOLE)
s2_module(TexMipStrip    console LINK FileIO Image Misc squish DEFINES _CONSOLE)
s2_module(LSConverter  console LINK FileIO Image libpng Misc squish lifestudio_shim DEFINES _CONSOLE)

# Kept on disk but intentionally NOT built (sources preserved for reference):
#   MapEdit      -- needs the proprietary Stingray + LifeMode SDKs; does not compile.
#   Scintilla    -- only used by MapEdit.
#   OpenDynamix  -- only used by MapEdit (ODE physics; would also need ODE headers).

else() # UNIX -- see docs/linux-port.md for the per-subsystem replacement plan.
# leaves
s2_module(Misc lib)
target_link_libraries(Misc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})   # PlatformCompat.cpp uses std::thread and dlopen
# core
s2_module(FileIO    lib LINK Misc)
s2_module(MiscDll   lib LINK FileIO Misc DEFINES MISCDLL_EXPORTS)
s2_module(Image     lib LINK zlib libpng squish)
# ADOImport: BasicDBLinux.cpp instead of BasicDB.cpp (drops the ADO/COM content-import
# pipeline, which nothing calls on Linux anyway -- see docs/linux-port.md, "Разбор этапа «БД»").
s2_module(ADOImport lib SRC BasicDBLinux.cpp StdAfx.cpp LINK FileIO Misc DEFINES ADOIMPORT_EXPORTS)
# FModSound: no-op stub instead of the real FMOD-backed implementation (no obtainable
# Linux build of this FMOD vintage -- see docs/linux-port.md, "Разбор этапа «Звук»").
s2_module(FModSound lib SRC FMSoundStub.cpp StdAfx.cpp LINK FileIO Misc)
# Input: Bind.cpp is shared unchanged (verified hardware-agnostic); InputSDL2.cpp replaces
# the DirectInput8-backed Input.cpp with a minimal stub (see docs/linux-port.md, "Разбор
# этапа «Ввод»" -- full SDL2 device/binding support is tracked separately).
s2_module(Input     lib SRC Bind.cpp InputSDL2.cpp StdAfx.cpp LINK FileIO MiscDll Misc SDL2::SDL2)
s2_module(Script    lib LINK FileIO Input MiscDll Misc)
s2_module(DBFormat  lib LINK ADOImport FileIO Misc)

if(S2_LINUX_FULL_ENGINE)
  # Main_SRC minus GBinkPlayer.cpp (no-op stub instead, see Main/GBinkPlayerStub.cpp),
  # 2DSceneSW.cpp/SWTexture.cpp (the MMX software rasterizer -- a fallback path when D3D9
  # hardware T&L isn't available; excluded rather than porting its asm until proven needed)
  # and SplashScreen.cpp/SplashScreenDialog.cpp (a pure-GDI Win32 splash window -- no-op
  # stub instead, see Main/SplashScreenStub.cpp).
  # NOTE: Main/Gfx.cpp, GfxRender.cpp, GfxBuffers.cpp still call raw D3D9 (IDirect3DDevice9)
  # directly -- this target will NOT compile until the dxvk-native render port lands
  # (docs/linux-port.md, render section). S2_LINUX_FULL_ENGINE exists so a plain
  # `cmake --build` succeeds on the modules that ARE ready without this expected failure.
  set(Main_SRC_LINUX ${Main_SRC})
  list(REMOVE_ITEM Main_SRC_LINUX "GBinkPlayer.cpp" "2DSceneSW.cpp" "SWTexture.cpp"
                                  "SplashScreen.cpp" "SplashScreenDialog.cpp")
  list(APPEND Main_SRC_LINUX "GBinkPlayerStub.cpp" "SplashScreenStub.cpp")
  s2_module(Main lib SRC ${Main_SRC_LINUX}
                       LINK ADOImport DBFormat FileIO FModSound Input MiscDll Misc Script SDL2::SDL2)
  # MainLinux.cpp + WinFrameSDL2.cpp instead of Main.cpp + WinFrame.cpp + the .rc resource
  # (see Game/MainLinux.cpp, Game/WinFrameSDL2.cpp).
  s2_module(Game console WHOLE Main SRC MainLinux.cpp WinFrameSDL2.cpp StdAfx.cpp
                       LINK ADOImport DBFormat FileIO FModSound Input MiscDll Misc Script SDL2::SDL2)
endif()

# Tools not yet ported (need a Linux content-authoring story of their own -- see
# docs/linux-port.md -- but aren't needed to run already-built content):
#   DataImport, PkgBuilder, ShaderCompiler, FontGen, TexConv, TexMipStrip, LSConverter.
# Kept on disk, not built here (same as on Windows): MapEdit, Scintilla, OpenDynamix.
endif()

# ----------------------------------------------------------------------------
#  Visual Studio debugging (Windows only -- meaningless on Linux, and the Game
#  target may not even exist there unless S2_LINUX_FULL_ENGINE is on)
# ----------------------------------------------------------------------------
if(WIN32)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Game)

set(S2_GAME_DIR "" CACHE PATH "Silent Storm install dir")
if(NOT S2_GAME_DIR)
  foreach(_d "C:/Program Files (x86)/Steam/steamapps/common/Silent Storm"
             "C:/Program Files/Steam/steamapps/common/Silent Storm")
    if(EXISTS "${_d}")
      set(S2_GAME_DIR "${_d}" CACHE PATH "" FORCE)
      break()
    endif()
  endforeach()
endif()
if(S2_GAME_DIR)
  set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${S2_GAME_DIR}"
    VS_DEBUGGER_ENVIRONMENT "PATH=${S2_GAME_DIR};%PATH%")
  message(STATUS "Game working dir: ${S2_GAME_DIR}")
else()
  message(STATUS "Game install not found -- pass -DS2_GAME_DIR=\"<path>\" to set the working dir.")
endif()
endif() # WIN32
