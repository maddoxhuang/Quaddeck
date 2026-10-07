cmake_minimum_required(VERSION 3.24)
project(QuadDeck VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QUADDECK_BUILD_TESTS "Build core and native regression tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
    xaudio2 ole32 shell32 comdlg32 shlwapi comctl32 uxtheme dwmapi winhttp crypt32 advapi32 dbghelp
  )
  target_compile_definitions(QuadDeck PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  )
  if(MSVC)
    target_compile_options(QuadDeck PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # QuadDeck.pdb beside the Release executable lets the hang watchdog name
    # functions and lines. /DEBUG would switch these two optimizations off.
    # /PDBALTPATH records only the file name in the executable, not the
    # build machine's directory; the watchdog searches the executable's own
    # folder, so it still finds the file.
    target_compile_options(QuadDeck PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(QuadDeck PRIVATE
      "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/PDBALTPATH:%_PDB%>")
  endif()

  install(TARGETS QuadDeck RUNTIME DESTINATION .)
else()
  message(STATUS "QuadDeck GUI is Windows-only; building core tests only.")
endif()

if(QUADDECK_BUILD_TESTS)
  enable_testing()
  add_executable(QuadDeckCoreTests tests/core_tests.cpp)
  target_include_directories(QuadDeckCoreTests PRIVATE src)
  if(MSVC)
    target_compile_options(QuadDeckCoreTests PRIVATE /W4 /permissive- /utf-8 /UNDEBUG)
  else()
    target_compile_options(QuadDeckCoreTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  endif()
  add_test(NAME QuadDeckCoreTests COMMAND QuadDeckCoreTests)

  if(WIN32)
    add_executable(QuadDeckSoftwareVideoFrameTests tests/software_video_frame_tests.cpp)
    target_include_directories(QuadDeckSoftwareVideoFrameTests PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_compile_definitions(QuadDeckSoftwareVideoFrameTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckSoftwareVideoFrameTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckSoftwareVideoFrameTests COMMAND QuadDeckSoftwareVideoFrameTests)

    add_executable(QuadDeckCacheTests tests/cache_tests.cpp src/ReadAheadCache.cpp)
    target_include_directories(QuadDeckCacheTests PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckCacheTests PRIVATE ${FFmpeg_LIBRARIES})
    target_compile_definitions(QuadDeckCacheTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckCacheTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckCacheTests COMMAND QuadDeckCacheTests)
    set_tests_properties(QuadDeckCacheTests PROPERTIES TIMEOUT 45)

    add_executable(QuadDeckHangWatchdogTests tests/hang_watchdog_tests.cpp src/HangWatchdog.cpp)
    target_include_directories(QuadDeckHangWatchdogTests PRIVATE src)
    target_link_libraries(QuadDeckHangWatchdogTests PRIVATE dbghelp)
    target_compile_definitions(QuadDeckHangWatchdogTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      # The live test reads its own function names and lines from its PDB.
      target_compile_options(QuadDeckHangWatchdogTests PRIVATE /W4 /permissive- /utf-8 /EHsc /Zi)
      target_link_options(QuadDeckHangWatchdogTests PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
    add_test(NAME QuadDeckHangWatchdogTests COMMAND QuadDeckHangWatchdogTests)
    set_tests_properties(QuadDeckHangWatchdogTests PROPERTIES TIMEOUT 45)

    add_executable(QuadDeckAudioSeekTests
      tests/audio_seek_tests.cpp
      src/VideoSource.cpp
      src/ReadAheadCache.cpp
    )
    target_include_directories(QuadDeckAudioSeekTests PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckAudioSeekTests PRIVATE ${FFmpeg_LIBRARIES} d3d11 dxgi)
    target_compile_definitions(QuadDeckAudioSeekTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckAudioSeekTests PRIVATE /W4 /permissive- /utf-8 /EHsc /UNDEBUG)
    endif()
    add_test(NAME QuadDeckAudioSeekTests COMMAND QuadDeckAudioSeekTests)

    add_executable(QuadDeckSubtitleStreamTests
      tests/subtitle_stream_tests.cpp
      src/VideoSource.cpp
      src/ReadAheadCache.cpp
    )
    target_include_directories(QuadDeckSubtitleStreamTests PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckSubtitleStreamTests PRIVATE ${FFmpeg_LIBRARIES} d3d11 dxgi)
    target_compile_definitions(QuadDeckSubtitleStreamTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckSubtitleStreamTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckSubtitleStreamTests COMMAND QuadDeckSubtitleStreamTests)
    set_tests_properties(QuadDeckSubtitleStreamTests PROPERTIES TIMEOUT 60)

    # libass against scripts written for it: where lines land, what the
    # viewer's size and raising do, a font a video brings, the colours.
    add_executable(QuadDeckAssRenderTests
      tests/ass_render_tests.cpp
      src/AssSubtitles.cpp
    )
    target_include_directories(QuadDeckAssRenderTests PRIVATE src ${LIBASS_INCLUDE_DIR})
    target_link_libraries(QuadDeckAssRenderTests PRIVATE ${LIBASS_LIBRARY} ole32 shell32)
    target_compile_definitions(QuadDeckAssRenderTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckAssRenderTests PRIVATE /W4 /permissive- /utf-8 /EHsc /UNDEBUG)
    endif()
    add_test(NAME QuadDeckAssRenderTests COMMAND QuadDeckAssRenderTests)
    set_tests_properties(QuadDeckAssRenderTests PROPERTIES TIMEOUT 60)

    add_executable(QuadDeckColorConversionTests
      tests/color_conversion_tests.cpp
      src/D3DRenderer.cpp
      src/VideoSource.cpp
      src/ReadAheadCache.cpp
    )
    target_include_directories(QuadDeckColorConversionTests PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckColorConversionTests PRIVATE
      ${FFmpeg_LIBRARIES} d3d11 dxgi d3dcompiler ole32 shell32
    )
    target_compile_definitions(QuadDeckColorConversionTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckColorConversionTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckColorConversionTests COMMAND QuadDeckColorConversionTests)

    add_executable(QuadDeckShaderCompatTests tests/shader_compat_tests.cpp)
    target_include_directories(QuadDeckShaderCompatTests PRIVATE src)
    target_link_libraries(QuadDeckShaderCompatTests PRIVATE d3dcompiler)
    target_compile_definitions(QuadDeckShaderCompatTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckShaderCompatTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckShaderCompatTests COMMAND QuadDeckShaderCompatTests)

    add_executable(QuadDeckAppRegressionTests
      tests/app_regression_tests.cpp
      tests/single_instance_tests.cpp
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
    )
    target_include_directories(QuadDeckAppRegressionTests PRIVATE src ${FFmpeg_INCLUDE_DIRS} ${LIBASS_INCLUDE_DIR})
    target_link_libraries(QuadDeckAppRegressionTests PRIVATE
      ${FFmpeg_LIBRARIES}
      ${LIBASS_LIBRARY}
      nlohmann_json::nlohmann_json
      d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs xaudio2 ole32 shell32 comdlg32
      shlwapi comctl32 uxtheme dwmapi winhttp crypt32 advapi32 dbghelp
    )
    target_compile_definitions(QuadDeckAppRegressionTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckAppRegressionTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckAppRegressionTests COMMAND QuadDeckAppRegressionTests)
    add_test(NAME QuadDeckSingleInstanceTests COMMAND QuadDeckAppRegressionTests --single-instance-process)
    set_tests_properties(QuadDeckSingleInstanceTests PROPERTIES TIMEOUT 90)

    add_executable(QuadDeckEmbyTests tests/emby_tests.cpp)
    target_include_directories(QuadDeckEmbyTests PRIVATE src)
    target_link_libraries(QuadDeckEmbyTests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(QuadDeckEmbyTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckEmbyTests PRIVATE /W4 /permissive- /utf-8 /EHsc /UNDEBUG)
    endif()
    add_test(NAME QuadDeckEmbyTests COMMAND QuadDeckEmbyTests)


    # Needs a reachable Emby server and credentials, so it is not a CTest.
    add_executable(QuadDeckEmbyProbe
      tests/emby_probe.cpp
      src/EmbyClient.cpp
      src/FilePersistence.cpp
    )
    target_include_directories(QuadDeckEmbyProbe PRIVATE src)
    target_link_libraries(QuadDeckEmbyProbe PRIVATE nlohmann_json::nlohmann_json ole32 shell32 winhttp crypt32)
    target_compile_definitions(QuadDeckEmbyProbe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckEmbyProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()

    add_executable(QuadDeckPersistenceTests
      tests/persistence_tests.cpp
      src/FilePersistence.cpp
    )
    target_include_directories(QuadDeckPersistenceTests PRIVATE src)
    target_compile_definitions(QuadDeckPersistenceTests PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckPersistenceTests PRIVATE /W4 /permissive- /utf-8 /EHsc /UNDEBUG)
    endif()
    add_test(NAME QuadDeckPersistenceTests COMMAND QuadDeckPersistenceTests)

    add_executable(QuadDeckSeekProbe
      tests/seek_probe.cpp
      src/VideoSource.cpp
      src/ReadAheadCache.cpp
    )
    target_include_directories(QuadDeckSeekProbe PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckSeekProbe PRIVATE
      ${FFmpeg_LIBRARIES}
      d3d11 dxgi
    )
    target_compile_definitions(QuadDeckSeekProbe PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckSeekProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()

    # Uses explicitly generated synthetic H.264 files, never the user's media.
    # Optional native decoder-path evidence, not an unconditional CTest.
    add_executable(QuadDeckHdrDecodeProbe
      tests/hdr_decode_probe.cpp src/VideoSource.cpp src/ReadAheadCache.cpp)
    target_include_directories(QuadDeckHdrDecodeProbe PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckHdrDecodeProbe PRIVATE ${FFmpeg_LIBRARIES} d3d11 dxgi)
    target_compile_definitions(QuadDeckHdrDecodeProbe PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(QuadDeckHdrDecodeProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()

    add_executable(QuadDeckShaderProbe
      tests/shader_probe.cpp
      src/D3DRenderer.cpp
      src/VideoSource.cpp
      src/ReadAheadCache.cpp
    )
    target_include_directories(QuadDeckShaderProbe PRIVATE src ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(QuadDeckShaderProbe PRIVATE
      ${FFmpeg_LIBRARIES}
      d3d11 dxgi d3dcompiler
    )
    target_compile_definitions(QuadDeckShaderProbe PRIVATE
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
      target_compile_options(QuadDeckShaderProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME QuadDeckShaderProbe COMMAND QuadDeckShaderProbe)
  endif()
endif()
