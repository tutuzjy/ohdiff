set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# C++ Modules =======
if(STATIC_BUILD)
    add_library(ohdiff_core STATIC)
else()
    add_library(ohdiff_core SHARED)
endif()
target_sources(ohdiff_core PRIVATE
        Backup.cpp
        BackupLayer.cpp
        BackupTask.cpp
        RestoreTask.cpp
        MergeTask.cpp
        Chunker.cpp
        datamov.cpp
        log/log.cpp
)
target_include_directories(ohdiff_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ohdiff_core Boost::program_options Boost::serialization TBB::tbb TBB::tbbmalloc xxHash::xxhash
                      libzstd_static fmt::fmt-header-only)
# C++ Modules =======

# Executable ========
add_executable(ohdiff main.cpp)
target_link_libraries(ohdiff ohdiff_core)
install(TARGETS ohdiff RUNTIME DESTINATION bin COMPONENT ohdiff_runtime)
if(NOT STATIC_BUILD)
    install(TARGETS ohdiff_core tbb tbbmalloc xxhash
            boost_program_options boost_serialization boost_thread boost_atomic boost_chrono boost_date_time boost_container
            LIBRARY DESTINATION lib COMPONENT ohdiff_runtime)
endif()
# Executable ========
