# load common dependencies
# this file will also load platform specific dependencies

include(dependencies/Boost_Sunshine)

# common dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ffmpeg pre-compiled binaries
if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    if(WIN32)
        set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
    endif()
    set(FFMPEG_PREPARED_BINARIES
            "${CMAKE_SOURCE_DIR}/third-party/build-deps/dist/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

    # check if the directory exists
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}")
        message(FATAL_ERROR
                "FFmpeg pre-compiled binaries not found at ${FFMPEG_PREPARED_BINARIES}. \
                Please consider contributing to the LizardByte/build-deps repository. \
                Optionally, you can use the FFMPEG_PREPARED_BINARIES option to specify the path to the \
                system-installed FFmpeg libraries")
    endif()

    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY
                "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()
    set(FFMPEG_LIBRARIES
            "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
            ${HDR10_PLUS_LIBRARY}
            ${FFMPEG_PLATFORM_LIBRARIES})
else()
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        ${FFMPEG_PLATFORM_LIBRARIES})
endif()

set(FFMPEG_INCLUDE_DIRS
        "${FFMPEG_PREPARED_BINARIES}/include")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    if(APPLE)
        include("${CMAKE_MODULE_PATH}/dependencies/macos.cmake")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
    endif()
endif()
