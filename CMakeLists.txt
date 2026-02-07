cmake_minimum_required(VERSION 3.16)

project(vnef_video C)

option(VNEF_VIDEO_BUILD_SHARED "Build shared library" ON)

if (VNEF_VIDEO_BUILD_SHARED)
    add_library(vnef_video SHARED
        src/vnef_video.c
    )
else()
    add_library(vnef_video STATIC
        src/vnef_video.c
    )
endif()

target_include_directories(vnef_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

target_link_libraries(vnef_video PRIVATE PkgConfig::FFMPEG)

if (WIN32 AND VNEF_VIDEO_BUILD_SHARED)
    target_compile_definitions(vnef_video PRIVATE VNEF_VIDEO_BUILD_DLL)
endif()

if (UNIX)
    target_compile_options(vnef_video PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(vnef_dump examples/dump_info.c)
target_link_libraries(vnef_dump PRIVATE vnef_video)
