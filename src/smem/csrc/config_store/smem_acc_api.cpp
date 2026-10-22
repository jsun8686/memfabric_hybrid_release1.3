/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "smem_acc_api.h"

#include <new>
#include <string>

#include "acc_http_server.h"
#include "acc_tcp_server.h"
#include "mf_ipv4_validator.h"
#include "smem_define.h"
#include "smem_logger.h"

namespace {
using namespace ock;

struct RuntimeServer {
    acc::AccTcpServerPtr tcp;
    acc::AccHttpServerPtr http;
};

using LinkPtr = acc::AccTcpLinkComplexPtr;

const char *SafeText(const char *value)
{
    return value == nullptr ? "<null>" : value;
}

RuntimeServer *AsServer(smem_acc_server_t value)
{
    return static_cast<RuntimeServer *>(value);
}

LinkPtr *AsLink(smem_acc_link_t value)
{
    return static_cast<LinkPtr *>(value);
}

acc::AccTlsOption MakeTls(const smem_tls_options_t *value)
{
    acc::AccTlsOption result;
    if (value == nullptr) {
        return result;
    }
    result.enableTls = value->enable_tls != 0;
    result.tlsTopPath = value->tls_top_path == nullptr ? "" : value->tls_top_path;
    result.tlsCert = value->tls_cert == nullptr ? "" : value->tls_cert;
    result.tlsCrlPath = value->tls_crl_path == nullptr ? "" : value->tls_crl_path;
    result.tlsCaPath = value->tls_ca_path == nullptr ? "" : value->tls_ca_path;
    if (value->tls_ca_file != nullptr && value->tls_ca_file[0] != '\0') {
        result.tlsCaFile.insert(value->tls_ca_file);
    }
    if (value->tls_crl_file != nullptr && value->tls_crl_file[0] != '\0') {
        result.tlsCrlFile.insert(value->tls_crl_file);
    }
    result.tlsPk = value->tls_private_key == nullptr ? "" : value->tls_private_key;
    result.tlsPkPwd = value->tls_private_key_password == nullptr ? "" : value->tls_private_key_password;
    return result;
}

smem_acc_message_t MakeMessage(const acc::AccMsgHeader &header)
{
    return {header.type, header.result, header.bodyLen, header.seqNo};
}

smem_acc_message_v2_t MakeMessageV2(const acc::AccMsgHeader &header)
{
    return {header.type, header.result, header.bodyLen, header.seqNo, header.crc};
}

smem_acc_server_t TcpServerCreate()
{
    auto server = acc::AccTcpServer::Create();
    if (server == nullptr) {
        SM_LOG_ERROR("Failed to create TCP server");
        return nullptr;
    }
    auto *runtime = new (std::nothrow) RuntimeServer;
    if (runtime != nullptr) {
        runtime->tcp = dynamic_cast<acc::AccTcpServer *>(server.Get());
    }
    return runtime;
}

smem_acc_server_t HttpServerCreate()
{
    auto server = acc::AccHttpServer::Create();
    if (server == nullptr) {
        SM_LOG_ERROR("Failed to create HTTP server");
        return nullptr;
    }
    auto *runtime = new (std::nothrow) RuntimeServer;
    if (runtime != nullptr) {
        runtime->http = server;
        runtime->tcp = dynamic_cast<acc::AccTcpServer *>(server.Get());
    }
    return runtime;
}

void ServerDestroy(smem_acc_server_t server)
{
    delete AsServer(server);
}

int32_t TcpServerStart(smem_acc_server_t server, const smem_acc_tcp_server_options_t *options,
                       const smem_tls_options_t *tls)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr || options == nullptr) {
        SM_LOG_ERROR("Invalid TCP server start parameters, server: "
                     << server << ", listenIp: " << SafeText(options == nullptr ? nullptr : options->listen_ip)
                     << ", listenPort: " << (options == nullptr ? 0 : options->listen_port));
        return acc::ACC_INVALID_PARAM;
    }
    acc::AccTcpServerOptions native;
    native.listenIp = options->listen_ip == nullptr ? "" : options->listen_ip;
    native.listenPort = options->listen_port;
    native.workerCount = options->worker_count;
    native.linkSendQueueSize = options->link_send_queue_size;
    native.magic = options->magic;
    native.version = options->version;
    native.enableListener = options->enable_listener != 0;
    return runtime->tcp->Start(native, MakeTls(tls));
}

