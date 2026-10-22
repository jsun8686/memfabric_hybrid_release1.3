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
#ifndef MEMFABRIC_SMEM_ACC_API_H
#define MEMFABRIC_SMEM_ACC_API_H

#include "smem_acc_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an ACC TCP server.
 *
 * @return Server handle on success, or NULL on failure; release it with smem_acc_server_destroy
 */
smem_acc_server_t smem_acc_tcp_server_create(void);

/**
 * @brief Create an ACC HTTP server.
 *
 * @return Server handle on success, or NULL on failure; release it with smem_acc_server_destroy
 */
smem_acc_server_t smem_acc_http_server_create(void);

/**
 * @brief Destroy an ACC server and release its handle.
 *
 * Stop the server before destroying it. The handle must not be used after this call.
 *
 * @param server           [in] server handle, NULL is ignored
 */
void smem_acc_server_destroy(smem_acc_server_t server);

/**
 * @brief Start an ACC TCP server with the supplied options.
 *
 * @param server           [in] TCP server handle
 * @param options          [in] TCP server options, must not be NULL
 * @param tls              [in] TLS options, NULL disables TLS
 * @return 0 if successful, or an ACC error code on failure
 */
int32_t smem_acc_tcp_server_start(smem_acc_server_t server, const smem_acc_tcp_server_options_t *options,
                                  const smem_tls_options_t *tls);

/**
 * @brief Start an ACC HTTP server and enable its listener.
 *
 * @param server           [in] HTTP server handle
 * @param options          [in] HTTP server options, must not be NULL
 * @param tls              [in] TLS options, NULL disables TLS
 * @return 0 if successful, or an ACC error code on failure
 */
int32_t smem_acc_http_server_start(smem_acc_server_t server, const smem_acc_http_server_options_t *options,
                                   const smem_tls_options_t *tls);

/**
 * @brief Stop an ACC server without releasing its handle.
 *
 * @param server           [in] server handle, NULL is ignored
 */
void smem_acc_server_stop(smem_acc_server_t server);

/**
 * @brief Load the TLS libraries used by an ACC server.
 *
 * @param server           [in] server handle
 * @param path             [in] TLS library path, must not be NULL
 * @return 0 if successful, or an ACC error code on failure
 */
int32_t smem_acc_server_load_tls_library(smem_acc_server_t server, const char *path);

/**
 * @brief Register the TLS private key password decryption callback.
 *
 * @param server           [in] server handle
 * @param callback         [in] decryption callback, NULL clears the handler
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_decrypt_handler(smem_acc_server_t server, smem_decrypt_cb callback, void *context);

/**
 * @brief Register the callback for an incoming TCP connection.
 *
 * The request and link passed to the callback are borrowed and valid only during the callback.
 * Use smem_acc_link_retain to keep a link beyond the callback.
 *
 * @param server           [in] server handle
 * @param callback         [in] new link callback, NULL clears the handler
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_new_link_handler(smem_acc_server_t server, smem_acc_new_link_cb callback, void *context);

/**
 * @brief Register a TCP request callback for a message type.
 *
 * The message, data and link passed to the callback are borrowed and valid only during the callback.
 * Copy data or use smem_acc_link_retain if needed after the callback.
 *
 * @param server           [in] server handle
 * @param message_type     [in] message type to handle
 * @param callback         [in] request callback, NULL clears the handler for this type
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_request_handler(smem_acc_server_t server, int16_t message_type, smem_acc_request_cb callback,
                                         void *context);

/**
 * @brief Register a TCP send completion callback for a message type.
 *
 * The message passed to the callback is valid only during the callback.
 * The callback result indicates send completion, not the peer's application response.
 *
 * @param server           [in] server handle
 * @param message_type     [in] message type to handle
 * @param callback         [in] send completion callback, NULL clears the handler for this type
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_sent_handler(smem_acc_server_t server, int16_t message_type, smem_acc_sent_cb callback,
                                      void *context);

/**
 * @brief Register a TCP request callback with the extended message header, including the CRC field.
 *
 * This replaces the request handler for the same message type, including the non-v2 handler.
 * The message, data and link are borrowed and valid only during the callback.
 * Copy data or use smem_acc_link_retain if needed after the callback.
 *
 * @param server           [in] server handle
 * @param message_type     [in] message type to handle
 * @param callback         [in] extended request callback, NULL clears the handler for this type
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_request_handler_v2(smem_acc_server_t server, int16_t message_type,
                                            smem_acc_request_cb_v2 callback, void *context);

/**
 * @brief Register a TCP send completion callback with the extended header and per-send callback data.
 *
 * This replaces the send completion handler for the same type, including the non-v2 handler.
 * The message and callback data are borrowed and valid only during the callback.
 *
 * @param server           [in] server handle
 * @param message_type     [in] message type to handle
 * @param callback         [in] extended send completion callback, NULL clears the handler for this type
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_sent_handler_v2(smem_acc_server_t server, int16_t message_type, smem_acc_sent_cb_v2 callback,
                                         void *context);

/**
 * @brief Register the callback for a broken TCP link.
 *
 * The link passed to the callback is borrowed. Use smem_acc_link_retain to keep its handle
 * beyond the callback; retaining the handle does not reconnect the link.
 *
 * @param server           [in] server handle
 * @param callback         [in] link broken callback, NULL clears the handler
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 */
void smem_acc_server_set_link_broken_handler(smem_acc_server_t server, smem_acc_link_broken_cb callback, void *context);

