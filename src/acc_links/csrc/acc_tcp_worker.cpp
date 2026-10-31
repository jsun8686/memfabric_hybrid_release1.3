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
#include <pthread.h>
#include <sys/resource.h>

#include "acc_tcp_worker.h"
#include "mf_monotonic_time.h"

namespace ock {
namespace acc {
uint64_t AccTcpWorker::CalcOverrunMs(uint64_t iterStartUs, uint64_t prevIterStartUs, uint32_t budgetMs) noexcept
{
    /* 首轮无可比时刻，不计超支 */
    if (prevIterStartUs == 0 || iterStartUs <= prevIterStartUs) {
        return 0;
    }
    const uint64_t elapsedUs = iterStartUs - prevIterStartUs;
    const uint64_t budgetUs = static_cast<uint64_t>(budgetMs) * 1000ULL;
    return elapsedUs > budgetUs ? (elapsedUs - budgetUs) / 1000ULL : 0;
}

uint64_t AccTcpWorker::UnobservableMs() const noexcept
{
    /* 先 acquire 读 iterStartUs_、后读 baseMs：写侧顺序为"先 fetch_add baseMs、再 release 存
       iterStartUs"，此顺序保证读到新一轮起点时必能看到随之累加的 baseMs，避免漏掉刚结算的一轮超支 */
    const uint64_t iterStartUs = iterStartUs_.load(std::memory_order_acquire);
    const uint64_t baseMs = overrunBaseMs_.load(std::memory_order_relaxed);
    if (iterStartUs == 0) { /* 尚未进入首轮，无进行中的不可观测窗口 */
        return baseMs;
    }
    /* 加上当前这一轮已超出预算的部分：worker 正卡在回调里时，该超支尚未结算进 baseMs */
    return baseMs + CalcOverrunMs(mf::MonotonicTime::TimeUs(), iterStartUs, PollBudgetMs());
}

Result AccTcpWorker::Start()
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return ACC_OK;
    }

    auto result = ValidateOptions();
    if (result != ACC_OK) {
        started_.store(false);
        return result;
    }

    if ((epollFD_ = epoll_create(8192L)) < 0) {
        LOG_ERROR("Failed to create epoll in worker " << options_.Name() << ", errno " << errno);
        started_.store(false);
        return ACC_EPOLL_ERROR;
    }

    threadStarted_.store(false);

    std::thread tmpThread(&AccTcpWorker::RunInThread, this, &threadStarted_);
    epollThread_ = std::move(tmpThread);

    while (!threadStarted_.load()) {
        usleep(UNO_32);
    }

    return ACC_OK;
}

void AccTcpWorker::Stop(bool afterFork)
{
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

    StopInner(afterFork);
}

void AccTcpWorker::StopInner(bool afterFork)
{
    LOG_DEBUG("Try to stop worker " << options_.Name());
    needStop_ = true;
    if (epollThread_.joinable()) {
        if (afterFork) {
            epollThread_.detach();
        } else {
            epollThread_.join();
        }
    }

    if (epollFD_ != -1) {
        SafeCloseFd(epollFD_, !afterFork);
    }
}

Result AccTcpWorker::AddLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept
{
    ASSERT_RETURN(link.Get(), ACC_INVALID_PARAM);
    ASSERT_RETURN(link->fd_ != -1, ACC_INVALID_PARAM);

    struct epoll_event evNewFd {};
    evNewFd.data.ptr = link.Get();
    evNewFd.events = events;

    LOG_DEBUG("Adding link " << link->ShortName() << " into sock worker " << options_.Name());

    if (UNLIKELY(epoll_ctl(epollFD_, EPOLL_CTL_ADD, link->fd_, &evNewFd) != 0)) {
        LOG_ERROR("Failed to add link " << link->ShortName() << " into worker " << options_.Name() << ", errno "
                                        << errno);
        return ACC_EPOLL_ERROR;
    }

    link->IncreaseRef(); /* increase ref and remove ref when remove */
    return ACC_OK;
}

Result AccTcpWorker::RemoveLink(const AccTcpLinkDefaultPtr &link) noexcept
{
    ASSERT_RETURN(link.Get(), ACC_INVALID_PARAM);
    ASSERT_RETURN(link->fd_ != -1, ACC_INVALID_PARAM);

    LOG_DEBUG("Try to remove link " << link->ShortName() << " from sock worker " << options_.Name());

    if (UNLIKELY(epoll_ctl(epollFD_, EPOLL_CTL_DEL, link->fd_, nullptr) != 0)) {
        LOG_ERROR("Failed to remove " << link->ShortName() << " from sock worker " << options_.Name()
                                      << ", errno:" << errno);
        return ACC_EPOLL_ERROR;
    }

    link->DecreaseRef(); /* decrease ref as increased in add */
    return ACC_OK;
}

