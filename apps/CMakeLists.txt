find_package(Threads REQUIRED)

# engine processes and demo endpoints; app_support.hpp holds their shared CLI helpers.
foreach(app lle-sender lle-receiver lle-demo-producer lle-demo-consumer)
    if(app STREQUAL "lle-demo-producer")
        add_executable(${app} demo/producer.cpp)
    elseif(app STREQUAL "lle-demo-consumer")
        add_executable(${app} demo/consumer.cpp)
    else()
        add_executable(${app} ${app}/main.cpp)
    endif()
    target_include_directories(${app} PRIVATE common)
    target_link_libraries(${app} PRIVATE lle::lle lle_project_warnings Threads::Threads)
endforeach()

add_executable(lle-udp-smoke udp_smoke.cpp)
target_link_libraries(lle-udp-smoke PRIVATE lle::lle lle_project_warnings)

if(BUILD_TESTING)
    # runs the four programs as separate processes over UDP loopback.
    add_test(NAME lle.apps.local_demo
             COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/demo/run_local.sh)
    set_tests_properties(lle.apps.local_demo PROPERTIES
        ENVIRONMENT "LLE_DEMO_BIN_DIR=$<TARGET_FILE_DIR:lle-sender>;LLE_DEMO_COUNT=20000;LLE_DEMO_RATE=20000;LLE_DEMO_STRICT=1"
        TIMEOUT 60)
endif()