/**
 * @brief Connect to a peer TCP server.
 *
 * @param server           [in] started server handle
 * @param peer_ip          [in] peer IP address, must not be NULL
 * @param port             [in] peer listening port
 * @param request          [in] connection handshake information, must not be NULL
 * @param max_retry_times  [in] maximum connection retry count
 * @param link             [out] link handle on success, must not be NULL; release with smem_acc_link_release
 * @return 0 if successful, or an ACC error code on failure; do not use the output link on failure
 */
int32_t smem_acc_server_connect(smem_acc_server_t server, const char *peer_ip, uint16_t port,
                                const smem_acc_connect_request_t *request, uint32_t max_retry_times,
                                smem_acc_link_t *link);

/**
 * @brief Release a TCP link of this server which the caller has decided is no longer usable, e.g. the
 *        peer's heartbeat timed out while the link still looks established.
 *
 * The link goes through the standard broken-link path: the registered link-broken handler is invoked
 * (so the caller can drop its own bookkeeping), the link is removed from the server side bookkeeping
 * and the fd is closed, so the peer notices the disconnection and reconnects.
 *
 * The link must be a link of this server, i.e. a link which the server currently manages; passing a
 * foreign link (another server instance, or a link already released) is rejected.
 *
 * @param server           [in] server handle
 * @param link             [in] link handle of a link currently managed by this server
 * @return 0 if the link was released by this call, ACC_INVALID_PARAM if server/link is invalid or the
 *         link is not a link currently managed by this server, or another ACC error code on failure
 */
int32_t smem_acc_server_release_link(smem_acc_server_t server, smem_acc_link_t link);

/**
 * @brief Register a handler for an HTTP method and path.
 *
 * The request passed to the callback is borrowed and must not be used after the callback returns.
 *
 * @param server           [in] HTTP server handle
 * @param method           [in] 0:GET, 1:HEAD, 2:POST, 3:PUT, 4:DELETE, 5:CONNECT, 6:OPTIONS, 7:TRACE, 8:PATCH
 * @param path             [in] request path, must not be NULL
 * @param callback         [in] HTTP request callback, must not be NULL
 * @param context          [in] user context passed to the callback; keep valid while callbacks can run
 * @return 0 if successful, or an ACC error code on failure
 */
int32_t smem_acc_http_server_register_handler(smem_acc_server_t server, uint8_t method, const char *path,
                                              smem_acc_http_cb callback, void *context);

/**
 * @brief Create an owned handle retaining the same TCP link.
 *
 * Use this API to keep a borrowed callback link beyond the callback. The returned handle is
 * independent of the input handle and must be released with smem_acc_link_release.
 *
 * @param link             [in] borrowed or owned link handle
 * @return New owned handle, or NULL if the input is NULL or allocation fails
 */
smem_acc_link_t smem_acc_link_retain(smem_acc_link_t link);

/**
 * @brief Release an owned TCP link handle.
 *
 * Do not release a borrowed callback handle. Each handle returned by smem_acc_link_retain or
 * smem_acc_server_connect must be released exactly once and must not be used afterwards.
 *
 * @param link             [in] owned link handle, NULL is ignored
 */
void smem_acc_link_release(smem_acc_link_t link);

/**
 * @brief Get the identifier of a TCP link.
 *
 * @param link             [in] link handle
 * @return Link identifier, or UINT32_MAX if the handle is NULL or contains no link
 */
uint32_t smem_acc_link_id(smem_acc_link_t link);

