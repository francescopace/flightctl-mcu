/*
 * mbedTLS's network functions on Zephyr's sockets. The core reaches the
 * network only through mbedtls_net_* (connect, send, receive with a timeout);
 * Zephyr builds mbedTLS without its own implementation of them
 * (MBEDTLS_NET_C), which is written for a full POSIX system.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/net/socket.h>

#include "fc_port.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

/* What the socket said, which mbedTLS's error code does not carry. */
static int failed(const char *what, int code)
{
    fc_port_log(FC_LOG_WARN, "network: %s failed (errno %d)", what, errno);
    return code;
}

void mbedtls_net_init(mbedtls_net_context *ctx)
{
    ctx->fd = -1;
}

int mbedtls_net_connect(mbedtls_net_context *ctx, const char *host, const char *port, int proto)
{
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = proto == MBEDTLS_NET_PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM,
    };
    struct zsock_addrinfo *list = NULL;
    if (zsock_getaddrinfo(host, port, &hints, &list) != 0 || list == NULL) {
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    }
    int ret = MBEDTLS_ERR_NET_UNKNOWN_HOST;
    for (struct zsock_addrinfo *a = list; a != NULL; a = a->ai_next) {
        int fd = zsock_socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }
        if (zsock_connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            ctx->fd = fd;
            ret = 0;
            break;
        }
        zsock_close(fd);
        ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
    }
    zsock_freeaddrinfo(list);
    return ret;
}

int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;
    if (fd < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    ssize_t n = zsock_send(fd, buf, len, 0);
    if (n >= 0) {
        return (int)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return errno == EPIPE || errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET
                                                 : failed("send", MBEDTLS_ERR_NET_SEND_FAILED);
}

int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;
    if (fd < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    ssize_t n = zsock_recv(fd, buf, len, 0);
    if (n >= 0) {
        return (int)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return errno == EPIPE || errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET
                                                 : failed("receive", MBEDTLS_ERR_NET_RECV_FAILED);
}

/* `timeout` in milliseconds; 0 waits for ever. */
int mbedtls_net_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;
    if (fd < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    struct zsock_pollfd p = {.fd = fd, .events = ZSOCK_POLLIN};
    int rc = zsock_poll(&p, 1, timeout == 0 ? -1 : (int)timeout);
    if (rc == 0) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (rc < 0) {
        return failed("poll", MBEDTLS_ERR_NET_RECV_FAILED);
    }
    return mbedtls_net_recv(ctx, buf, len);
}

void mbedtls_net_free(mbedtls_net_context *ctx)
{
    if (ctx->fd >= 0) {
        zsock_close(ctx->fd);
    }
    ctx->fd = -1;
}