Result AccTcpWorker::ValidateOptions()
{
    ASSERT_RETURN(newRequestHandle_ != nullptr, ACC_INVALID_PARAM);
    ASSERT_RETURN(requestSentHandle_ != nullptr, ACC_INVALID_PARAM);
    ASSERT_RETURN(linkBrokenHandle_ != nullptr, ACC_INVALID_PARAM);

    if (options_.name_.empty()) {
        LOG_ERROR("Invalid options, name is empty");
        return ACC_INVALID_PARAM;
    }

    return ACC_OK;
}

void AccTcpWorker::SetPropertiesForThread()
{
    cpu_set_t cpuSet;
    if (options_.cpuId != -1) {
        CPU_ZERO(&(cpuSet));
        CPU_SET(options_.cpuId, &(cpuSet));
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &(cpuSet)) != 0) {
            LOG_WARN("Unable to bind worker " << options_.Name() << " to cpu " << options_.cpuId);
        }
    }

    /* set thread name */
    pthread_setname_np(pthread_self(), options_.Name().c_str());

    if (options_.threadPriority != 0) {
        if (setpriority(PRIO_PROCESS, 0, options_.threadPriority) != 0) {
            LOG_WARN("Unable to set thread priority of worker " << options_.Name() << ":" << errno);
        }
    }
}

void AccTcpWorker::RunInThread(std::atomic<bool> *started)
{
    SetPropertiesForThread();
    started->store(true);
    LOG_TRACE("Worker [" << options_.ToString() << "] progress thread started");

    const uint16_t pollBatchSize = 16L;
    const uint32_t timeout = options_.pollingTimeoutMs;
    const uint32_t budgetMs = PollBudgetMs();

    struct epoll_event ev[pollBatchSize];

    while (!needStop_) {
        /* 结算上一轮：超出轮询预算的部分即本 worker 不可观测时长。先累加、再前移本轮起点，
           读侧最坏会重复计入刚结算的一段，方向保守（只会少剔链路）。 */
        const uint64_t iterStartUs = mf::MonotonicTime::TimeUs();
        const uint64_t prevIterStartUs = iterStartUs_.load(std::memory_order_relaxed);
        if (prevIterStartUs != 0) {
            const uint64_t overrunMs = CalcOverrunMs(iterStartUs, prevIterStartUs, budgetMs);
            if (overrunMs > 0) {
                overrunBaseMs_.fetch_add(overrunMs, std::memory_order_relaxed);
            }
        }
        iterStartUs_.store(iterStartUs, std::memory_order_release);

        /* do epoll wait with timeout */
        int count = epoll_wait(epollFD_, ev, pollBatchSize, timeout);
        if (count > 0) {
            /* there are events, handle it */
            LOG_DEBUG("Got " << count << " in worker " << options_.Name());
            for (uint16_t i = 0; i < static_cast<uint16_t>(count); ++i) {
                struct epoll_event &oneEv = (ev)[i];
                ProcessEvent(oneEv);
            }
        } else if (count == 0) {
            LOG_DEBUG("Got " << count << " in worker " << options_.Name());
            continue;
        } else if (errno == EINTR) {
            LOG_DEBUG("Got error no EINTR in worker " << options_.Name());
            continue;
        } else {
            LOG_ERROR("Failed to do epoll_wait in worker " << options_.Name() << ", errno:" << errno);
            break;
        }
    }

    /* 线程退出后本 worker 已不再推进 iterStartUs_，必须复位为 0：否则 UnobservableMs() 会以
       "当前时刻 - 冻结的起点" 的墙钟 1:1 持续增长，使该 worker 上所有链路再也不可能被判心跳
       超时（该剔不剔），且无任何告警。复位后 UnobservableMs() 只返回已冻结的 overrunBaseMs_，
       扣除量有界，链路可正常判超时。此处与写侧一致采用 release，保证读侧看到 0 时也能看到
       此前已 fetch_add 进 overrunBaseMs_ 的全部超支。 */
    iterStartUs_.store(0, std::memory_order_release);

    LOG_DEBUG("Worker " << options_.Name() << " progress thread exiting");
}
Result AccTcpWorker::ModifyLink(const AccTcpLinkDefaultPtr &link, uint32_t events) noexcept
{
    ASSERT_RETURN(link.Get(), ACC_INVALID_PARAM);

    LOG_DEBUG("Try to modify link " << link->ShortName() << " in sock worker " << options_.Name() << " with event "
                                    << events);

    struct epoll_event evNewFd {};
    evNewFd.data.ptr = link.Get();
    evNewFd.events = events;

    if (UNLIKELY(epoll_ctl(epollFD_, EPOLL_CTL_MOD, link->fd_, &evNewFd) != 0)) {
        LOG_ERROR("Failed to modify " << link->ShortName() << " for sock worker " << options_.Name()
                                      << ", errno:" << errno);
        return ACC_EPOLL_ERROR;
    }

    return ACC_OK;
}

