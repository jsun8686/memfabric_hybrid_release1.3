#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# Build the p2p_demo example.
#
# Required environment variables:
#   MEMFABRIC_HYBRID_HOME_PATH  - memfabric_hybrid install root (run package)
set -e
CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)

if [ -z "${MEMFABRIC_HYBRID_HOME_PATH}" ]; then
    echo "[ERROR] MEMFABRIC_HYBRID_HOME_PATH is not set, install memfabric_hybrid first" >&2
    exit 1
fi

rm -rf build out
mkdir -p build
cmake -B build -S "${CURRENT_DIR}"
cmake --build build -j
cmake --install build
echo "[INFO] build done, binary at ${CURRENT_DIR}/out/bin/p2p_demo"
