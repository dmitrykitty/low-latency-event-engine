if(LLE_ENABLE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(LLE_SETARCH_EXECUTABLE setarch REQUIRED)
    # disable address randomization only in test processes so tsan can reserve its shadow memory.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.29")
        set(CMAKE_TEST_LAUNCHER "${LLE_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    else()
        # older cmake uses this property for native google test discovery and execution too.
        set(CMAKE_CROSSCOMPILING_EMULATOR "${LLE_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R")
    endif()
endif()

find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(lle-smoke-test unit/smoke_test.cpp)
target_link_libraries(
    lle-smoke-test
    PRIVATE
        lle::lle
        lle_project_warnings
        GTest::gtest_main
)

gtest_discover_tests(lle-smoke-test TEST_PREFIX "lle.smoke.")

add_executable(lle-shm-segment-test unit/shm_segment_test.cpp)
target_link_libraries(
    lle-shm-segment-test
    PRIVATE
        lle::lle
        lle_project_warnings
        GTest::gtest_main
)

gtest_discover_tests(lle-shm-segment-test TEST_PREFIX "lle.shm.segment.")

add_executable(lle-ring-init-test unit/ring_init_test.cpp)
target_link_libraries(lle-ring-init-test PRIVATE lle::lle lle_project_warnings GTest::gtest_main)
gtest_discover_tests(lle-ring-init-test TEST_PREFIX "lle.shm.ring.")

add_executable(lle-ring-transfer-test unit/ring_transfer_test.cpp)
target_link_libraries(lle-ring-transfer-test PRIVATE lle::lle lle_project_warnings GTest::gtest_main)
gtest_discover_tests(lle-ring-transfer-test TEST_PREFIX "lle.shm.transfer.")

find_package(Threads REQUIRED)
add_executable(lle-ring-concurrency-test integration/ring_concurrency_test.cpp)
target_link_libraries(lle-ring-concurrency-test PRIVATE lle::lle lle_project_warnings GTest::gtest_main Threads::Threads)
gtest_discover_tests(lle-ring-concurrency-test TEST_PREFIX "lle.shm.concurrent." PROPERTIES TIMEOUT 60)
