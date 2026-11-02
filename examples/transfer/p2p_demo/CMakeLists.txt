cmake_minimum_required(VERSION 3.12)
project(p2p_demo)

include(GNUInstallDirs)

# memfabric_hybrid install root (for smem_trans.h and libmf_smem.so)
set(MF_PATH $ENV{MEMFABRIC_HYBRID_HOME_PATH})
if ("${MF_PATH}" STREQUAL "")
    message(FATAL_ERROR "MEMFABRIC_HYBRID_HOME_PATH not set, please install memfabric_hybrid first")
endif ()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(TARGET_ARCH "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(TARGET_ARCH "aarch64")
else ()
    message(FATAL_ERROR "unsupported arch: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif ()
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_LIST_DIR}/out)

add_executable(p2p_demo ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

# _GLIBCXX_USE_CXX11_ABI=0 must match the memfabric_hybrid build (default old ABI)
target_compile_options(p2p_demo PRIVATE -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -Wall)

target_include_directories(p2p_demo PUBLIC
        ${MF_PATH}/include/smem/host
)

target_link_directories(p2p_demo PUBLIC
        ${MF_PATH}/${TARGET_ARCH}-linux/lib64
        ${MF_PATH}/${TARGET_ARCH}-linux/lib
)

target_link_libraries(p2p_demo PRIVATE mf_smem)

install(TARGETS p2p_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