void AccTcpWorker::ProcessBufferedRequest(AccTcpLinkDefault *link) noexcept
{
    if (!link->HasBufferedRequest()) {
        return;
    }
    if (newRequestHandle_ == nullptr) {
        return;
    }
    auto r = link->HandlePollIn();
    if (r != ACC_LINK_MSG_READY) {
        return;
    }
    AccTcpRequestContext ctx(link->header_, link->data_, link);
    (void)newRequestHandle_(ctx);
    (void)ModifyLink(link, EPOLLIN | EPOLLOUT | EPOLLET);
}

Result AccTcpWorker::ProcessPollIn(AccTcpLinkDefault *link) noexcept
{
    auto result = link->HandlePollIn();
    if (result == ACC_LINK_MSG_READY) {
        AccTcpRequestContext ctx(link->header_, link->data_, link);
        if (newRequestHandle_ != nullptr) {
            (void)newRequestHandle_(ctx);
        }
        (void)ModifyLink(link, EPOLLIN | EPOLLOUT | EPOLLET);
        return ACC_OK;
    }
    if (result == ACC_LINK_EAGAIN) {
        (void)ModifyLink(link, EPOLLIN | EPOLLOUT | EPOLLET);
        return ACC_OK;
    }
    if (result == ACC_LINK_ERROR || result == ACC_LINK_MSG_INVALID) {
        LOG_DEBUG("RCV broken on link " << link->id_ << ", call linkBrokenHandle_");
        if (linkBrokenHandle_ != nullptr) {
            (void)linkBrokenHandle_(link);
        }
        return ACC_OK;
    }
    return ACC_OK;
}

Result AccTcpWorker::ProcessPollOut(AccTcpLinkDefault *link) noexcept
{
    AccMsgHeader outHeader{};
    AccDataBufferPtr cbCtx;
    auto result = link->HandlePollOut(outHeader, cbCtx);
    if (result == ACC_LINK_MSG_SENT) {
        if (requestSentHandle_ != nullptr) {
            (void)requestSentHandle_(MSG_SENT, outHeader, cbCtx);
        }
        (void)ModifyLink(link, EPOLLIN | EPOLLOUT | EPOLLET);
        if (link->HasPendingCleanup()) {
            if (linkBrokenHandle_ != nullptr) {
                (void)linkBrokenHandle_(link);
            }
            return ACC_OK;
        }
        ProcessBufferedRequest(link);
    } else if (result == ACC_LINK_EAGAIN) {
        (void)ModifyLink(link, EPOLLIN | EPOLLOUT | EPOLLET);
    } else if (result == ACC_LINK_ERROR) {
        (void)ModifyLink(link, EPOLLWRNORM);
    }
    return ACC_OK;
}

Result AccTcpWorker::ProcessPollWrNorm(AccTcpLinkDefault *link) noexcept
{
    if (linkBrokenHandle_ != nullptr) {
        (void)linkBrokenHandle_(link);
    } else {
        LOG_ERROR("LinkBrokenHandler not set in worker " << options_.Name());
    }
    return ACC_OK;
}

Result AccTcpWorker::ProcessEvent(struct epoll_event &event) noexcept
{
    auto *link = static_cast<AccTcpLinkDefault *>(event.data.ptr);
    if (UNLIKELY(link == nullptr)) {
        LOG_ERROR("Link is null in polled event for worker " << options_.Name());
        return ACC_EPOLL_ERROR;
    }

    if (event.events & EPOLLIN) {
        return ProcessPollIn(link);
    }
    if (event.events & EPOLLOUT) {
        return ProcessPollOut(link);
    }
    if (event.events & EPOLLWRNORM) {
        return ProcessPollWrNorm(link);
    }

    LOG_DEBUG("Receive link " << link->id_ << " event " << event.events);
    return ACC_OK;
}

} // namespace acc
} // namespace ock
