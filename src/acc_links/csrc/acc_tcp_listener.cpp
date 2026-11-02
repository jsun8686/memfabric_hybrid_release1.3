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
#include <net/if.h>
#include <sys/time.h>
#include <pthread.h>
#include "acc_common_util.h"
#include "mf_ipv4_validator.h"
#include "acc_tcp_listener.h"

namespace ock {
namespace acc {
#ifndef UT_ENABLED
constexpr int LISTEN_POLL_TIME = 500; // 500ms
#else
constexpr int LISTEN_POLL_TIME = 10; // 10ms
#endif

namespace {
// 依据 listener 自身配置的 ip:port 构造 URL，IPv6 需加方括号，保证解析出的地址就是本 listener 要 bind 的地址。
std::string BuildListenUrl(const std::string &ip, uint16_t port)
{
    if (ip.find(':') != std::string::npos && (ip.empty() || ip.front() != '[')) {
        return "tcp://[" + ip + "]:" + std::to_string(port);
    }
    return "tcp://" + ip + ":" + std::to_string(port);
}
} // namespace

Result AccTcpListener::Start() noexcept
{
    if (started_) {
        LOG_INFO("AccTcpListener at " << NameAndPort() << " already started");
        return ACC_OK;
    }

    VALIDATE_RETURN(connHandler_ != nullptr, "connection handler not initialized", ACC_ERROR);

    // 必须按本 listener 自己的 ip:port 解析地址：HA 主备场景下同一端口会先后出现不同 IP 的 parser，
    // 若按端口复用会 bind 到其它节点的 IP，报 "Cannot assign requested address"(EADDRNOTAVAIL)，
    // 本节点永远无法成为 leader。parser 管理器按完整 url（ip+port）缓存，不会出现该问题。
    auto &mgr = mf::SocketAddressParserMgr::getInstance();
    auto parser = mgr.CreateParser(BuildListenUrl(listenIp_, listenPort_));
    VALIDATE_RETURN(parser != nullptr, "parser not initialized", ACC_ERROR);

    /* create socket */
    auto tmpFD = ::socket(parser->GetAddressFamily(), SOCK_STREAM, 0);
    if (tmpFD < 0) {
        LOG_ERROR("Failed to create listen socket, error " << strerror(errno)
                                                           << ", please check if running of fd limit");
        return ACC_ERROR;
    }

    /* set option, bind and listen */
    if (reusePort_) {
        int flags = 1;
        if (::setsockopt(tmpFD, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<void *>(&flags), sizeof(flags)) < 0) {
            SafeCloseFd(tmpFD);
            LOG_ERROR("Failed to set reuse port of " << NameAndPort() << " as " << strerror(errno));
            return ACC_ERROR;
        }
    }

    if (::bind(tmpFD, parser->GetSockAddr(), parser->GetAddrLen()) < 0 || ::listen(tmpFD, 200L) < 0) {
        auto errorNum = errno;
        SafeCloseFd(tmpFD);
        if (errorNum == EADDRINUSE) {
            LOG_ERROR("address in use for bind listen on " << NameAndPort());
            return ACC_LINK_ADDRESS_IN_USE;
        }
        LOG_ERROR("Failed to bind or listen on " << NameAndPort() << " as errno " << strerror(errorNum));
        return ACC_ERROR;
    }

    pollTimeoutMs_ = LISTEN_POLL_TIME;
    auto ret = StartAcceptThread();
    if (ret != ACC_OK) {
        SafeCloseFd(tmpFD);
        return ret;
    }

    listenFd_ = tmpFD;

    int retry_times = 10000;
    while (!threadStarted_.load()) {
        usleep(100L);
        retry_times -= 1;
        if (retry_times == 0) {
            LOG_ERROR("Internal thread start timeout.");
            SafeCloseFd(listenFd_);
            return ACC_ERROR;
        }
    }

    started_ = true;
    return ACC_OK;
}

Result AccTcpListener::StartAcceptThread() noexcept
{
    threadStarted_.store(false);

    try {
        acceptThread_ = std::thread([this]() { this->RunInThread(); });
    } catch (const std::system_error &e) {
        LOG_ERROR("Failed to create accept thread: " << e.what());
        return ACC_ERROR;
    } catch (...) {
        LOG_ERROR("Unknown error creating accept thread");
        return ACC_ERROR;
    }

    return ACC_OK;
}

void AccTcpListener::Stop(bool afterFork) noexcept
{
    if (!started_) {
        return;
    }

    needStop_ = true;
    if (acceptThread_.joinable()) {
        if (afterFork) {
            acceptThread_.detach();
        } else {
            acceptThread_.join();
        }
    }

    SafeCloseFd(listenFd_, !afterFork);

    started_ = false;
}

void AccTcpListener::RunInThread() noexcept
{
    pthread_setname_np(pthread_self(), threadName_.c_str());
    LOG_TRACE("Acc listener accept thread for " << NameAndPort() << " start ...");
    threadStarted_.store(true);

    while (!needStop_) {
        try {
            struct pollfd pollEventFd = {};
            pollEventFd.fd = listenFd_;
            pollEventFd.events = POLLIN;
            pollEventFd.revents = 0;

            int rc = poll(&pollEventFd, 1, pollTimeoutMs_);
            if (rc < 0 && errno != EINTR) {
                LOG_ERROR("Get poll event failed  , errno " << strerror(errno));
                break;
            } else if (needStop_) {
                LOG_WARN("Acc listener accept thread get stop signal, will exit...");
                break;
            } else if (rc == 0) {
                continue;
            }

            struct sockaddr_in addressIn {};
            socklen_t len = sizeof(addressIn);
            auto fd = ::accept(listenFd_, reinterpret_cast<struct sockaddr *>(&addressIn), &len);
            if (fd < 0) {
                LOG_WARN("Unable to accept on new socket with " << strerror(errno) << ", ignore and continue");
                continue;
            }

            int flags = 1;
            setsockopt(fd, SOL_TCP, TCP_NODELAY, &flags, sizeof(flags));

            struct timeval timeout = {ACC_LINK_RECV_TIMEOUT, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            EnableTcpKeepalive(fd);

            ProcessNewConnection(fd, addressIn);
        } catch (std::exception &ex) {
            LOG_WARN("Got exception in AccTcpListener::RunInThread, exception " << ex.what()
                                                                                << ", ignore and continue");
        } catch (...) {
            LOG_WARN("Got unknown error in AccTcpListener::RunInThread, ignore and continue");
        }
    }

    LOG_TRACE("Working thread for AccTcpStore listener at " << NameAndPort() << " exiting");
}

void AccTcpListener::ProcessNewConnection(int fd, struct sockaddr_in addressIn) noexcept
{
    std::string ipPort = inet_ntoa(addressIn.sin_addr);
    ipPort += ":";
    ipPort += std::to_string(ntohs(addressIn.sin_port));

    /* receive header */
    AccConnReq req;
    auto received = ::recv(fd, &req, sizeof(req), 0);
    if (received != sizeof(req)) {
        LOG_ERROR("Failed to read header from the socket connected from " << ipPort);
        SafeCloseFd(fd);
        return;
    }

    SSL *ssl = nullptr;
    if (enableTls_) {
        auto ret = AccTcpSslHelper::NewSslLink(true, fd, sslCtx_, ssl);
        if (ret != ACC_OK) {
            LOG_ERROR("Failed to new connection ssl link");
            SafeCloseFd(fd);
            return;
        }
    }

    LOG_TRACE("Connected from " << ipPort << " successfully, ssl " << (enableTls_ ? "enable" : "disable"));
    AccTcpLinkDefaultPtr newLink;
    if (linkFactory_) {
        newLink = linkFactory_(fd, ipPort, ssl);
    } else {
        auto complexLink = AccMakeRef<AccTcpLinkComplexDefault>(fd, ipPort, AccTcpLinkDefault::NewId(), ssl);
        newLink = AccConvert<AccTcpLinkComplexDefault, AccTcpLinkDefault>(complexLink);
    }
    if (newLink == nullptr) {
        LOG_ERROR("Failed to create listener tcp link object, probably out of memory");
        if (ssl != nullptr) {
            if (AccCommonUtil::SslShutdownHelper(ssl) != ACC_OK) {
                LOG_ERROR("shut down ssl failed!");
            }
            OpenSslApiWrapper::SslFree(ssl);
            ssl = nullptr;
        }
        SafeCloseFd(fd);
        return;
    }

    // newLink作为智能指针 异常分支返回时会自动析构释放资源
    auto result = connHandler_(req, newLink.Get());
    if (result != ACC_OK) {
        LOG_WARN("ProcessNewConnection: connHandler_ non-ok, send error resp result=" << result);
        AccConnResp resp;
        resp.result = static_cast<int16_t>(result);
        (void)::send(fd, &resp, sizeof(resp), 0);
        return;
    }

    AccConnResp resp;
    resp.result = 0;
    auto sent = newLink->BlockSend(reinterpret_cast<void *>(&resp), sizeof(resp));
    if (sent != ACC_OK) {
        LOG_WARN("Unable to connect response to " << ipPort);
    }
}
} // namespace acc
} // namespace ock
