cmake_minimum_required(VERSION 3.25)

project(
    low_latency_event_engine
    VERSION 0.1.0
    DESCRIPTION "Low-latency shared-memory and UDP event distribution engine"
    LANGUAGES CXX
)

include(CTest)

option(LLE_BUILD_APPS "Build sender and receiver command-line applications" ON)
option(LLE_BUILD_TESTS "Build the test suite" ${BUILD_TESTING})
option(LLE_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(LLE_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LLE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(LLE_ENABLE_ASAN AND LLE_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan must use separate build trees")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lle_project_options INTERFACE)
target_compile_features(lle_project_options INTERFACE cxx_std_23)

if(LLE_ENABLE_ASAN)
    target_compile_options(lle_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lle_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

if(LLE_ENABLE_TSAN)
    target_compile_options(lle_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(lle_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
endif()

add_library(lle_project_warnings INTERFACE)
if(LLE_ENABLE_WARNINGS)
    target_compile_options(
        lle_project_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wold-style-cast
            -Wnon-virtual-dtor
    )
endif()

add_library(
    lle
    STATIC
        src/platform/affinity.cpp
        src/platform/clock.cpp
        src/platform/version.cpp
        src/protocol/decoder.cpp
        src/protocol/encoder.cpp
        src/receiver/receiver.cpp
        src/reliability/gap_detector.cpp
        src/reliability/reorder_buffer.cpp
        src/reliability/retransmit_window.cpp
        src/sender/sender.cpp
        src/shm/segment.cpp
        src/shm/spsc_ring.cpp
        src/transport/udp_receiver.cpp
        src/transport/udp_sender.cpp
)

add_library(lle::lle ALIAS lle)

target_include_directories(
    lle
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(lle PRIVATE LLE_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(lle PUBLIC lle_project_options PRIVATE lle_project_warnings)

if(LLE_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(LLE_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

