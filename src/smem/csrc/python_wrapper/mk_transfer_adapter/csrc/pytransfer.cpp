/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" // ignore pybind11 warning

#include "pytransfer.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <pybind11/stl.h>
#include "mf_env_define.h"
#include "transfer_util.h"
#include "smem_ptracer.h"
#include "adapter_logger.h"

using namespace ock::adapter;

namespace py = pybind11;

static const char *PY_TRANSFER_LIB_VERSION =
    "library version: " STR2(PROJECT_VERSION_RAW) ", build time: " __DATE__ " " __TIME__
                                                  ", commit: " STR2(GIT_LAST_COMMIT);
constexpr uint64_t MAX_BATCH_COUNT = 1024 * 1024;

TransferAdapterPy::TransferAdapterPy() {}

TransferAdapterPy::~TransferAdapterPy()
{
    // Without ordered teardown the process exits with store/dispatcher/HCOM
    // threads still running while static destructors free SMEM/HCOM state,
    // which corrupts the heap (e.g. abort in malloc_consolidate) at exit.
    py::gil_scoped_release gilRelease;
    Shutdown();
}

int TransferAdapterPy::Initialize(const char *storeUrl, const char *uniqueId, const char *role, uint32_t deviceId,
                                  TransDataOpType dataOpType, const char *storeServerRole, const char *nic)
{
    if (strcmp(role, "Prefill") != 0 && strcmp(role, "Decode") != 0) {
        ADAPTER_LOG_ERROR("The value of role is invalid. Expected 'Prefill' or 'Decode.");
        return -1;
    }
    if (uniqueId == nullptr || strlen(uniqueId) == 0 || strlen(uniqueId) >= sizeof(config_.url)) {
        ADAPTER_LOG_ERROR("uniqueId is invalid or too long, uniqueId=" << (uniqueId == nullptr ? "null" : uniqueId)
                                                                       << ", maxLen=" << (sizeof(config_.url) - 1));
        return -1;
    }

    // set log level from env (SMEM OutLogger)
    const std::string &mfLevel = ock::mf::env::MF_LOG_LEVEL;
    if (mfLevel.size() == 1) {
        unsigned char c = static_cast<unsigned char>(mfLevel[0]);
        if (std::isdigit(c)) {
            int level = c - '0';
            smem_set_log_level(level);
            ock::mf::OutLogger::Instance().SetLogLevel(static_cast<ock::mf::LogLevel>(level));
        }
    }
    smem_set_conf_store_tls(false, nullptr, 0);

    // The config store protocol is fixed to tcp, storeUrl and storeServerRole are
    // kept only for python compatibility and take no effect any more.
    (void)storeServerRole;
    ADAPTER_LOG_INFO("Begin to initialize trans, role=" << role << ", uniqueId=" << uniqueId
                                                        << ", storeUrl=" << (storeUrl == nullptr ? "" : storeUrl)
                                                        << ", config store protocol is fixed to tcp");

    // init config
    int32_t ret = smem_trans_config_init(&config_);
    ADAPTER_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Failed to init smem_trans_config, ret=" << ret);

    config_.role = (strcmp(role, "Prefill") == 0) ? SMEM_TRANS_SENDER : SMEM_TRANS_RECEIVER;
    config_.deviceId = deviceId;
    config_.dataOpType = static_cast<smem_bm_data_op_type>(dataOpType);
    auto copyLen = strlen(uniqueId);
    std::copy_n(uniqueId, copyLen, config_.url);
    config_.url[copyLen] = '\0';

    if (nic != nullptr && strlen(nic) > 0) {
        auto nicLen = std::min(strlen(nic), sizeof(config_.nic) - 1);
        std::copy_n(nic, nicLen, config_.nic);
        config_.nic[nicLen] = '\0';
    }

    ret = smem_trans_init(deviceId, 0);
    ADAPTER_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Failed to init smem_trans, ret=" << ret << " deviceId=" << deviceId);
    smemInited_ = true;

    // the store server is created inside the entry, connections to peers are
    // established lazily on first copy with the remote unique id
    handle_ = smem_trans_create(&config_, 0);
    if (handle_ == nullptr) {
        ADAPTER_LOG_ERROR("smem_trans_create failed, url=" << config_.url << " deviceId=" << deviceId);
        return -1;
    }

    ADAPTER_LOG_INFO("initialized trans, url=" << config_.url);
    return 0;
}

