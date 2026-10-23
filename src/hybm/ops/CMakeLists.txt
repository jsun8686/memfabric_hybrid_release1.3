cmake_minimum_required(VERSION 3.12.0)

# ---- Standalone project mode: invoked directly as cmake -S src/hybm/ops -B ... ----
# When added via add_subdirectory from the top-level project, CMAKE_SOURCE_DIR differs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hybm_ops LANGUAGES C CXX)

    # Calculate repo root: src/hybm/ops -> up 3 levels
    get_filename_component(_OPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    get_filename_component(_HYBM_DIR "${_OPS_DIR}/../.." REALPATH)
    get_filename_component(_SRC_DIR "${_HYBM_DIR}/.." REALPATH)
    get_filename_component(_REPO_ROOT "${_SRC_DIR}/.." REALPATH)

    set(HYBM_KERNEL_PROJECT_ROOT "${_REPO_ROOT}" CACHE PATH "Repo root for hybm kernel build")
    set(PROJECT_HYBM_SRC_BASE "${_REPO_ROOT}/src/hybm" CACHE PATH "hybm src base")
    set(PROJECT_UTIL_SRC_BASE "${_REPO_ROOT}/src/util/csrc" CACHE PATH "util src base")
    if(NOT TARGET_INSTALL_DIR)
        set(TARGET_INSTALL_DIR "${_REPO_ROOT}/output" CACHE PATH "Install destination")
    endif()
    set(CMAKE_INSTALL_PREFIX "${TARGET_INSTALL_DIR}")

    # AICPU kernel source files for cross-compilation (standalone mode)
    set(AICPU_KERNEL_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/hybm_kernel/hybm_batch_transfer.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/hybm_kernel/hybm_rdma_aicpu.cc"
        CACHE STRING "Source files for AICPU kernel cross-compilation")

    # Include dlog/slog config if present (added by prior changes)
    if(EXISTS "${HYBM_KERNEL_PROJECT_ROOT}/cmake/kernel/config_slog.cmake")
        include("${HYBM_KERNEL_PROJECT_ROOT}/cmake/kernel/config_slog.cmake")
    endif()

    message(STATUS "[hybm_ops] Standalone build")
    message(STATUS "[hybm_ops] HYBM_KERNEL_PROJECT_ROOT = ${HYBM_KERNEL_PROJECT_ROOT}")
    message(STATUS "[hybm_ops] TARGET_INSTALL_DIR       = ${TARGET_INSTALL_DIR}")
endif()

add_subdirectory(hybm_kernel)
