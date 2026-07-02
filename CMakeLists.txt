cmake_minimum_required(VERSION 3.15)
project(desktop_streamer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(X264_EXECUTABLE x264
    PATHS "C:/x264_sdk/bin/x64"
)
if(NOT X264_EXECUTABLE)
    message(FATAL_ERROR "x264 not found. Install x264 SDK or add to PATH")
endif()

get_filename_component(X264_BIN_DIR ${X264_EXECUTABLE} DIRECTORY)
set(X264_CANDIDATE ${X264_BIN_DIR})
set(X264_ROOT "")
foreach(i RANGE 5)
    if(EXISTS "${X264_CANDIDATE}/include/x264.h")
        set(X264_ROOT ${X264_CANDIDATE})
        break()
    endif()
    get_filename_component(X264_CANDIDATE "${X264_CANDIDATE}" DIRECTORY)
endforeach()
if(NOT X264_ROOT)
    get_filename_component(X264_ROOT ${X264_BIN_DIR} DIRECTORY)
endif()

message(STATUS "x264 root: ${X264_ROOT}")
set(X264_INCLUDE_DIR "${X264_ROOT}/include")
set(X264_LIB_DIR "${X264_ROOT}/lib")

if(NOT EXISTS ${X264_INCLUDE_DIR})
    message(FATAL_ERROR "x264 include not found: ${X264_INCLUDE_DIR}")
endif()
if(NOT EXISTS ${X264_LIB_DIR})
    message(FATAL_ERROR "x264 lib not found: ${X264_LIB_DIR}")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(SOURCES
    main.cpp
    WinRT-API/capture_wgc.cpp
    WinRT-API/encoder_x264.cpp
    rtp/h264_rtp_packetizer.cpp
    rtsp/rtsp_server.cpp
)
set(HEADERS
    WinRT-API/capture_wgc.h
    WinRT-API/encoder_x264.h
    rtp/rtp_header.h
    rtp/h264_rtp_packetizer.h
    rtsp/rtsp_server.h
)

add_executable(desktop_streamer ${SOURCES} ${HEADERS})

target_include_directories(desktop_streamer PRIVATE
    ${X264_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}
)

find_library(X264_LIBRARY libx264 PATHS ${X264_LIB_DIR} "${X264_LIB_DIR}/x64" "C:/x264_sdk/lib/x64" NO_DEFAULT_PATH)
if(NOT X264_LIBRARY)
    find_library(X264_LIBRARY x264 PATHS ${X264_LIB_DIR} "${X264_LIB_DIR}/x64" NO_DEFAULT_PATH)
endif()
if(NOT X264_LIBRARY)
    message(FATAL_ERROR "x264 library not found in ${X264_LIB_DIR}")
endif()

target_link_libraries(desktop_streamer ${X264_LIBRARY})

if(WIN32)
    target_link_libraries(desktop_streamer
        ws2_32
        d3d11
        dxgi
        ole32
        oleaut32
        uuid
        user32
        advapi32
        runtimeobject
    )
endif()

if(MSVC)
    target_compile_definitions(desktop_streamer PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_compile_options(desktop_streamer PRIVATE /W3)
else()
    target_compile_options(desktop_streamer PRIVATE -Wall)
endif()
