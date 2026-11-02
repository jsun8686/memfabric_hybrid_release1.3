#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# Run the p2p_demo example.
#
# Required environment variables:
#   MEMFABRIC_HYBRID_HOME_PATH  - memfabric_hybrid install root (run package)
set -e
CURRENT_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)

if [ -z "${MEMFABRIC_HYBRID_HOME_PATH}" ]; then
    echo "[ERROR] MEMFABRIC_HYBRID_HOME_PATH is not set" >&2
    exit 1
fi

ARCH=$(uname -m)
export LD_LIBRARY_PATH="${MEMFABRIC_HYBRID_HOME_PATH}/${ARCH}-linux/lib64:${LD_LIBRARY_PATH}"

BIN="${CURRENT_DIR}/out/bin/p2p_demo"
if [ ! -f "${BIN}" ]; then
    echo "[ERROR] binary not found at ${BIN}, run build.sh first" >&2
    exit 1
fi
exec "${BIN}"