int32_t HttpServerStart(smem_acc_server_t server, const smem_acc_http_server_options_t *options,
                        const smem_tls_options_t *tls)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->http == nullptr || options == nullptr) {
        SM_LOG_ERROR("Invalid HTTP server start parameters, server: "
                     << server << ", listenIp: " << SafeText(options == nullptr ? nullptr : options->listen_ip)
                     << ", listenPort: " << (options == nullptr ? 0 : options->listen_port));
        return acc::ACC_INVALID_PARAM;
    }
    acc::AccHttpServerOptions native;
    native.enableListener = true;
    native.listenIp = options->listen_ip == nullptr ? "" : options->listen_ip;
    native.listenPort = options->listen_port;
    native.workerCount = options->worker_count;
    native.maxBodySize = options->max_body_size;
    return runtime->http->Start(native, MakeTls(tls));
}

void ServerStop(smem_acc_server_t server)
{
    auto *runtime = AsServer(server);
    if (runtime != nullptr && runtime->tcp != nullptr) {
        runtime->tcp->Stop();
    }
}

int32_t ServerLoadTlsLibrary(smem_acc_server_t server, const char *path)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr || path == nullptr) {
        SM_LOG_ERROR("Invalid TLS library parameters, server: " << server << ", path: " << SafeText(path));
        return acc::ACC_INVALID_PARAM;
    }
    return runtime->tcp->LoadDynamicLib(path);
}

void ServerSetDecryptHandler(smem_acc_server_t server, smem_decrypt_cb callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterDecryptHandler(nullptr);
    } else {
        runtime->tcp->RegisterDecryptHandler(
            [callback, context](const char *cipher, size_t cipherLen, char *plain, size_t plainLen) {
                return callback(context, cipher, cipherLen, reinterpret_cast<uint8_t *>(plain), plainLen);
            });
    }
}

void ServerSetNewLinkHandler(smem_acc_server_t server, smem_acc_new_link_cb callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterNewLinkHandler(nullptr);
        return;
    }
    runtime->tcp->RegisterNewLinkHandler([callback, context](const acc::AccConnReq &req, const LinkPtr &link) {
        smem_acc_connect_request_t request{req.magic, req.version, static_cast<uint8_t>(req.reconnect), req.rankId};
        LinkPtr borrowed = link;
        return callback(context, &request, &borrowed);
    });
}

void ServerSetRequestHandler(smem_acc_server_t server, int16_t type, smem_acc_request_cb callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterNewRequestHandler(type, nullptr);
        return;
    }
    runtime->tcp->RegisterNewRequestHandler(type, [callback, context](const acc::AccTcpRequestContext &request) {
        auto message = MakeMessage(request.Header());
        LinkPtr borrowed = request.Link();
        return callback(context, &message, request.DataPtr(), request.DataLen(), &borrowed);
    });
}

void ServerSetSentHandler(smem_acc_server_t server, int16_t type, smem_acc_sent_cb callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterRequestSentHandler(type, nullptr);
        return;
    }
    runtime->tcp->RegisterRequestSentHandler(type, [callback, context](acc::AccMsgSentResult result,
                                                                       const acc::AccMsgHeader &header,
                                                                       const acc::AccDataBufferPtr &) {
        auto message = MakeMessage(header);
        return callback(context, static_cast<int32_t>(result), &message);
    });
}

