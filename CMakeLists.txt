cmake_minimum_required(VERSION 3.28)
project(ohdiff CXX)

set(CMAKE_CXX_STANDARD 23)

if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    set(OHdiff_STATIC_DEFAULT OFF)
else()
    set(OHdiff_STATIC_DEFAULT ON)
endif()
option(STATIC_BUILD "Static build." ${OHdiff_STATIC_DEFAULT})
option(BUILD_LOG_TOOLS "Build log generation and directory modification tools." OFF)

if (STATIC_BUILD)
    set(BUILD_SHARED_LIBS OFF)
else()
    set(BUILD_SHARED_LIBS ON)
endif()


# third party lib
add_subdirectory(third_party)

# ====

if (STATIC_BUILD)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
else()
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON) # RUNPATH 使用相对路径，保证程序在不同目录也能正常运行
endif()

add_compile_options(-Wno-reorder-init-list)

# main program
add_subdirectory(src)

if (BUILD_LOG_TOOLS)
    add_subdirectory(tools)
endif()

set(license_directory share/licenses/ohdiff)
install(FILES LICENSE DESTINATION ${license_directory} COMPONENT ohdiff_runtime)
install(FILES LICENSES/fastcdc-MIT.txt DESTINATION ${license_directory} COMPONENT ohdiff_runtime)
install(FILES third_party/oneTBB/LICENSE.txt DESTINATION ${license_directory}/oneTBB COMPONENT ohdiff_runtime)
install(FILES third_party/boost/LICENSE_1_0.txt DESTINATION ${license_directory}/boost COMPONENT ohdiff_runtime)
install(FILES third_party/xxHash/LICENSE DESTINATION ${license_directory}/xxHash COMPONENT ohdiff_runtime)
install(FILES third_party/zstd/LICENSE DESTINATION ${license_directory}/zstd COMPONENT ohdiff_runtime)
install(FILES third_party/fmt/LICENSE DESTINATION ${license_directory}/fmt COMPONENT ohdiff_runtime)
