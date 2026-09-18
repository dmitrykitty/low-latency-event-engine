if(LLE_ENABLE_ASAN OR LLE_ENABLE_TSAN OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Comparative benchmarks require Release without sanitizers")
endif()

include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(rigtorp
    URL https://github.com/rigtorp/SPSCQueue/archive/565a5149d54930463d58cb0f69b978d439555e66.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR unused)
FetchContent_Declare(boost_headers
    URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(googlebenchmark rigtorp boost_headers)
find_package(Threads REQUIRED)

add_executable(lle-spsc-benchmark micro/spsc_benchmark.cpp)
target_link_libraries(lle-spsc-benchmark PRIVATE lle::lle benchmark::benchmark Threads::Threads lle_project_warnings)
target_include_directories(lle-spsc-benchmark SYSTEM PRIVATE ${rigtorp_SOURCE_DIR}/include ${boost_headers_SOURCE_DIR})