void ServerSetRequestHandlerV2(smem_acc_server_t server, int16_t type, smem_acc_request_cb_v2 callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterNewRequestHandler(type, nullptr);
        return;
    }
    runtime->tcp->RegisterNewRequestHandler(type, [callback, context](const acc::AccTcpRequestContext &request) {
        auto message = MakeMessageV2(request.Header());
        LinkPtr borrowed = request.Link();
        return callback(context, &message, request.DataPtr(), request.DataLen(), &borrowed);
    });
}

void ServerSetSentHandlerV2(smem_acc_server_t server, int16_t type, smem_acc_sent_cb_v2 callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterRequestSentHandler(type, nullptr);
        return;
    }
    runtime->tcp->RegisterRequestSentHandler(type, [callback, context](acc::AccMsgSentResult result,
                                                                       const acc::AccMsgHeader &header,
                                                                       const acc::AccDataBufferPtr &callbackContext) {
        auto message = MakeMessageV2(header);
        return callback(context, static_cast<int32_t>(result), &message,
                        callbackContext == nullptr ? nullptr : callbackContext->DataPtrVoid(),
                        callbackContext == nullptr ? 0 : callbackContext->DataLen());
    });
}

void ServerSetLinkBrokenHandler(smem_acc_server_t server, smem_acc_link_broken_cb callback, void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr) {
        return;
    }
    if (callback == nullptr) {
        runtime->tcp->RegisterLinkBrokenHandler(nullptr);
        return;
    }
    runtime->tcp->RegisterLinkBrokenHandler([callback, context](const LinkPtr &link) {
        LinkPtr borrowed = link;
        return callback(context, &borrowed);
    });
}

int32_t ServerConnect(smem_acc_server_t server, const char *peerIp, uint16_t port,
                      const smem_acc_connect_request_t *request, uint32_t maxRetryTimes, smem_acc_link_t *link)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->tcp == nullptr || peerIp == nullptr || request == nullptr || link == nullptr) {
        SM_LOG_ERROR("Invalid server connect parameters, server: "
                     << server << ", peerIp: " << SafeText(peerIp) << ", port: " << port
                     << ", request is null: " << (request == nullptr) << ", link is null: " << (link == nullptr));
        return acc::ACC_INVALID_PARAM;
    }
    acc::AccConnReq native;
    native.magic = request->magic;
    native.version = request->version;
    native.reconnect = static_cast<int8_t>(request->reconnect);
    native.rankId = request->rank_id;
    // CreateParser also registers the port-to-parser mapping used by ConnectToPeerServer.
    auto parser =
        mf::SocketAddressParserMgr::getInstance().CreateParser(std::string(peerIp) + ":" + std::to_string(port));
    if (parser == nullptr) {
        SM_LOG_ERROR("Failed to create address parser, peerIp: " << peerIp << ", port: " << port);
        return acc::ACC_INVALID_PARAM;
    }
    LinkPtr result;
    int32_t ret = runtime->tcp->ConnectToPeerServer(peerIp, port, native, maxRetryTimes, result);
    if (ret == acc::ACC_OK) {
        *link = new (std::nothrow) LinkPtr(result);
        if (*link == nullptr) {
            SM_LOG_ERROR("Failed to retain connected link, peerIp: " << peerIp << ", port: " << port);
            return acc::ACC_NEW_OBJECT_FAIL;
        }
    }
    return ret;
}

int32_t HttpServerRegisterHandler(smem_acc_server_t server, uint8_t method, const char *path, smem_acc_http_cb callback,
                                  void *context)
{
    auto *runtime = AsServer(server);
    if (runtime == nullptr || runtime->http == nullptr || path == nullptr || callback == nullptr ||
        method > static_cast<uint8_t>(acc::AccHttpMethod::PATCH)) {
        SM_LOG_ERROR("Invalid HTTP handler parameters, server: "
                     << server << ", method: " << static_cast<uint32_t>(method) << ", path: " << SafeText(path)
                     << ", callback is null: " << (callback == nullptr));
        return acc::ACC_INVALID_PARAM;
    }
    runtime->http->RegisterHttpHandler(
        static_cast<acc::AccHttpMethod>(method), path,
        [callback, context](acc::AccHttpRequestContext &request) { return callback(context, &request); });
    return acc::ACC_OK;
}

