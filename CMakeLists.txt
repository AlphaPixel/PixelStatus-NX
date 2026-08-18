cmake_minimum_required(VERSION 3.25)

project(PixelStatusNX VERSION 0.1.0 LANGUAGES CXX)

option(PIXELSTATUS_BUILD_SIMULATOR "Build the native desktop simulator" ON)
option(PIXELSTATUS_BUILD_TESTS "Build host-side tests" ON)

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
    src/core/mi_protocol.cpp
    src/core/renderer.cpp
    src/core/state.cpp
    src/core/status_api.cpp
)

target_include_directories(pixelstatus_core PUBLIC include)
target_link_libraries(pixelstatus_core PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(pixelstatus_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(pixelstatus_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PIXELSTATUS_BUILD_SIMULATOR)
    if(NOT WIN32)
        message(FATAL_ERROR "The current simulator uses the Win32 API")
    endif()

    set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
    set(HTTPLIB_TEST OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")
    FetchContent_Declare(
        cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.51.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cpp_httplib)

    add_executable(pixelstatus_simulator
        src/simulator/http_status_server.cpp
        src/simulator/main.cpp
        src/simulator/win32_output_driver.cpp
    )
    target_include_directories(pixelstatus_simulator PRIVATE src/simulator)
    target_link_libraries(pixelstatus_simulator PRIVATE
        pixelstatus_core
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

    if(MSVC)
        target_compile_options(pixelstatus_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(pixelstatus_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME pixelstatus_tests COMMAND pixelstatus_tests)
endif()
