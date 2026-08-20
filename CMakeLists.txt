cmake_minimum_required(VERSION 3.25)

project(PixelStatusNX VERSION 0.1.0 LANGUAGES CXX)

option(PIXELSTATUS_BUILD_SIMULATOR "Build the native desktop simulator" ON)
option(PIXELSTATUS_BUILD_TESTS "Build host-side tests" ON)
option(PIXELSTATUS_BUILD_HOST_HTTP "Build the desktop HTTP monitor adapter" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(pixelstatus_core
    src/core/appearance.cpp
    src/core/color.cpp
    src/core/config.cpp
    src/core/frame.cpp
    src/core/evaluator.cpp
    src/core/http_url.cpp
    src/core/mi_protocol.cpp
    src/core/monitor.cpp
    src/core/monitor_engine.cpp
    src/core/renderer.cpp
    src/core/state.cpp
    src/core/status_api.cpp
    src/core/validation.cpp
)

target_include_directories(pixelstatus_core PUBLIC include)
target_link_libraries(pixelstatus_core PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(pixelstatus_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(pixelstatus_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PIXELSTATUS_BUILD_HOST_HTTP OR PIXELSTATUS_BUILD_SIMULATOR)
    set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
    set(HTTPLIB_TEST OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")
    FetchContent_Declare(
        cpp_httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.51.0.tar.gz
        URL_HASH SHA256=d740ced75352f44e9d66d08806dc231b5621bb3592b5a5d5b2bd890a9a9d86cc
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(cpp_httplib)
endif()

if(PIXELSTATUS_BUILD_HOST_HTTP)
    add_library(pixelstatus_host
        src/host/dns_monitor_runner.cpp
        src/host/http_display_driver.cpp
        src/host/http_monitor_runner.cpp
        src/host/monitor_executor.cpp
        src/host/monitor_runner_factory.cpp
        src/host/tcp_connect_monitor_runner.cpp
    )
    target_include_directories(pixelstatus_host PUBLIC include)
    target_link_libraries(pixelstatus_host PRIVATE
        pixelstatus_core
        httplib::httplib
        nlohmann_json::nlohmann_json
    )
    if(WIN32)
        target_link_libraries(pixelstatus_host PRIVATE ws2_32)
    endif()

    if(MSVC)
        target_compile_options(pixelstatus_host PRIVATE /W4 /permissive-)
    else()
        target_compile_options(pixelstatus_host PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(PIXELSTATUS_BUILD_SIMULATOR)
    if(NOT WIN32)
        message(FATAL_ERROR "The current simulator uses the Win32 API")
    endif()
    if(NOT PIXELSTATUS_BUILD_HOST_HTTP)
        message(FATAL_ERROR "The simulator requires PIXELSTATUS_BUILD_HOST_HTTP")
    endif()

    add_executable(pixelstatus_simulator
        src/simulator/http_status_server.cpp
        src/simulator/main.cpp
        src/simulator/win32_output_driver.cpp
    )
    target_include_directories(pixelstatus_simulator PRIVATE src/simulator)
    target_link_libraries(pixelstatus_simulator PRIVATE
        pixelstatus_core
        pixelstatus_host
        httplib::httplib
        user32
        gdi32
    )
    target_compile_definitions(pixelstatus_simulator PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(pixelstatus_simulator PRIVATE /W4 /permissive-)
    endif()

    add_custom_command(
        TARGET pixelstatus_simulator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/pixelstatus.sample.json
            $<TARGET_FILE_DIR:pixelstatus_simulator>/pixelstatus.sample.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/http-monitor.example.json
            $<TARGET_FILE_DIR:pixelstatus_simulator>/http-monitor.example.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/concurrent-monitors.example.json
            $<TARGET_FILE_DIR:pixelstatus_simulator>/concurrent-monitors.example.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/tcp-connect.example.json
            $<TARGET_FILE_DIR:pixelstatus_simulator>/tcp-connect.example.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/dns-monitor.example.json
            $<TARGET_FILE_DIR:pixelstatus_simulator>/dns-monitor.example.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
            $<TARGET_FILE_DIR:pixelstatus_simulator>/THIRD_PARTY_NOTICES.md
        VERBATIM
    )
endif()

if(PIXELSTATUS_BUILD_TESTS)
    enable_testing()

    add_executable(pixelstatus_tests tests/test_main.cpp)
    target_link_libraries(pixelstatus_tests PRIVATE pixelstatus_core)
    target_compile_definitions(pixelstatus_tests PRIVATE
        PIXELSTATUS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
    )
    if(PIXELSTATUS_BUILD_HOST_HTTP)
        target_link_libraries(pixelstatus_tests PRIVATE
            pixelstatus_host
            httplib::httplib
            nlohmann_json::nlohmann_json
        )
        target_compile_definitions(pixelstatus_tests PRIVATE
            PIXELSTATUS_TEST_HOST_HTTP=1
        )
    endif()

    if(MSVC)
        target_compile_options(pixelstatus_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(pixelstatus_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME pixelstatus_tests COMMAND pixelstatus_tests)
endif()
