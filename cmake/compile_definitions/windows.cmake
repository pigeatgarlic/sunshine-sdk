# windows specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="windows")

enable_language(RC)
set(CMAKE_RC_COMPILER windres)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# curl
add_definitions(-DCURL_STATICLIB)
include_directories(SYSTEM ${CURL_STATIC_INCLUDE_DIRS})
link_directories(${CURL_STATIC_LIBRARY_DIRS})

# extra tools/binaries for audio/display devices
add_subdirectory(tools)  # todo - this is temporary, only tools for Windows are needed, for now

# nvidia
include_directories(SYSTEM third-party/nvapi-open-source-sdk)
file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS
        "third-party/nvapi-open-source-sdk/*.h"
        "src/platform/windows/nvprefs/*.cpp"
        "src/platform/windows/nvprefs/*.h")

# sunshine icon
if(NOT DEFINED SUNSHINE_ICON_PATH)
    set(SUNSHINE_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sunshine.ico")
endif()

configure_file(src/platform/windows/windows.rs.in windows.rc @ONLY)

set(PLATFORM_TARGET_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/windows.rc"
        src/platform/windows/misc.h
        src/platform/windows/misc.cpp
        src/platform/windows/display.h
        src/platform/windows/display_base.cpp
        src/platform/windows/display_vram.cpp
        src/platform/windows/display_ram.cpp
        ${NVPREFS_FILES})

set(OPENSSL_LIBRARIES
        libssl.a
        libcrypto.a)

list(PREPEND PLATFORM_LIBRARIES
        libstdc++.a
        libwinpthread.a
        libssp.a
        ksuser
        wsock32
        ws2_32
        d3d11 dxgi D3DCompiler
        setupapi
        dwmapi
        userenv
        synchronization.lib
        avrt
        ${CURL_STATIC_LIBRARIES})