smem_acc_link_t LinkRetain(smem_acc_link_t link)
{
    auto *native = AsLink(link);
    return native == nullptr ? nullptr : new (std::nothrow) LinkPtr(*native);
}

void LinkRelease(smem_acc_link_t link)
{
    delete AsLink(link);
}

uint32_t LinkId(smem_acc_link_t link)
{
    auto *native = AsLink(link);
    return native == nullptr || *native == nullptr ? UINT32_MAX : (*native)->Id();
}

void LinkSetContext(smem_acc_link_t link, uint64_t context)
{
    auto *native = AsLink(link);
    if (native != nullptr && *native != nullptr) {
        (*native)->UpCtx(context);
    }
}

uint64_t LinkGetContext(smem_acc_link_t link)
{
    auto *native = AsLink(link);
    return native == nullptr || *native == nullptr ? 0 : (*native)->UpCtx();
}

int32_t ServerReleaseLink(smem_acc_server_t server, smem_acc_link_t link)
{
    auto *runtime = AsServer(server);
    auto *native = AsLink(link);
    const bool serverReady = runtime != nullptr && runtime->tcp != nullptr;
    const bool linkReady = native != nullptr && *native != nullptr;
    if (!serverReady || !linkReady) {
        SM_LOG_ERROR("Invalid release link parameters, server is null: " << (!serverReady)
                                                                         << ", link is null: " << (!linkReady));
        return acc::ACC_INVALID_PARAM;
    }
    return runtime->tcp->ReleaseLink(*native);
}

int32_t LinkSend(smem_acc_link_t link, int16_t type, int16_t result, uint32_t sequence, const void *data,
                 uint32_t dataLen)
{
    auto *native = AsLink(link);
    if (native == nullptr || *native == nullptr || (dataLen != 0 && data == nullptr)) {
        SM_LOG_ERROR("Invalid link send parameters, link: " << link << ", messageType: " << type << ", dataLen: "
                                                            << dataLen << ", data is null: " << (data == nullptr));
        return acc::ACC_INVALID_PARAM;
    }
    auto buffer = acc::AccDataBuffer::Create(data, dataLen);
    if (buffer == nullptr) {
        SM_LOG_ERROR("Failed to create send buffer, messageType: " << type << ", dataLen: " << dataLen);
        return acc::ACC_NEW_OBJECT_FAIL;
    }
    return (*native)->NonBlockSend(type, result, sequence, buffer, nullptr);
}

int32_t LinkSendV2(smem_acc_link_t link, int16_t type, int16_t result, uint32_t sequence, const void *data,
                   uint32_t dataLen, const void *callbackData, uint32_t callbackDataLen)
{
    auto *native = AsLink(link);
    if (native == nullptr || *native == nullptr || (dataLen != 0 && data == nullptr) ||
        (callbackDataLen != 0 && callbackData == nullptr)) {
        SM_LOG_ERROR("Invalid link send v2 parameters, link: " << link << ", messageType: " << type << ", dataLen: "
                                                               << dataLen << ", callbackDataLen: " << callbackDataLen);
        return acc::ACC_INVALID_PARAM;
    }
    auto buffer = acc::AccDataBuffer::Create(data, dataLen);
    if (buffer == nullptr) {
        SM_LOG_ERROR("Failed to create send buffer, messageType: " << type << ", dataLen: " << dataLen);
        return acc::ACC_NEW_OBJECT_FAIL;
    }
    acc::AccDataBufferPtr callbackContext = nullptr;
    if (callbackData != nullptr) {
        callbackContext = acc::AccDataBuffer::Create(callbackData, callbackDataLen);
        if (callbackContext == nullptr) {
            SM_LOG_ERROR("Failed to create callback buffer, msgType: " << type << ", cbDataLen: " << callbackDataLen);
            return acc::ACC_NEW_OBJECT_FAIL;
        }
    }
    return (*native)->NonBlockSend(type, result, sequence, buffer, callbackContext);
}

