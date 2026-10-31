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
#ifndef ACC_LINKS_ACC_TCP_SERVER_DEFAULT_H
#define ACC_LINKS_ACC_TCP_SERVER_DEFAULT_H

#include "acc_includes.h"
#include "acc_tcp_link_default.h"
#include "acc_tcp_link_delay_cleanup.h"
#include "acc_tcp_listener.h"
#include "acc_tcp_ssl_helper.h"
#include "acc_tcp_worker.h"

namespace ock {
namespace acc {
class AccTcpServerDefault : public virtual AccTcpServer {
public:
    AccTcpServerDefault() = default;
    ~AccTcpServerDefault() override;

    Result Start(const AccTcpServerOptions &opt, const AccTlsOption &tlsOption) override;

    Result LoadDynamicLib(const std::string &dynLibPath) override;

    void Stop() override;
    void StopAfterFork() override;

    Result ConnectToPeerServer(const std::string &peerIp, uint16_t port, const AccConnReq &req, uint32_t maxRetryTimes,
                               AccTcpLinkComplexPtr &newLink) override;

    void RegisterNewRequestHandler(int16_t msgType, const AccNewReqHandler &h) override;
    void RegisterRequestSentHandler(int16_t msgType, const AccReqSentHandler &h) override;
    void RegisterLinkBrokenHandler(const AccLinkBrokenHandler &h) override;
    void RegisterNewLinkHandler(const AccNewLinkHandler &h) override;
    void RegisterDecryptHandler(const AccDecryptHandler &h) override;

    uint64_t GetWorkerUnobservableMs(uint32_t workerIdx) const override;

    virtual Result HandleRequestSent(AccMsgSentResult msgResult, const AccMsgHeader &header,
                                     const AccDataBufferPtr &cbCtx);
    virtual Result HandleLinkBroken(const AccTcpLinkDefaultPtr &link);
    Result ReleaseLink(const AccTcpLinkComplexPtr &link) override;

protected:
    void StopAndCleanWorkers(bool afterFork = false);

    virtual Result HandleNewConnection(const AccConnReq &req, const AccTcpLinkDefaultPtr &newLink);
    bool WorkerLinkLimitCheck(uint32_t workerIdx);
    void WorkerLinkCntUpdate(uint32_t workerIdx);
    Result WorkerSelect();

    Result HandleNewRequest(const AccTcpRequestContext &context);

    AccNewReqHandler newRequestHandle_[UNO_48]{};
    AccReqSentHandler requestSentHandle_[UNO_48]{};
    AccLinkBrokenHandler linkBrokenHandle_ = nullptr;
    AccDecryptHandler decryptHandler_ = nullptr;
    std::vector<AccTcpWorkerPtr> workers_;
    /* 叶子锁，仅用于同步"查询不可观测时长"读 workers_ 与停止清理时 clear 的摘除动作；
       与 mutex_ 无嵌套关系，避免把 acc 回调路径上的长持锁牵连进心跳查询 */
    mutable std::mutex workersMutex_;
    AccTcpListenerPtr listener_;
    std::atomic<uint32_t> nextWorkerIndex_{0};
    std::unordered_map<uint32_t, AccTcpLinkDefaultPtr> connectedLinks_;
    AccNewLinkHandler newLinkHandle_ = nullptr;
    AccTcpLinkDelayCleanupPtr delayCleanup_{nullptr};
    std::mutex linkCntMutex;
    std::unordered_map<uint32_t, uint32_t> workerLinkCnt_;
    uint32_t maxWorkerLinkeCnt_ = UNO_1024;

    std::mutex mutex_;
    std::atomic<bool> started_{false};
    AccTcpServerOptions options_;
    AccTcpSslHelperPtr sslHelper_ = nullptr;
    SSL_CTX *sslCtx_ = nullptr;
    AccTlsOption tlsOption_{};
    LinkFactoryFn linkFactory_;
    bool linkBlocking_{true};

private:
    Result ValidateOptions() const;
    virtual Result ValidateHandler() const;
    Result StartDelayCleanup();
    virtual Result StartWorkers();
    virtual Result StartListener();
    void StopAndCleanDelayCleanup(bool afterFork = false);
    void StopAndCleanListener(bool afterFork = false);
    void StopAndCleanSSLHelper(bool afterFork = false);

    Result GenerateSslCtx();
    Result CreateSSLLink(SSL *&ssl, int &tmpFD);
    void ValidateSSLLink(SSL *&ssl, int &tmpFD);
    Result LinkReceive(AccRef<AccTcpLinkComplexDefault> &tmpLink, const std::string &ipAndPort);

    Result Handshake(int &fd, const AccConnReq &connReq, const std::string &ipAndPort, AccTcpLinkComplexPtr &newLink);
};
using AccTcpServerDefaultPtr = AccRef<AccTcpServerDefault>;

inline void AccTcpServerDefault::RegisterNewRequestHandler(int16_t msgType, const AccNewReqHandler &h)
{
    ASSERT_RET_VOID(msgType >= MIN_MSG_TYPE && msgType < MAX_MSG_TYPE);
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(newRequestHandle_[msgType] == nullptr);
    newRequestHandle_[msgType] = h;
}

inline void AccTcpServerDefault::RegisterRequestSentHandler(int16_t msgType, const AccReqSentHandler &h)
{
    ASSERT_RET_VOID(msgType >= MIN_MSG_TYPE && msgType < MAX_MSG_TYPE);
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(requestSentHandle_[msgType] == nullptr);
    requestSentHandle_[msgType] = h;
}

inline void AccTcpServerDefault::RegisterLinkBrokenHandler(const AccLinkBrokenHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(linkBrokenHandle_ == nullptr);
    linkBrokenHandle_ = h;
}

inline void AccTcpServerDefault::RegisterNewLinkHandler(const AccNewLinkHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(newLinkHandle_ == nullptr);
    newLinkHandle_ = h;
}

inline void AccTcpServerDefault::RegisterDecryptHandler(const AccDecryptHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(decryptHandler_ == nullptr);
    decryptHandler_ = h;
}

inline Result AccTcpServerDefault::HandleNewRequest(const AccTcpRequestContext &context)
{
    auto msgType = context.MsgType();
    ASSERT_RETURN(msgType >= MIN_MSG_TYPE && msgType < MAX_MSG_TYPE, ACC_LINK_MSG_INVALID);
    auto &handler = newRequestHandle_[msgType];
    if (UNLIKELY(handler == nullptr)) {
        LOG_ERROR("NewRequestHandler is not register for msg type " << msgType << ", msg dropped");
        return ACC_LINK_MSG_INVALID;
    }

    return handler(context);
}

inline Result AccTcpServerDefault::HandleRequestSent(AccMsgSentResult msgResult, const AccMsgHeader &header,
                                                     const AccDataBufferPtr &cbCtx)
{
    auto msgType = header.type;
    ASSERT_RETURN(msgType >= MIN_MSG_TYPE && msgType < MAX_MSG_TYPE, ACC_LINK_MSG_INVALID);
    auto &handler = requestSentHandle_[msgType];
    if (handler == nullptr) {
        LOG_DEBUG("RequestSentHandler is not register for msg type " << msgType << ", msg dropped");
        return ACC_LINK_MSG_INVALID;
    }

    return handler(msgResult, header, cbCtx);
}

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_TCP_SERVER_DEFAULT_H