/**
 * @brief Set the application context value associated with a TCP link.
 *
 * @param link             [in] link handle, NULL or an empty handle is ignored
 * @param context          [in] application-defined value; ownership is not transferred
 */
void smem_acc_link_set_context(smem_acc_link_t link, uint64_t context);

/**
 * @brief Get the application context value associated with a TCP link.
 *
 * @param link             [in] link handle
 * @return Application context value, or 0 if the handle is NULL or contains no link
 */
uint64_t smem_acc_link_get_context(smem_acc_link_t link);

/**
 * @brief Queue a TCP message for asynchronous sending.
 *
 * The payload is copied before this call returns. Success means the message was queued,
 * not that it was received or processed by the peer.
 *
 * @param link             [in] link handle
 * @param message_type     [in] message type or operation code
 * @param result           [in] application result stored in the message header
 * @param sequence         [in] message sequence number
 * @param data             [in] payload, must not be NULL when data_len is nonzero
 * @param data_len         [in] payload length in bytes
 * @return 0 if queued successfully, or an ACC error code on failure
 */
int32_t smem_acc_link_send(smem_acc_link_t link, int16_t message_type, int16_t result, uint32_t sequence,
                           const void *data, uint32_t data_len);

/**
 * @brief Queue a TCP message with local data for the send completion callback.
 *
 * The payload and callback data are copied before this call returns. Callback data is not sent
 * to the peer; it is provided to the handler registered by smem_acc_server_set_sent_handler_v2.
 * Success means the message was queued, not that it was received or processed by the peer.
 *
 * @param link             [in] link handle
 * @param message_type     [in] message type or operation code
 * @param result           [in] application result stored in the message header
 * @param sequence         [in] message sequence number
 * @param data             [in] payload, must not be NULL when data_len is nonzero
 * @param data_len         [in] payload length in bytes
 * @param callback_data    [in] local callback data, must not be NULL when callback_data_len is nonzero
 * @param callback_data_len [in] local callback data length in bytes
 * @return 0 if queued successfully, or an ACC error code on failure
 */
int32_t smem_acc_link_send_v2(smem_acc_link_t link, int16_t message_type, int16_t result, uint32_t sequence,
                              const void *data, uint32_t data_len, const void *callback_data,
                              uint32_t callback_data_len);

/**
 * @brief Get the body of an HTTP request.
 *
 * The returned buffer is borrowed and valid only during the HTTP request callback.
 * Do not free or modify it.
 *
 * @param request          [in] HTTP request handle received by the callback
 * @param length           [out] body length in bytes, optional; set to 0 for a NULL request
 * @return Body pointer, or NULL if the request is NULL or has no body
 */
const void *smem_acc_http_request_body(smem_acc_http_request_t request, uint32_t *length);

/**
 * @brief Get a named parameter from an HTTP request.
 *
 * The returned string is borrowed and valid only during the HTTP request callback.
 * Do not free or modify it.
 *
 * @param request          [in] HTTP request handle received by the callback
 * @param name             [in] parameter name, must not be NULL
 * @return Null-terminated value, or NULL if the parameter is absent or an argument is NULL
 */
const char *smem_acc_http_request_param(smem_acc_http_request_t request, const char *name);

/**
 * @brief Visit all parameters of an HTTP request synchronously.
 *
 * The parameter names and values passed to the visitor are valid only during each visitor call.
 * Copy them if they are needed afterwards.
 *
 * @param request          [in] HTTP request handle received by the callback
 * @param callback         [in] parameter visitor, must not be NULL
 * @param context          [in] user context passed to the visitor
 * @return 0 if successful, or an ACC error code on failure
 */
int32_t smem_acc_http_request_visit_params(smem_acc_http_request_t request, smem_acc_http_param_cb callback,
                                           void *context);

/**
 * @brief Queue an HTTP response for the current request.
 *
 * Call this API while the HTTP request callback is running. The response body is copied.
 * Success does not guarantee that the peer has received the response.
 *
 * @param request          [in] HTTP request handle received by the callback
 * @param status           [in] HTTP response status code
 * @param content_type     [in] Content-Type header value, must not be NULL
 * @param body             [in] response body, must not be NULL when body_len is nonzero
 * @param body_len         [in] response body length in bytes
 * @return 0 if queued successfully, or an ACC error code on failure
 */
int32_t smem_acc_http_request_reply(smem_acc_http_request_t request, uint16_t status, const char *content_type,
                                    const void *body, uint32_t body_len);

#ifdef __cplusplus
}
#endif

#endif // MEMFABRIC_SMEM_ACC_API_H
