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
#ifndef ACC_LINKS_ACC_TCP_WORKER_H
#define ACC_LINKS_ACC_TCP_WORKER_H

#include <utility>

#include "acc_tcp_common.h"
#include "acc_tcp_link.h"
#include "acc_tcp_link_default.h"
#include "acc_tcp_request_context.h"
#include "acc_tcp_shared_buf.h"

namespace ock {
namespace acc {
using LinkBrokenHandlerInner = std::function<int32_t(const AccTcpLinkDefaultPtr &link)>;

/*
 * Worker is for epoll event from connection sockets
 */
class AccTcpWorker : public AccReferable {
public:
    explicit AccTcpWorker(AccTcpWorkerOptions options) : options_(std::move(options)) {}
    ~AccTcpWorker() override
    {
        Stop();
    }

    Result Start();
    void Stop(bool afterFork = false);

    Result AddLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept;
    Result ModifyLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept;
    Result RemoveLink(const AccTcpLinkDefaultPtr &link) noexcept;

    void RegisterNewRequestHandler(const AccNewReqHandler &h);
    void RegisterRequestSentHandler(const AccReqSentHandler &h);
    void RegisterLinkBrokenHandler(const LinkBrokenHandlerInner &h);

    /*
     * @brief 累计不可观测时长(ms)：单轮 epoll 迭代墙钟耗时超出轮询预算的部分之和，含当前这一轮。
     *        worker 被长耗时回调占住期间收不到也记不了心跳，上层据此从心跳静默时长中扣除。
     */
    uint64_t UnobservableMs() const noexcept;

    /*
     * @brief 由相邻两轮迭代的起始时刻计算本轮超出预算的时长
     * @param prevIterStartUs [in] 上一轮迭代起始时刻(us)，0 表示首轮
     * @return 超出轮询预算的时长(ms)，未超出返回 0
     */
    static uint64_t CalcOverrunMs(uint64_t iterStartUs, uint64_t prevIterStartUs, uint32_t budgetMs) noexcept;

private:
    void SetPropertiesForThread();
    void RunInThread(std::atomic<bool> *started);
    Result ValidateOptions();
    void StopInner(bool afterFork);
    Result ProcessEvent(struct epoll_event &event) noexcept;
    Result ProcessPollIn(AccTcpLinkDefault *link) noexcept;
    Result ProcessPollOut(AccTcpLinkDefault *link) noexcept;
    Result ProcessPollWrNorm(AccTcpLinkDefault *link) noexcept;
    void ProcessBufferedRequest(AccTcpLinkDefault *link) noexcept;

private:
    /* 空闲一轮约耗时 pollingTimeoutMs，叠加余量后才计为超支，避免调度抖动被计入 */
    static constexpr uint32_t kPollBudgetMarginMs = 100U;

    uint32_t PollBudgetMs() const noexcept
    {
        return options_.pollingTimeoutMs + kPollBudgetMarginMs;
    }

private:
    int epollFD_ = -1;      /* epoll fd */
    bool needStop_ = false; /* if the worker need to be stopped */
    AccNewReqHandler newRequestHandle_ = nullptr;
    AccReqSentHandler requestSentHandle_ = nullptr;
    LinkBrokenHandlerInner linkBrokenHandle_ = nullptr;

    /* non-hot variables */
    std::mutex mutex_;
    AccTcpWorkerOptions options_;      /* worker options */
    std::atomic<bool> started_{false}; /* if the worker started */
    std::thread epollThread_;          /* thread */
    std::atomic<bool> threadStarted_{false};

    /* iterStartUs_ 为本轮迭代起始时刻(us)，0 表示尚未进入首轮；由 worker 线程写、判定线程读 */
    std::atomic<uint64_t> iterStartUs_{0};
    std::atomic<uint64_t> overrunBaseMs_{0}; /* 已结算的累计超支(ms) */
};
using AccTcpWorkerPtr = AccRef<AccTcpWorker>;

inline void AccTcpWorker::RegisterNewRequestHandler(const AccNewReqHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(newRequestHandle_ == nullptr);
    newRequestHandle_ = h;
}

inline void AccTcpWorker::RegisterRequestSentHandler(const AccReqSentHandler &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(requestSentHandle_ == nullptr);
    requestSentHandle_ = h;
}

inline void AccTcpWorker::RegisterLinkBrokenHandler(const LinkBrokenHandlerInner &h)
{
    ASSERT_RET_VOID(h != nullptr);
    ASSERT_RET_VOID(linkBrokenHandle_ == nullptr);
    linkBrokenHandle_ = h;
}
} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_TCP_WORKER_H
