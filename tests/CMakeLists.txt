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