int TransferAdapterPy::GetRpcPort()
{
    if (handle_ == nullptr) {
        ADAPTER_LOG_WARN("get_rpc_port returns 0; call initialize() first to get the real listening port");
        return 0;
    }
    uint16_t port = 0;
    int ret = smem_trans_get_rpc_port(handle_, &port);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_get_rpc_port error, ret=" << ret);
        return 0;
    }
    ADAPTER_LOG_INFO("Get rpcPort is " << port);
    return static_cast<int>(port);
}

int TransferAdapterPy::TransferSyncWrite(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address,
                                         size_t length, uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    const void *srcAddress = reinterpret_cast<const void *>(buffer);
    void *destAddress = reinterpret_cast<void *>(peer_buffer_address);
    TP_TRACE_BEGIN(TP_TRANS_WRITE);
    int ret = smem_trans_write(handle_, srcAddress, destUniqueId, destAddress, length, flags);
    TP_TRACE_END(TP_TRANS_WRITE, ret);
    TP_TRACE_RECORD(TP_TRANS_WRITE_SIZE, length * 1000ULL, 0);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_write error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::BatchTransferSyncWrite(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                              std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                                              uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    if (buffers.size() != peer_buffer_addresses.size() || buffers.size() != lengths.size() ||
        buffers.size() > UINT32_MAX) {
        ADAPTER_LOG_ERROR("Buffers, peer_buffer_addresses and lengths is not equal or too long.");
        return -1;
    }

    const size_t batchSize = buffers.size();
    std::vector<const void *> srcAddresses(batchSize);
    std::vector<void *> destAddresses(batchSize);
    std::vector<size_t> dataSizes(batchSize);
    size_t totalSize = 0;
    for (size_t i = 0; i < batchSize; ++i) {
        srcAddresses[i] = reinterpret_cast<const void *>(buffers[i]);
        destAddresses[i] = reinterpret_cast<void *>(peer_buffer_addresses[i]);
        dataSizes[i] = lengths[i];
        totalSize += lengths[i];
    }
    TP_TRACE_BEGIN(TP_TRANS_BATCH_WRITE);
    int ret = smem_trans_batch_write(handle_, srcAddresses.data(), destUniqueId, destAddresses.data(), dataSizes.data(),
                                     static_cast<uint32_t>(batchSize), flags);
    TP_TRACE_END(TP_TRANS_BATCH_WRITE, ret);
    TP_TRACE_RECORD(TP_TRANS_BATCH_WRITE_SIZE, totalSize * 1000ULL, 0);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_batch_write error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::TransferSyncRead(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address,
                                        size_t length, uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    void *srcAddress = reinterpret_cast<void *>(buffer);
    const void *destAddress = reinterpret_cast<const void *>(peer_buffer_address);
    TP_TRACE_BEGIN(TP_TRANS_READ);
    int ret = smem_trans_read(handle_, srcAddress, destUniqueId, destAddress, length, flags);
    TP_TRACE_END(TP_TRANS_READ, ret);
    TP_TRACE_RECORD(TP_TRANS_READ_SIZE, length * 1000ULL, 0);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_read error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::TransferAsyncReadSubmit(const char *destUniqueId, uintptr_t buffer,
                                               uintptr_t peer_buffer_address, size_t length, uintptr_t stream,
                                               uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    void *srcAddress = reinterpret_cast<void *>(buffer);
    const void *destAddress = reinterpret_cast<const void *>(peer_buffer_address);
    void *st = reinterpret_cast<void *>(stream);

    int ret = smem_trans_read_submit(handle_, srcAddress, destUniqueId, destAddress, length, st, flags);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_read_submit error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::TransferAsyncWriteSubmit(const char *destUniqueId, uintptr_t buffer,
                                                uintptr_t peer_buffer_address, size_t length, uintptr_t stream,
                                                uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    const void *srcAddress = reinterpret_cast<const void *>(buffer);
    void *destAddress = reinterpret_cast<void *>(peer_buffer_address);
    void *st = reinterpret_cast<void *>(stream);

    int ret = smem_trans_write_submit(handle_, srcAddress, destUniqueId, destAddress, length, st, flags);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_write_submit error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::BatchTransferSyncRead(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                             std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                                             uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    if (buffers.size() != peer_buffer_addresses.size() || buffers.size() != lengths.size() ||
        buffers.size() > UINT32_MAX) {
        ADAPTER_LOG_ERROR("Buffers, peer_buffer_addresses and lengths is not equal or too long.");
        return -1;
    }

    const size_t batchSize = buffers.size();
    std::vector<void *> srcAddresses(batchSize);
    std::vector<const void *> destAddresses(batchSize);
    std::vector<size_t> dataSizes(batchSize);
    size_t totalSize = 0;
    for (size_t i = 0; i < batchSize; ++i) {
        srcAddresses[i] = reinterpret_cast<void *>(buffers[i]);
        destAddresses[i] = reinterpret_cast<const void *>(peer_buffer_addresses[i]);
        dataSizes[i] = lengths[i];
        totalSize += lengths[i];
    }
    TP_TRACE_BEGIN(TP_TRANS_BATCH_READ);
    int ret = smem_trans_batch_read(handle_, srcAddresses.data(), destUniqueId, destAddresses.data(), dataSizes.data(),
                                    static_cast<uint32_t>(batchSize), flags);
    TP_TRACE_END(TP_TRANS_BATCH_READ, ret);
    TP_TRACE_RECORD(TP_TRANS_BATCH_READ_SIZE, totalSize * 1000ULL, 0);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_batch_read error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::BatchTransferAsyncWriteSubmit(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                                     std::vector<uintptr_t> peer_buffer_addresses,
                                                     std::vector<size_t> lengths, uintptr_t stream, uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    if (buffers.size() != peer_buffer_addresses.size() || buffers.size() != lengths.size() ||
        buffers.size() > UINT32_MAX) {
        ADAPTER_LOG_ERROR("Buffers, peer_buffer_addresses and lengths is not equal or too long.");
        return -1;
    }

    const size_t batchSize = buffers.size();
    std::vector<const void *> srcAddresses(batchSize);
    std::vector<void *> destAddresses(batchSize);
    std::vector<size_t> dataSizes(batchSize);
    void *st = reinterpret_cast<void *>(stream);

    for (size_t i = 0; i < batchSize; ++i) {
        srcAddresses[i] = reinterpret_cast<const void *>(buffers[i]);
        destAddresses[i] = reinterpret_cast<void *>(peer_buffer_addresses[i]);
        dataSizes[i] = lengths[i];
    }

    int ret = smem_trans_batch_write_submit(handle_, srcAddresses.data(), destUniqueId, destAddresses.data(),
                                            dataSizes.data(), static_cast<uint32_t>(batchSize), st, flags);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_batch_write_submit error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::BatchTransferAsyncReadSubmit(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                                    std::vector<uintptr_t> peer_buffer_addresses,
                                                    std::vector<size_t> lengths, uintptr_t stream, uint32_t flags)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    if (buffers.size() != peer_buffer_addresses.size() || buffers.size() != lengths.size() ||
        buffers.size() > UINT32_MAX) {
        ADAPTER_LOG_ERROR("Buffers, peer_buffer_addresses and lengths is not equal or too long.");
        return -1;
    }

    const size_t batchSize = buffers.size();
    std::vector<void *> srcAddresses(batchSize);
    std::vector<const void *> destAddresses(batchSize);
    std::vector<size_t> dataSizes(batchSize);
    void *st = reinterpret_cast<void *>(stream);

    for (size_t i = 0; i < batchSize; ++i) {
        srcAddresses[i] = reinterpret_cast<void *>(buffers[i]);
        destAddresses[i] = reinterpret_cast<const void *>(peer_buffer_addresses[i]);
        dataSizes[i] = lengths[i];
    }

    int ret = smem_trans_batch_read_submit(handle_, srcAddresses.data(), destUniqueId, destAddresses.data(),
                                           dataSizes.data(), static_cast<uint32_t>(batchSize), st, flags);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_trans_batch_read_submit error, ret=" << ret << " dest=" << destUniqueId);
    }
    return ret;
}

int TransferAdapterPy::RegisterMemory(uintptr_t buffer_addr, size_t capacity)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);
    char *buffer = reinterpret_cast<char *>(buffer_addr);
    return smem_trans_register_mem(handle_, buffer, capacity, 0);
}

int TransferAdapterPy::UnregisterMemory(uintptr_t buffer_addr)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);
    char *buffer = reinterpret_cast<char *>(buffer_addr);
    return smem_trans_deregister_mem(handle_, buffer);
}

int TransferAdapterPy::BatchRegisterMemory(std::vector<uintptr_t> buffer_addrs, std::vector<size_t> capacities)
{
    ADAPTER_ASSERT_RETURN(handle_ != nullptr, "handle_ is null", -1);

    if (buffer_addrs.size() != capacities.size()) {
        ADAPTER_LOG_ERROR("Size of buffer_addrs and capacities is not equal.");
        return -1;
    }

    const size_t count = buffer_addrs.size();
    if (count > MAX_BATCH_COUNT) {
        ADAPTER_LOG_ERROR("array size (" << count << ") exceeds limit(" << MAX_BATCH_COUNT << ")");
        return -1;
    }

    std::vector<void *> registerAddrs(count);
    for (size_t i = 0; i < count; ++i) {
        registerAddrs[i] = reinterpret_cast<void *>(buffer_addrs[i]);
    }

    return smem_trans_batch_register_mem(handle_, registerAddrs.data(), capacities.data(), static_cast<uint32_t>(count),
                                         0);
}

void TransferAdapterPy::TransferDestroy()
{
    if (handle_ != nullptr) {
        smem_trans_destroy(handle_, 0);
        handle_ = nullptr;
    }
}

void TransferAdapterPy::UnInitialize()
{
    if (smemInited_) {
        smem_trans_uninit(0);
        smemInited_ = false;
    }
}

void TransferAdapterPy::Shutdown()
{
    TransferDestroy();
    UnInitialize();
}

void DefineAdapterFunctions(py::module_ &m)
{
    m.def("create_config_store", &pytransfer_create_config_store, py::call_guard<py::gil_scoped_release>(),
          py::arg("store_url"));

    m.def("set_log_level", &pytransfer_set_log_level, py::call_guard<py::gil_scoped_release>(), py::arg("level"), R"(
set log print level.

Arguments:
    level(int): log level, 0:debug 1:info 2:warn 3:error)");
    m.def("set_conf_store_tls", &pytransfer_set_conf_store_tls, py::call_guard<py::gil_scoped_release>(),
          py::arg("enable"), py::arg("tls_info"), R"(
set the config store tls info.
Parameters:
    enable (boolean): enable config store tls or not
        tls_info (string): tls config string
Returns:
    returns zero on success. On error, none-zero is returned.
)");
    m.doc() = PY_TRANSFER_LIB_VERSION;
}

PYBIND11_MODULE(_pymf_transfer, m)
{
    py::enum_<TransferAdapterPy::TransferOpcode> transfer_opcode(m, "TransferOpcode", py::arithmetic());
    transfer_opcode.value("Read", TransferAdapterPy::TransferOpcode::READ)
        .value("Write", TransferAdapterPy::TransferOpcode::WRITE)
        .export_values();
    py::enum_<TransferAdapterPy::TransDataOpType> transfer_type(m, "TransDataOpType", py::arithmetic());
    transfer_type.value("SDMA", TransferAdapterPy::TransDataOpType::SDMA)
        .value("HOST_RDMA", TransferAdapterPy::TransDataOpType::HOST_RDMA)
        .value("DEVICE_RDMA", TransferAdapterPy::TransDataOpType::DEVICE_RDMA)
        .value("DEVICE_URMA", TransferAdapterPy::TransDataOpType::DEVICE_URMA)
        .value("DEVICE_UBOE", TransferAdapterPy::TransDataOpType::DEVICE_UBOE)
        .export_values();

    DefineAdapterFunctions(m);

    auto adaptor_cls =
        py::class_<TransferAdapterPy>(m, "TransferEngine")
            .def(py::init<>())
            .def("initialize", &TransferAdapterPy::Initialize, py::call_guard<py::gil_scoped_release>(),
                 py::arg("store_url"), py::arg("session_id"), py::arg("role"), py::arg("device_id"),
                 py::arg("data_op_type") = TransferAdapterPy::TransDataOpType::SDMA,
                 py::arg("store_server_role") = "Decode", py::arg("nic") = "")
            .def("get_rpc_port", &TransferAdapterPy::GetRpcPort, py::call_guard<py::gil_scoped_release>())
            .def("transfer_sync_write", &TransferAdapterPy::TransferSyncWrite, py::call_guard<py::gil_scoped_release>(),
                 py::arg("dest_session"), py::arg("buffer"), py::arg("peer_buffer"), py::arg("length"),
                 py::arg("flags") = 0)
            .def("batch_transfer_sync_write", &TransferAdapterPy::BatchTransferSyncWrite,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffers"),
                 py::arg("peer_buffers"), py::arg("lengths"), py::arg("flags") = 0)
            .def("batch_transfer_async_write_submit", &TransferAdapterPy::BatchTransferAsyncWriteSubmit,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffers"),
                 py::arg("peer_buffers"), py::arg("lengths"), py::arg("stream"), py::arg("flags") = 0)
            .def("transfer_sync_read", &TransferAdapterPy::TransferSyncRead, py::call_guard<py::gil_scoped_release>(),
                 py::arg("dest_session"), py::arg("buffer"), py::arg("peer_buffer"), py::arg("length"),
                 py::arg("flags") = 0)
            .def("transfer_async_write_submit", &TransferAdapterPy::TransferAsyncWriteSubmit,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffer"),
                 py::arg("peer_buffer"), py::arg("length"), py::arg("stream"), py::arg("flags") = 0)
            .def("transfer_async_read_submit", &TransferAdapterPy::TransferAsyncReadSubmit,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffer"),
                 py::arg("peer_buffer"), py::arg("length"), py::arg("stream"), py::arg("flags") = 0)
            .def("batch_transfer_sync_read", &TransferAdapterPy::BatchTransferSyncRead,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffers"),
                 py::arg("peer_buffers"), py::arg("lengths"), py::arg("flags") = 0)
            .def("batch_transfer_async_read_submit", &TransferAdapterPy::BatchTransferAsyncReadSubmit,
                 py::call_guard<py::gil_scoped_release>(), py::arg("dest_session"), py::arg("buffers"),
                 py::arg("peer_buffers"), py::arg("lengths"), py::arg("stream"), py::arg("flags") = 0)
            .def("register_memory", &TransferAdapterPy::RegisterMemory, py::call_guard<py::gil_scoped_release>(),
                 py::arg("buffer_addr"), py::arg("capacity"))
            .def("unregister_memory", &TransferAdapterPy::UnregisterMemory, py::call_guard<py::gil_scoped_release>(),
                 py::arg("buffer_addr"))
            .def("batch_register_memory", &TransferAdapterPy::BatchRegisterMemory,
                 py::call_guard<py::gil_scoped_release>(), py::arg("buffer_addrs"), py::arg("capacities"))
            .def("destroy", &TransferAdapterPy::TransferDestroy, py::call_guard<py::gil_scoped_release>())
            .def("unInitialize", &TransferAdapterPy::UnInitialize, py::call_guard<py::gil_scoped_release>());

    adaptor_cls.attr("TransferOpcode") = transfer_opcode;
    adaptor_cls.attr("TransDataOpType") = transfer_type;
}

#pragma GCC diagnostic pop
