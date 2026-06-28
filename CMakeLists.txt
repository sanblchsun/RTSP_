cmake_minimum_required(VERSION 3.10)
project(DesktopStreamer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Поиск FFmpeg
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(NOT FFMPEG_EXECUTABLE)
    message(FATAL_ERROR "FFmpeg не найден в PATH")
endif()

# Получаем путь к FFmpeg
get_filename_component(FFMPEG_BIN_DIR ${FFMPEG_EXECUTABLE} DIRECTORY)
get_filename_component(FFMPEG_ROOT ${FFMPEG_BIN_DIR} DIRECTORY)

message(STATUS "Найден FFmpeg: ${FFMPEG_EXECUTABLE}")
message(STATUS "FFmpeg root: ${FFMPEG_ROOT}")

# Устанавливаем пути
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

# Проверяем существование директорий
if(NOT EXISTS ${FFMPEG_INCLUDE_DIR})
    message(FATAL_ERROR "FFmpeg include directory не найден: ${FFMPEG_INCLUDE_DIR}")
endif()

if(NOT EXISTS ${FFMPEG_LIB_DIR})
    message(FATAL_ERROR "FFmpeg lib directory не найден: ${FFMPEG_LIB_DIR}")
endif()

# Исходные файлы
set(SOURCES
    desktop_streamer.cpp
    main.cpp
)

set(HEADERS
    desktop_streamer.h
    main.h
)

add_executable(desktop_streamer ${SOURCES} ${HEADERS})

target_include_directories(desktop_streamer PRIVATE ${FFMPEG_INCLUDE_DIR})

# Линковка библиотек FFmpeg
target_link_libraries(desktop_streamer
    ${FFMPEG_LIB_DIR}/avcodec.lib
    ${FFMPEG_LIB_DIR}/avformat.lib
    ${FFMPEG_LIB_DIR}/avutil.lib
    ${FFMPEG_LIB_DIR}/avdevice.lib
    ${FFMPEG_LIB_DIR}/swscale.lib
    ${FFMPEG_LIB_DIR}/swresample.lib
)

# Системные библиотеки Windows
if(WIN32)
    target_link_libraries(desktop_streamer
        ws2_32
        secur32
        bcrypt
        mfplat
        mfuuid
        strmiids
        ole32
        oleaut32
        uuid
    )
endif()

# Копирование DLL файлов
if(EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    foreach(dll ${FFMPEG_DLLS})
        add_custom_command(TARGET desktop_streamer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${dll} $<TARGET_FILE_DIR:desktop_streamer>)
    endforeach()
endif()

# Настройки компилятора
if(MSVC)
    target_compile_definitions(desktop_streamer PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    # Отключаем предупреждения для внешних заголовков
    target_compile_options(desktop_streamer PRIVATE /W3)
else()
    target_compile_options(desktop_streamer PRIVATE -Wall -Wextra)
endif()