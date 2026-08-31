add_executable(lle-smoke-test unit/smoke_test.cpp)
target_link_libraries(lle-smoke-test PRIVATE lle::lle lle_project_warnings)

add_test(NAME lle.smoke COMMAND lle-smoke-test)

