cmake_minimum_required(VERSION 3.15)

# macOS deployment target: the macOS backend uses
# kFSEventStreamCreateFlagUseExtendedData (FSEvents v2), introduced in 10.13.
# Pin here BEFORE project() so toolchain detection picks it up. Callers can
# override on the command line with -DCMAKE_OSX_DEPLOYMENT_TARGET=...
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13)
endif()

project(filecat LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(WIN32)
    set(FILECAT_PLATFORM_SOURCES src/platform/windows/filecat_win.c)
elseif(APPLE)
    set(FILECAT_PLATFORM_SOURCES src/platform/macos/filecat_mac.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FILECAT_PLATFORM_SOURCES src/platform/linux/filecat_linux.c)
elseif(UNIX)
    message(FATAL_ERROR "Filecat: only Linux is supported in the UNIX family (backend uses inotify)")
else()
    message(FATAL_ERROR "Filecat: unsupported platform")
endif()

add_library(filecat STATIC ${FILECAT_PLATFORM_SOURCES})

target_include_directories(filecat
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if(APPLE)
    target_link_libraries(filecat PRIVATE "-framework CoreServices")
endif()

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

option(FILECAT_BUILD_TESTS "Build test programs" ON)
if(FILECAT_BUILD_TESTS)
    enable_testing()

    # Threads::Threads is the cross-platform CMake interface for the
    # pthread/Win32 thread libs the helpers need. On Windows it's an
    # empty target (we link CRT directly via _beginthreadex) but
    # find_package keeps the call site uniform.
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    set(_FILECAT_TESTS
        test_correctness
        test_stress
        test_high_load)

    foreach(_t IN LISTS _FILECAT_TESTS)
        add_executable(${_t} tests/${_t}.c)
        target_link_libraries(${_t} PRIVATE filecat Threads::Threads)
        target_include_directories(${_t} PRIVATE tests)
        if(MSVC)
            target_compile_options(${_t} PRIVATE /W3)
            target_compile_definitions(${_t} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE)
        else()
            target_compile_options(${_t} PRIVATE -Wall -Wextra)
            # snprintf("%s" + literal, ...) into char[N] from a char[N]
            # source triggers GCC's worst-case-N+11 analysis even when
            # the runtime inputs are tmpdir paths bounded by the OS.
            # We don't ship the tests, so silencing keeps the noise out.
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                target_compile_options(${_t} PRIVATE -Wno-format-truncation)
            endif()
        endif()
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()

    # Per-test timeouts: correctness tests are quick; the high-load
    # suite runs a 5s soak so it needs more headroom.
    set_tests_properties(test_correctness PROPERTIES TIMEOUT 30)
    set_tests_properties(test_stress      PROPERTIES TIMEOUT 60)
    set_tests_properties(test_high_load   PROPERTIES TIMEOUT 90)
endif()

# Benchmarks are opt-in: they're for manual perf runs and CI dashboards,
# not the default build, and they share Threads with the tests.
option(FILECAT_BUILD_BENCH "Build benchmark programs" OFF)
if(FILECAT_BUILD_BENCH)
    if(NOT TARGET Threads::Threads)
        set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
        set(THREADS_PREFER_PTHREAD_FLAG TRUE)
        find_package(Threads REQUIRED)
    endif()

    set(_FILECAT_BENCHES
        bench_throughput
        bench_latency
        bench_rss
        bench_open)

    foreach(_b IN LISTS _FILECAT_BENCHES)
        add_executable(${_b} bench/${_b}.c)
        target_link_libraries(${_b} PRIVATE filecat Threads::Threads)
        target_include_directories(${_b} PRIVATE bench tests)
        if(MSVC)
            target_compile_options(${_b} PRIVATE /W3)
            target_compile_definitions(${_b} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE)
            # GetProcessMemoryInfo lives in psapi.lib (since Windows 7,
            # it's just an alias for kernel32 -- linking is still required).
            target_link_libraries(${_b} PRIVATE psapi)
        else()
            target_compile_options(${_b} PRIVATE -Wall -Wextra)
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                target_compile_options(${_b} PRIVATE -Wno-format-truncation)
            endif()
        endif()
    endforeach()
endif()
