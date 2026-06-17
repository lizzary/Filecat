cmake_minimum_required(VERSION 3.15)
project(filecat LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(WIN32)
    set(FILECAT_PLATFORM_SOURCES src/platform/windows/filecat_win.c)
elseif(APPLE)
    message(FATAL_ERROR "Filecat: macOS backend not implemented yet")
elseif(UNIX)
    message(FATAL_ERROR "Filecat: Linux backend not implemented yet")
else()
    message(FATAL_ERROR "Filecat: unsupported platform")
endif()

add_library(filecat STATIC ${FILECAT_PLATFORM_SOURCES})

target_include_directories(filecat
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if(MSVC)
    target_compile_options(filecat PRIVATE /W4)
else()
    target_compile_options(filecat PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(FILECAT_BUILD_EXAMPLES "Build example programs" ON)
if(FILECAT_BUILD_EXAMPLES)
    add_executable(filecat-watch examples/watch.c)
    target_link_libraries(filecat-watch PRIVATE filecat)
endif()
