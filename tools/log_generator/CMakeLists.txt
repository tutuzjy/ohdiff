add_executable(log_generator main.cpp)
target_link_libraries(log_generator PRIVATE ohdiff_core)
install(TARGETS log_generator RUNTIME DESTINATION libexec/ohdiff COMPONENT ohdiff_runtime)
