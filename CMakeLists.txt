cmake_minimum_required(VERSION 3.24)
project(QuadDeck VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# vcpkg.json cannot read CMake, so the two version numbers are compared here.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _quaddeck_manifest)
string(JSON _quaddeck_manifest_version GET "${_quaddeck_manifest}" version-semver)
if(NOT _quaddeck_manifest_version STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "vcpkg.json says version ${_quaddeck_manifest_version}, "
    "CMakeLists.txt ${PROJECT_VERSION}; change both together.")
endif()

# What this build is, for the first line of QuadDeck.log (version, commit,
# configuration) and for the version servers see (PROJECT_VERSION only).
# Taken at configure time, which build-windows.ps1 runs every time; one
# small generated source, so a new commit recompiles only that file.
set(QUADDECK_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _quaddeck_git_result OUTPUT_VARIABLE _quaddeck_git_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_quaddeck_git_result EQUAL 0 AND _quaddeck_git_commit)
    set(QUADDECK_COMMIT "${_quaddeck_git_commit}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE _quaddeck_git_result OUTPUT_VARIABLE _quaddeck_git_status
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _quaddeck_git_result EQUAL 0 OR NOT _quaddeck_git_status STREQUAL "")
      string(APPEND QUADDECK_COMMIT "-dirty")
    endif()
  endif()
endif()
configure_file(src/BuildInfo.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.cpp" @ONLY)
add_library(QuadDeckBuildInfo STATIC "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.cpp")
target_include_directories(QuadDeckBuildInfo PRIVATE src)
target_compile_definitions(QuadDeckBuildInfo PRIVATE "QUADDECK_BUILD_CONFIGURATION=\"$<CONFIG>\"")
# The player uses this for HTTP identification and diagnostic build metadata.
link_libraries(QuadDeckBuildInfo)

if(WIN32)
  enable_language(RC)
  find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample)
  find_package(nlohmann_json CONFIG REQUIRED)
  # libass draws ASS subtitles as their scripts are written. vcpkg builds it
  # with meson and installs no CMake package, only the header and library.
  find_path(LIBASS_INCLUDE_DIR ass/ass.h REQUIRED)
  find_library(LIBASS_LIBRARY NAMES ass libass REQUIRED)

  add_executable(QuadDeck WIN32
    src/main.cpp
    src/SingleInstance.cpp
    src/App.cpp
    src/AppPanes.cpp
    src/AppCommands.cpp
    src/AppPersistence.cpp
    src/AppPlayback.cpp
    src/AppChrome.cpp
    src/Overlay.cpp
    src/AppPanel.cpp
    src/AppEmby.cpp
    src/AppEmbyBrowser.cpp
    src/AppEmbyPlayback.cpp
    src/AppEmbyDetails.cpp
    src/AppEmbySearch.cpp
    src/AppTextFields.cpp
    src/AppSubtitles.cpp
    src/AssSubtitles.cpp
    src/AppLocal.cpp
    src/LocalThumbnails.cpp
    src/FileAssociations.cpp
    src/EmbyClient.cpp
    src/FilePersistence.cpp
    src/D3DRenderer.cpp
    src/VideoSource.cpp
    src/ReadAheadCache.cpp
    src/AudioOutput.cpp
    src/HangWatchdog.cpp
    src/QuadDeck.rc
  )

  target_include_directories(QuadDeck PRIVATE src ${FFmpeg_INCLUDE_DIRS} ${LIBASS_INCLUDE_DIR})
  target_link_libraries(QuadDeck PRIVATE
    ${FFmpeg_LIBRARIES}
    ${LIBASS_LIBRARY}
    nlohmann_json::nlohmann_json
    d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs
    xaudio2 ole32 shell32 comdlg32 shlwapi comctl32 uxtheme dwmapi winhttp crypt32 advapi32 dbghelp imm32
  )
  target_compile_definitions(QuadDeck PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  )
  if(MSVC)
    target_compile_options(QuadDeck PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # QuadDeck.pdb beside the Release executable lets the hang watchdog name
    # functions and lines. /DEBUG would switch these two optimizations off.
    target_compile_options(QuadDeck PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(QuadDeck PRIVATE "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/PDBALTPATH:QuadDeck.pdb>")
  endif()

  install(TARGETS QuadDeck RUNTIME DESTINATION .)
else()
  message(FATAL_ERROR "QuadDeck requires Windows and the MSVC toolchain.")
endif()
