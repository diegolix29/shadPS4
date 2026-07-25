/* Bachata overlay for Vortek client main.c (based on brunodev85/vortek).
 * Adds configurable socket path, timeouts, logging, and optional handshake.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "vortek.h"
#include "bachata_vortek_protocol.h"

#ifndef BACHATA_VORTEK_CLIENT_BUILD_ID
#define BACHATA_VORTEK_CLIENT_BUILD_ID "unknown"
#endif

#ifndef BACHATA_VORTEK_DEFAULT_SOCKET
#define BACHATA_VORTEK_DEFAULT_SOCKET "/tmp/bachata-vortek.sock"
#endif

/* Linux sockaddr_un.sun_path is typically 108 bytes. */
#define BACHATA_VORTEK_MAX_SOCKET_PATH (sizeof(((struct sockaddr_un*)0)->sun_path) - 1)
#define BACHATA_VORTEK_CONNECT_TIMEOUT_SEC 2
#define BACHATA_VORTEK_IO_TIMEOUT_SEC 2

int serverFd = -1;
uint16_t maxClientRequestId = 1;
MemoryPool globalMemoryPool = {0};
RingBuffer* serverRing = NULL;
RingBuffer* clientRing = NULL;

static int g_log_level = 1; /* 0=quiet 1=info 2=trace */
static char g_socket_path[BACHATA_VORTEK_MAX_SOCKET_PATH + 1];
static int g_handshake_enabled = 1;

