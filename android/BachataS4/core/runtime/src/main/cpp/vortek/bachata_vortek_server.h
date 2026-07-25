#ifndef BACHATA_VORTEK_SERVER_H
#define BACHATA_VORTEK_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native state codes (mirror Kotlin VortekServerState ordinals loosely). */
enum BachataVortekNativeState {
    BACHATA_VORTEK_STATE_STOPPED = 0,
    BACHATA_VORTEK_STATE_STARTING = 1,
    BACHATA_VORTEK_STATE_LOADER_READY = 2,
    BACHATA_VORTEK_STATE_SOCKET_READY = 3,
    BACHATA_VORTEK_STATE_CLIENT_CONNECTED = 4,
    BACHATA_VORTEK_STATE_CONTEXT_READY = 5,
    BACHATA_VORTEK_STATE_STOPPING = 6,
    BACHATA_VORTEK_STATE_FAILED = 7,
};

enum BachataVortekNativeError {
    BACHATA_VORTEK_OK = 0,
    BACHATA_VORTEK_ALREADY_RUNNING = 1,
    BACHATA_VORTEK_ERR_SOCKET_PATH = 2,
    BACHATA_VORTEK_ERR_SOCKET_TOO_LONG = 3,
    BACHATA_VORTEK_ERR_UNSAFE_EXISTING = 4,
    BACHATA_VORTEK_ERR_PARENT_MISSING = 5,
    BACHATA_VORTEK_ERR_BIND = 6,
    BACHATA_VORTEK_ERR_LOADER = 7,
    BACHATA_VORTEK_ERR_SYMBOL = 8,
    BACHATA_VORTEK_ERR_LISTEN = 9,
    BACHATA_VORTEK_ERR_INTERNAL = 10,
    BACHATA_VORTEK_ERR_NOT_RUNNING = 11,
    BACHATA_VORTEK_ERR_PROTOCOL = 12,
    BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED = 13,
    BACHATA_VORTEK_ERR_CONTEXT = 14,
};

/** Start listen thread. socket_path must be absolute, short, app-owned. */
int bachata_vortek_server_start(
    const char* socket_path,
    const char* expected_client_build,
    const char* server_build);

int bachata_vortek_server_stop(void);
int bachata_vortek_server_state(void);
int bachata_vortek_server_last_error(void);

/** Wait until state >= SOCKET_READY or FAILED. timeout_ms <=0 waits forever. */
int bachata_vortek_server_wait_socket_ready(int timeout_ms);

/** Wait until CONTEXT_READY or FAILED/STOPPED. */
int bachata_vortek_server_wait_context_ready(int timeout_ms);

/**
 * In-process protocol client self-test against a running server.
 * Exercises handshake + CREATE_CONTEXT + ring map + disconnect.
 * Returns BACHATA_VORTEK_OK on success.
 */
int bachata_vortek_protocol_self_test(const char* socket_path, const char* client_build);

/** Host Vulkan version string last seen (empty if unknown). */
const char* bachata_vortek_server_host_api_version(void);

#ifdef __cplusplus
}
#endif

#endif
