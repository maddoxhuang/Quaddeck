include(FindPackageHandleStandardArgs)

set(_FFMPEG_COMPONENTS ${FFmpeg_FIND_COMPONENTS})
if(NOT _FFMPEG_COMPONENTS)
  set(_FFMPEG_COMPONENTS avformat avcodec avutil swscale swresample)
endif()

set(FFmpeg_INCLUDE_DIRS "")
set(FFmpeg_LIBRARIES "")

find_path(FFmpeg_INCLUDE_DIR
  NAMES libavcodec/avcodec.h
  PATH_SUFFIXES include
)

foreach(_component IN LISTS _FFMPEG_COMPONENTS)
  string(TOUPPER "${_component}" _upper)
  find_library(FFmpeg_${_upper}_LIBRARY
    NAMES ${_component}
    PATH_SUFFIXES lib
  )
  if(FFmpeg_${_upper}_LIBRARY)
    list(APPEND FFmpeg_LIBRARIES "${FFmpeg_${_upper}_LIBRARY}")
    set(FFmpeg_${_component}_FOUND TRUE)
  else()
    set(FFmpeg_${_component}_FOUND FALSE)
  endif()
endforeach()

if(FFmpeg_INCLUDE_DIR)
  set(FFmpeg_INCLUDE_DIRS "${FFmpeg_INCLUDE_DIR}")
endif()

find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFmpeg_INCLUDE_DIR FFmpeg_LIBRARIES
  HANDLE_COMPONENTS
)

mark_as_advanced(FFmpeg_INCLUDE_DIR)
