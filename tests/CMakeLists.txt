add_executable(lle-smoke-test unit/smoke_test.cpp)
target_link_libraries(lle-smoke-test PRIVATE lle::lle lle_project_warnings)

add_test(NAME lle.smoke COMMAND lle-smoke-test)

add_executable(lle-shm-segment-test unit/shm_segment_test.cpp)
target_link_libraries(lle-shm-segment-test PRIVATE lle::lle lle_project_warnings)

add_test(NAME lle.shm.segment COMMAND lle-shm-segment-test)
