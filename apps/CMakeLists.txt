add_executable(lle-sender lle-sender/main.cpp)
target_link_libraries(lle-sender PRIVATE lle::lle lle_project_warnings)

add_executable(lle-receiver lle-receiver/main.cpp)
target_link_libraries(lle-receiver PRIVATE lle::lle lle_project_warnings)

add_executable(lle-udp-smoke udp_smoke.cpp)
target_link_libraries(lle-udp-smoke PRIVATE lle::lle lle_project_warnings)

