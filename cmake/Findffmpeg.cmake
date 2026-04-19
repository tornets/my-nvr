#[=======================================================================[.rst:
Findffmpeg
-----------

Find the FFmpeg libraries (avformat, avcodec, avutil, swresample).

This module defines:

``ffmpeg_FOUND``
  True if all required FFmpeg components were found.

``ffmpeg_VERSION``
  FFmpeg version string (from libavutil).

``ffmpeg_INCLUDE_DIRS``
  Include directories for all components.

``ffmpeg_LIBRARIES``
  List of all component libraries.

Imported Target
^^^^^^^^^^^^^^^

``ffmpeg::ffmpeg``
  Interface target linking all required FFmpeg components.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

# Required FFmpeg components
set(_ffmpeg_components avformat avcodec avutil swresample)

find_package(PkgConfig)

if(PkgConfig_FOUND)
    set(_ffmpeg_pkgconfig_failed FALSE)

    foreach(_comp ${_ffmpeg_components})
        pkg_check_modules("LibAV${_comp}" QUIET IMPORTED_TARGET "lib${_comp}")
        if(NOT LibAV${_comp}_FOUND)
            set(_ffmpeg_pkgconfig_failed TRUE)
            if(NOT ffmpeg_FIND_QUIETLY)
                message(STATUS "FFmpeg component not found via pkg-config: lib${_comp}")
            endif()
        endif()
    endforeach()

    if(NOT _ffmpeg_pkgconfig_failed)
        # Create the aggregate ffmpeg::ffmpeg target
        if(NOT TARGET ffmpeg::ffmpeg)
            add_library(ffmpeg::ffmpeg INTERFACE IMPORTED)
        endif()

        foreach(_comp ${_ffmpeg_components})
            target_link_libraries(ffmpeg::ffmpeg INTERFACE PkgConfig::LibAV${_comp})
        endforeach()

        # Collect include dirs and libraries for compatibility variables
        set(_ffmpeg_all_include_dirs "")
        set(_ffmpeg_all_libraries "")
        foreach(_comp ${_ffmpeg_components})
            if(DEFINED LibAV${_comp}_INCLUDE_DIRS)
                list(APPEND _ffmpeg_all_include_dirs ${LibAV${_comp}_INCLUDE_DIRS})
            endif()
            if(DEFINED LibAV${_comp}_LIBRARIES)
                list(APPEND _ffmpeg_all_libraries ${LibAV${_comp}_LIBRARIES})
            endif()
        endforeach()

        list(REMOVE_DUPLICATES _ffmpeg_all_include_dirs)
        list(REMOVE_DUPLICATES _ffmpeg_all_libraries)

        set(ffmpeg_INCLUDE_DIRS "${_ffmpeg_all_include_dirs}")
        set(ffmpeg_LIBRARIES "${_ffmpeg_all_libraries}")

        # Extract version from libavutil
        if(DEFINED LibAVavutil_VERSION)
            set(ffmpeg_VERSION "${LibAVavutil_VERSION}")
        endif()

        find_package_handle_standard_args(ffmpeg
            REQUIRED_VARS ffmpeg_LIBRARIES
            VERSION_VAR ffmpeg_VERSION
        )
    else()
        find_package_handle_standard_args(ffmpeg
            REQUIRED_VARS _ffmpeg_pkgconfig_failed
            FAIL_MESSAGE "Could not find all FFmpeg components via pkg-config"
        )
    endif()
else()
    find_package_handle_standard_args(ffmpeg
        REQUIRED_VARS PkgConfig_FOUND
        FAIL_MESSAGE "PkgConfig is required to find system FFmpeg"
    )
endif()

# Clean up internal variables
unset(_ffmpeg_components)
unset(_ffmpeg_pkgconfig_failed)
unset(_ffmpeg_all_include_dirs)
unset(_ffmpeg_all_libraries)
unset(_comp)
