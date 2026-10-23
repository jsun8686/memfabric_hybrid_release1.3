set(CMAKE_SKIP_RPATH TRUE)

# Allow standalone build: use HYBM_KERNEL_PROJECT_ROOT if defined, else fall back to PROJECT_SOURCE_DIR
if(NOT DEFINED HYBM_KERNEL_PROJECT_ROOT)
  set(HYBM_KERNEL_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
endif()
set(HYBM_AICPU_PACKAGE_NAME cann-hybm-compat.tar.gz)
set(AICPU_COMPILER "${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")
set(_ASCEND_INC_DIR "${ASCEND_HOME_PATH}/pkg_inc/base")

# Output artifacts in build directory (not directly to install dir)
set(AICPU_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_kernel.so")
set(AICPU_JSON_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_kernel.json")
set(AICPU_TAR_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${HYBM_AICPU_PACKAGE_NAME}")

set(AICPU_INCLUDE_DIRS
    ${PROJECT_HYBM_SRC_BASE}/include
    ${PROJECT_HYBM_SRC_BASE}/csrc/common
    ${PROJECT_HYBM_SRC_BASE}/csrc/under_api
    ${_ASCEND_INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

message("==================hybm ai cpu ops compile====================================")

# Build include flags
set(AICPU_INC_FLAGS)
foreach(INC_DIR ${AICPU_INCLUDE_DIRS})
  list(APPEND AICPU_INC_FLAGS "-I${INC_DIR}")
endforeach()

if(NOT DEFINED AICPU_KERNEL_SRC)
  set(AICPU_KERNEL_SRC
      "${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_transfer.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/hybm_rdma_aicpu.cc"
  )
endif()

set(AICPU_LINK_FLAGS "-ldl")

# ---- Copy static .json ----
add_custom_command(
    OUTPUT ${AICPU_JSON_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_kernel.json
            ${AICPU_JSON_OUTPUT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_kernel.json
)

# ---- Cross-compile .so ----
set(AICPU_KERNEL_DEPS
    ${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_transfer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hybm_rdma_aicpu.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hybm_kernel_log.h
    ${PROJECT_HYBM_SRC_BASE}/include/hybm_def.h
)

add_custom_command(
    OUTPUT ${AICPU_SO_OUTPUT}
    COMMAND ${AICPU_COMPILER}
        -shared -O2 -std=c++17 -fPIC
        ${AICPU_INC_FLAGS}
        ${AICPU_LINK_FLAGS}
        -o ${AICPU_SO_OUTPUT}
        ${AICPU_KERNEL_SRC}
    DEPENDS ${AICPU_KERNEL_SRC} ${AICPU_KERNEL_DEPS}
    COMMENT "Compiling AICPU kernel shared library with aarch64 cross-compiler"
)

# ---- Package .so into tar.gz (AICPUKernel mode) ----
set(AICPU_TAR_ROOT "aicpu_kernels_device")
set(AICPU_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/tar_staging")

add_custom_command(
    OUTPUT ${AICPU_TAR_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${AICPU_STAGING_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${AICPU_SO_OUTPUT}" "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}/"
    COMMAND tar czf "${AICPU_TAR_OUTPUT}" -C "${AICPU_STAGING_DIR}" "${AICPU_TAR_ROOT}"
    DEPENDS ${AICPU_SO_OUTPUT}
    COMMENT "Packaging AICPU kernel into tar.gz (TAR_ROOT_DIR=${AICPU_TAR_ROOT})"
)

message("==================hybm ai cpu ops compile done====================================")

# ---- Default target: build all artifacts ----
add_custom_target(cann_hybm_kernel ALL DEPENDS ${AICPU_SO_OUTPUT} ${AICPU_JSON_OUTPUT} ${AICPU_TAR_OUTPUT})

# ---- Install consumer artifacts ----
install(FILES ${AICPU_JSON_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

install(FILES ${AICPU_TAR_OUTPUT}
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

install(PROGRAMS ${HYBM_KERNEL_PROJECT_ROOT}/script/kernel/hybm/install.sh
        DESTINATION ${TARGET_INSTALL_DIR}/hybm/aicpu_kernel
        RENAME install.sh
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