const void *HttpRequestBody(smem_acc_http_request_t request, uint32_t *length)
{
    auto *native = static_cast<acc::AccHttpRequestContext *>(request);
    if (length != nullptr) {
        *length = native == nullptr ? 0 : native->BodyLen();
    }
    return native == nullptr ? nullptr : native->BodyPtr();
}

const char *HttpRequestParam(smem_acc_http_request_t request, const char *name)
{
    auto *native = static_cast<acc::AccHttpRequestContext *>(request);
    if (native == nullptr || name == nullptr) {
        return nullptr;
    }
    const auto &params = native->Params();
    auto it = params.find(name);
    return it == params.end() ? nullptr : it->second.c_str();
}

int32_t HttpRequestVisitParams(smem_acc_http_request_t request, smem_acc_http_param_cb callback, void *context)
{
    auto *native = static_cast<acc::AccHttpRequestContext *>(request);
    if (native == nullptr || callback == nullptr) {
        SM_LOG_ERROR("Invalid HTTP parameter visitor, request: " << request
                                                                 << ", callback is null: " << (callback == nullptr));
        return acc::ACC_INVALID_PARAM;
    }
    const auto params = native->Params();
    for (const auto &[name, value] : params) {
        callback(context, name.c_str(), value.c_str());
    }
    return acc::ACC_OK;
}

int32_t HttpRequestReply(smem_acc_http_request_t request, uint16_t status, const char *contentType, const void *body,
                         uint32_t bodyLen)
{
    auto *native = static_cast<acc::AccHttpRequestContext *>(request);
    if (native == nullptr || contentType == nullptr || (bodyLen != 0 && body == nullptr)) {
        SM_LOG_ERROR("Invalid HTTP reply parameters, request: "
                     << request << ", status: " << status << ", contentType: " << SafeText(contentType)
                     << ", bodyLen: " << bodyLen << ", body is null: " << (body == nullptr));
        return acc::ACC_INVALID_PARAM;
    }
    std::string payload;
    if (bodyLen != 0) {
        payload.assign(static_cast<const char *>(body), bodyLen);
    }
    return native->Reply(static_cast<acc::AccHttpStatusCode>(status), contentType, payload);
}

} // namespace