static void bachata_log(int level, const char* fmt, ...) {
    if (level > g_log_level) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[Bachata.Vortek] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void bachata_configure_from_env(void) {
    const char* level = getenv("BACHATA_VORTEK_LOG_LEVEL");
    if (level && level[0]) {
        g_log_level = atoi(level);
        if (g_log_level < 0) g_log_level = 0;
        if (g_log_level > 2) g_log_level = 2;
    }
    if (getenv("BACHATA_VORTEK_TRACE") && getenv("BACHATA_VORTEK_TRACE")[0] == '1') {
        g_log_level = 2;
    }

    const char* handshake = getenv("BACHATA_VORTEK_HANDSHAKE");
    if (handshake && handshake[0] == '0') g_handshake_enabled = 0;

    const char* socket_path = getenv("BACHATA_VORTEK_SOCKET");
    if (!socket_path || !socket_path[0]) {
        socket_path = BACHATA_VORTEK_DEFAULT_SOCKET;
    }

    size_t len = strlen(socket_path);
    if (len == 0 || len > BACHATA_VORTEK_MAX_SOCKET_PATH) {
        bachata_log(1, "state=invalid_socket reason=length len=%zu max=%zu", len, (size_t)BACHATA_VORTEK_MAX_SOCKET_PATH);
        g_socket_path[0] = '\0';
        return;
    }
    if (strstr(socket_path, "com.winlator") != NULL) {
        bachata_log(1, "state=invalid_socket reason=winlator_path");
        g_socket_path[0] = '\0';
        return;
    }
    if (socket_path[0] != '/' && socket_path[0] != '\0') {
        /* allow abstract sockets only if explicitly leading @ — otherwise require absolute */
        if (socket_path[0] != '@') {
            bachata_log(1, "state=invalid_socket reason=not_absolute");
            g_socket_path[0] = '\0';
            return;
        }
    }

    memcpy(g_socket_path, socket_path, len + 1);
}

static int set_socket_timeouts(int fd) {
    struct timeval tv;
    tv.tv_sec = BACHATA_VORTEK_IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

static int vortekServerConnect(void) {
    if (g_socket_path[0] == '\0') return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (set_socket_timeouts(fd) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, g_socket_path, sizeof(server_addr.sun_path) - 1);

    bachata_log(1, "socket=%s", g_socket_path);
    bachata_log(1, "client_build=%s", BACHATA_VORTEK_CLIENT_BUILD_ID);
    bachata_log(1, "state=connecting");

    int res;
    do {
        res = 0;
        if (connect(fd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_un)) < 0) res = -errno;
    } while (res == -EINTR);

    if (res < 0) {
        bachata_log(1, "state=server_unavailable socket=%s errno=%d", g_socket_path, -res);
        close(fd);
        return -1;
    }

    bachata_log(2, "state=connected fd=%d", fd);
    return fd;
}

static bool performHandshake(void) {
    if (!g_handshake_enabled) {
        bachata_log(2, "state=handshake_skipped");
        return true;
    }

    BachataVortekHandshakeRequest request;
    memset(&request, 0, sizeof(request));
    request.magic = BACHATA_VORTEK_MAGIC;
    request.proto_major = BACHATA_VORTEK_PROTO_MAJOR;
    request.proto_minor = BACHATA_VORTEK_PROTO_MINOR;
    request.pointer_size = (uint16_t)sizeof(void*);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    request.endianness = BACHATA_VORTEK_ENDIAN_LITTLE;
#else
    request.endianness = BACHATA_VORTEK_ENDIAN_BIG;
#endif
#ifdef VK_HEADER_VERSION
    request.vulkan_header_version = VK_HEADER_VERSION;
#else
    request.vulkan_header_version = 0;
#endif
    strncpy(request.client_build_id, BACHATA_VORTEK_CLIENT_BUILD_ID, sizeof(request.client_build_id) - 1);

    char header[HEADER_SIZE];
    *(int*)(header + 0) = REQUEST_CODE_BACHATA_HANDSHAKE;
    *(int*)(header + 4) = (int)sizeof(request);

    if (sock_write(serverFd, header, HEADER_SIZE) != HEADER_SIZE) {
        bachata_log(1, "state=handshake_failed reason=write_header");
        return false;
    }
    if (sock_write(serverFd, (char*)&request, (int)sizeof(request)) != (int)sizeof(request)) {
        bachata_log(1, "state=handshake_failed reason=write_body");
        return false;
    }

    char response_header[HEADER_SIZE];
    if (sock_read(serverFd, response_header, HEADER_SIZE) != HEADER_SIZE) {
        bachata_log(1, "state=handshake_failed reason=read_header");
        return false;
    }
    int response_code = *(int*)(response_header + 0);
    int response_size = *(int*)(response_header + 4);
    if (response_code != REQUEST_CODE_BACHATA_HANDSHAKE || response_size != (int)sizeof(BachataVortekHandshakeResponse)) {
        bachata_log(1, "state=handshake_failed reason=bad_response code=%d size=%d", response_code, response_size);
        return false;
    }

    BachataVortekHandshakeResponse response;
    memset(&response, 0, sizeof(response));
    if (sock_read(serverFd, (char*)&response, (int)sizeof(response)) != (int)sizeof(response)) {
        bachata_log(1, "state=handshake_failed reason=read_body");
        return false;
    }
    if (response.magic != BACHATA_VORTEK_MAGIC || response.status != BACHATA_VORTEK_HANDSHAKE_OK) {
        bachata_log(1, "state=handshake_failed magic=0x%x status=%u", response.magic, response.status);
        return false;
    }

    bachata_log(1, "state=handshake_ok protocol=%u.%u", response.proto_major, response.proto_minor);
    return true;
}

static bool createVkContext(void) {
    char header[HEADER_SIZE];
    *(int*)(header + 0) = REQUEST_CODE_CREATE_CONTEXT;
    *(int*)(header + 4) = 0;

    int res = write(serverFd, header, HEADER_SIZE);
    if (res < 0) return false;

    int shmFds[2];
    int numFds = 0;
    recv_fds(serverFd, shmFds, &numFds, NULL, 0);
    if (numFds != 2) return false;

    serverRing = RingBuffer_create(shmFds[0], SERVER_RING_BUFFER_SIZE);
    if (!serverRing) return false;

    clientRing = RingBuffer_create(shmFds[1], CLIENT_RING_BUFFER_SIZE);
    if (!clientRing) return false;

    close(shmFds[0]);
    close(shmFds[1]);

    if (!globalMemoryPool.data) {
        globalMemoryPool.data = malloc(MEMORY_POOL_MAX_SIZE);
        memset(globalMemoryPool.data, 0, MEMORY_POOL_MAX_SIZE);
    }
    return true;
}

static void terminationCallback(void) {
    CLOSEFD(serverFd);
    if (serverRing) RingBuffer_free(serverRing);
    if (clientRing) RingBuffer_free(clientRing);

    vt_free(&globalMemoryPool);
    MEMFREE(globalMemoryPool.data);
}

bool vortekInitOnce(void) {
    if (serverFd == -1) {
        bachata_configure_from_env();
        serverFd = vortekServerConnect();

        if (serverFd > 0) {
            if (!performHandshake()) {
                CLOSEFD(serverFd);
                return false;
            }
            if (!createVkContext()) {
                bachata_log(1, "state=create_context_failed");
                CLOSEFD(serverFd);
                return false;
            }
            bachata_log(1, "state=context_ready");
        }

        atexit(terminationCallback);
    }

    return serverFd > 0;
}
