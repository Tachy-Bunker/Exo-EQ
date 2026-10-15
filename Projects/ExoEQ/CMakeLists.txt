cmake_minimum_required(VERSION 3.14)
project(ExoEQ VERSION 1.0.0)

# Discover IPLUG2_DIR if not already set (for independent builds)
if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "iPlug2 root directory")
endif()

# Include iPlug2 CMake configuration
include(${IPLUG2_DIR}/iPlug2.cmake)

# IGraphics backend/renderer can be set via command line:
#   cmake -DIGRAPHICS_BACKEND=SKIA -DIGRAPHICS_RENDERER=GL3 ..
# Defaults: NANOVG/METAL on macOS, NANOVG/GL2 on Windows
# See iPlug2/Scripts/cmake/IGraphics.cmake for all options

find_package(iPlug2 REQUIRED)

# Build all formats except AUv3 (AUv3 is opt-in)
# To include AUv3: FORMATS ALL_AUV3
# To select specific formats: FORMATS APP VST3 AU
iplug_add_plugin(${PROJECT_NAME}
  FORMATS APP VST3
  SOURCES
    ExoEQ.cpp
    ExoEQ.h
    DSP/FilterCore.h
    DSP/StateVariableFilter.h
    DSP/KeyTrack.h
    DSP/BandChain.h
    resources/resource.h
    # --- Added this round: these two were already referenced by
    # ExoEQ.h/.cpp's #includes and compiling fine as headers, but their
    # .c/.cpp BODIES were never in this SOURCES list, so nothing ever
    # compiled their actual function implementations in -- the direct
    # cause of the pffft_*/MTS_* "unresolved external symbol" link
    # errors. Vendor/MTS-ESP/libMTSClient.cpp was already called out as
    # needed back when MTS-ESP first shipped; Vendor/pffft/pffft.c
    # should have been called out the same way when the FFT stage
    # shipped and wasn't -- both land here now.
    Vendor/pffft/pffft.c
    Vendor/MTS-ESP/libMTSClient.cpp
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
)

# --- Added this round: MTS-ESP's libMTSClient.cpp dlopen()s/LoadLibrary()s
# a system-installed shared library at runtime (never statically linked),
# but on Mac/Linux that dynamic-loading API itself (dlopen/dlsym/dlclose)
# lives in libdl, which isn't linked by default the way Windows' equivalent
# (LoadLibrary, part of the Windows system libs CMake already links) is.
# Windows needs nothing extra here.
if(UNIX AND NOT APPLE)
  foreach(_exoeq_dl_target ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-vst2 ${PROJECT_NAME}-clap ${PROJECT_NAME}-aax ${PROJECT_NAME}-app)
    if(TARGET ${_exoeq_dl_target})
      target_link_libraries(${_exoeq_dl_target} PRIVATE dl)
    endif()
  endforeach()
elseif(APPLE)
  # macOS's libdl symbols live in libSystem, linked by default already --
  # dlopen/dlsym/dlclose are available with no extra target_link_libraries
  # entry needed there. Left as an explicit no-op branch (rather than
  # folding into the UNIX branch above) so this doesn't silently try to
  # link a plain "dl" library name that doesn't exist as such on macOS.
endif()

# --- Upstream gap fix (2026-10-14) -----------------------------------------
# iPlug2's own Scripts/cmake/IPlugPlugin.cmake only appends resources/main.rc
# (which embeds the Roboto font, icon, menu) to the APP target's sources.
# VST3/VST2/CLAP/AAX module targets never get it, so any plugin format built
# this way has no embedded font -> NanoVG draws vector shapes (knobs) fine
# but renders zero text, silently. Wire main.rc into every module target
# ourselves rather than patching the vendored iPlug2 CMake (an iPlug2 update
# would wipe that edit).
if(WIN32)
  set(_exoeq_rc_file "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
  if(EXISTS "${_exoeq_rc_file}")
    foreach(_exoeq_fmt_target ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-vst2 ${PROJECT_NAME}-clap ${PROJECT_NAME}-aax)
      if(TARGET ${_exoeq_fmt_target})
        target_sources(${_exoeq_fmt_target} PRIVATE "${_exoeq_rc_file}")
        set_source_files_properties("${_exoeq_rc_file}" PROPERTIES
          COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/img\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
        )
        message(STATUS "[ExoEQ] Added main.rc (font/icon resources) to ${_exoeq_fmt_target}, working around iPlug2 CMake gap")
      endif()
    endforeach()
  endif()
endif()