extern "C" {
SMEM_API smem_acc_server_t smem_acc_tcp_server_create(void)
{
    return TcpServerCreate();
}

SMEM_API smem_acc_server_t smem_acc_http_server_create(void)
{
    return HttpServerCreate();
}

SMEM_API void smem_acc_server_destroy(smem_acc_server_t server)
{
    ServerDestroy(server);
}

SMEM_API int32_t smem_acc_tcp_server_start(smem_acc_server_t server, const smem_acc_tcp_server_options_t *options,
                                           const smem_tls_options_t *tls)
{
    return TcpServerStart(server, options, tls);
}

SMEM_API int32_t smem_acc_http_server_start(smem_acc_server_t server, const smem_acc_http_server_options_t *options,
                                            const smem_tls_options_t *tls)
{
    return HttpServerStart(server, options, tls);
}

SMEM_API void smem_acc_server_stop(smem_acc_server_t server)
{
    ServerStop(server);
}

SMEM_API int32_t smem_acc_server_load_tls_library(smem_acc_server_t server, const char *path)
{
    return ServerLoadTlsLibrary(server, path);
}

SMEM_API void smem_acc_server_set_decrypt_handler(smem_acc_server_t server, smem_decrypt_cb callback, void *context)
{
    ServerSetDecryptHandler(server, callback, context);
}

SMEM_API void smem_acc_server_set_new_link_handler(smem_acc_server_t server, smem_acc_new_link_cb callback,
                                                   void *context)
{
    ServerSetNewLinkHandler(server, callback, context);
}

SMEM_API void smem_acc_server_set_request_handler(smem_acc_server_t server, int16_t messageType,
                                                  smem_acc_request_cb callback, void *context)
{
    ServerSetRequestHandler(server, messageType, callback, context);
}

SMEM_API void smem_acc_server_set_sent_handler(smem_acc_server_t server, int16_t messageType, smem_acc_sent_cb callback,
                                               void *context)
{
    ServerSetSentHandler(server, messageType, callback, context);
}

SMEM_API void smem_acc_server_set_request_handler_v2(smem_acc_server_t server, int16_t messageType,
                                                     smem_acc_request_cb_v2 callback, void *context)
{
    ServerSetRequestHandlerV2(server, messageType, callback, context);
}

SMEM_API void smem_acc_server_set_sent_handler_v2(smem_acc_server_t server, int16_t messageType,
                                                  smem_acc_sent_cb_v2 callback, void *context)
{
    ServerSetSentHandlerV2(server, messageType, callback, context);
}

SMEM_API void smem_acc_server_set_link_broken_handler(smem_acc_server_t server, smem_acc_link_broken_cb callback,
                                                      void *context)
{
    ServerSetLinkBrokenHandler(server, callback, context);
}

SMEM_API int32_t smem_acc_server_connect(smem_acc_server_t server, const char *peerIp, uint16_t port,
                                         const smem_acc_connect_request_t *request, uint32_t maxRetryTimes,
                                         smem_acc_link_t *link)
{
    return ServerConnect(server, peerIp, port, request, maxRetryTimes, link);
}

SMEM_API int32_t smem_acc_http_server_register_handler(smem_acc_server_t server, uint8_t method, const char *path,
                                                       smem_acc_http_cb callback, void *context)
{
    return HttpServerRegisterHandler(server, method, path, callback, context);
}

SMEM_API int32_t smem_acc_server_release_link(smem_acc_server_t server, smem_acc_link_t link)
{
    return ServerReleaseLink(server, link);
}

SMEM_API smem_acc_link_t smem_acc_link_retain(smem_acc_link_t link)
{
    return LinkRetain(link);
}

SMEM_API void smem_acc_link_release(smem_acc_link_t link)
{
    LinkRelease(link);
}

SMEM_API uint32_t smem_acc_link_id(smem_acc_link_t link)
{
    return LinkId(link);
}

SMEM_API void smem_acc_link_set_context(smem_acc_link_t link, uint64_t context)
{
    LinkSetContext(link, context);
}

SMEM_API uint64_t smem_acc_link_get_context(smem_acc_link_t link)
{
    return LinkGetContext(link);
}

SMEM_API int32_t smem_acc_link_send(smem_acc_link_t link, int16_t messageType, int16_t result, uint32_t sequence,
                                    const void *data, uint32_t dataLen)
{
    return LinkSend(link, messageType, result, sequence, data, dataLen);
}

SMEM_API int32_t smem_acc_link_send_v2(smem_acc_link_t link, int16_t messageType, int16_t result, uint32_t sequence,
                                       const void *data, uint32_t dataLen, const void *callbackData,
                                       uint32_t callbackDataLen)
{
    return LinkSendV2(link, messageType, result, sequence, data, dataLen, callbackData, callbackDataLen);
}

SMEM_API const void *smem_acc_http_request_body(smem_acc_http_request_t request, uint32_t *length)
{
    return HttpRequestBody(request, length);
}

SMEM_API const char *smem_acc_http_request_param(smem_acc_http_request_t request, const char *name)
{
    return HttpRequestParam(request, name);
}

SMEM_API int32_t smem_acc_http_request_visit_params(smem_acc_http_request_t request, smem_acc_http_param_cb callback,
                                                    void *context)
{
    return HttpRequestVisitParams(request, callback, context);
}

SMEM_API int32_t smem_acc_http_request_reply(smem_acc_http_request_t request, uint16_t status, const char *contentType,
                                             const void *body, uint32_t bodyLen)
{
    return HttpRequestReply(request, status, contentType, body, bodyLen);
}
}
