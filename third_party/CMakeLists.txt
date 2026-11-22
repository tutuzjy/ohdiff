set(BUILD_TESTING OFF) # 为 third_party 子目录关闭测试构建

foreach(dependency IN ITEMS oneTBB boost/libs/program_options boost/libs/serialization xxHash/cmake_unofficial zstd/build/cmake fmt)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dependency}/CMakeLists.txt")
        message(FATAL_ERROR "Missing third_party/${dependency}. Run: git submodule update --init --recursive")
    endif()
endforeach()

# oneTBB
set(TBB_STRICT OFF CACHE BOOL "")
set(TBB_TEST OFF CACHE BOOL "")
set(TBB_INSTALL OFF CACHE BOOL "")
add_subdirectory(oneTBB)

set(BOOST_INCLUDE_LIBRARIES "program_options;serialization" CACHE STRING "Boost libraries built by ohdiff")
add_subdirectory(boost)
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    # Backport boostorg/core@9a3febf848cc9e94bc3c36d7384a77ab0a05c217 without modifying the submodule.
    set(boost_compat_include "${CMAKE_CURRENT_BINARY_DIR}/boost_compat")
    set(sp_thread_sleep "boost/core/detail/sp_thread_sleep.hpp")
    file(READ "boost/libs/core/include/${sp_thread_sleep}" boost_header)
    string(REPLACE "#if defined(BOOST_HAS_PTHREADS) && !defined(__ANDROID__)\n\n"
                   "#if defined(BOOST_HAS_PTHREADS) && !defined(__ANDROID__) && !defined(__OHOS__)\n\n"
                   boost_header "${boost_header}")
    file(WRITE "${boost_compat_include}/${sp_thread_sleep}" "${boost_header}")
    target_include_directories(boost_core BEFORE INTERFACE "$<BUILD_INTERFACE:${boost_compat_include}>")
endif()

add_subdirectory(xxHash/cmake_unofficial)

add_subdirectory(zstd/build/cmake)

set(FMT_INSTALL OFF CACHE BOOL "Disable installation of the bundled fmt library")
add_subdirectory(fmt EXCLUDE_FROM_ALL)
