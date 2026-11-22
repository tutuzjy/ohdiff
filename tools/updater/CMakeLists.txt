add_executable(updater main.cpp directory_manager.cpp)
target_link_libraries(updater PRIVATE ohdiff_core)