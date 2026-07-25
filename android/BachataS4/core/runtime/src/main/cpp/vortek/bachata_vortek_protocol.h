/* Copy of runtime/vortek-protocol/bachata_vortek_protocol.h for NDK includes.
 * Keep byte-compatible with the glibc client.
 */
#ifndef BACHATA_VORTEK_PROTOCOL_H
#define BACHATA_VORTEK_PROTOCOL_H

#include <stdint.h>

#define BACHATA_VORTEK_MAGIC 0x4254564Bu /* 'BTVK' */
#define BACHATA_VORTEK_PROTO_MAJOR 1
#define BACHATA_VORTEK_PROTO_MINOR 0
#define BACHATA_VORTEK_ENDIAN_LITTLE 1
#define BACHATA_VORTEK_ENDIAN_BIG 2

/* Upstream: 1=CREATE_CONTEXT, 2=SEND_EXTRA_DATA, 100+=Vulkan. Code 3 is free. */
#define REQUEST_CODE_CREATE_CONTEXT 1
#define REQUEST_CODE_SEND_EXTRA_DATA 2
#define REQUEST_CODE_BACHATA_HANDSHAKE 3
#define REQUEST_CODE_VK_CALL_START 100

#if REQUEST_CODE_BACHATA_HANDSHAKE == REQUEST_CODE_CREATE_CONTEXT || \
    REQUEST_CODE_BACHATA_HANDSHAKE == REQUEST_CODE_SEND_EXTRA_DATA || \
    REQUEST_CODE_BACHATA_HANDSHAKE >= REQUEST_CODE_VK_CALL_START
#error "REQUEST_CODE_BACHATA_HANDSHAKE collides with upstream Vortek request codes"
#endif

#define BACHATA_VORTEK_HANDSHAKE_OK 0
#define BACHATA_VORTEK_HANDSHAKE_UNSUPPORTED 1
#define BACHATA_VORTEK_HANDSHAKE_MISMATCH 2

#define BACHATA_VORTEK_HEADER_SIZE 8
#define BACHATA_VORTEK_SERVER_RING_BYTES 4194304u
#define BACHATA_VORTEK_CLIENT_RING_BYTES 262144u

typedef struct BachataVortekHandshakeRequest {
    uint32_t magic;
    uint16_t proto_major;
    uint16_t proto_minor;
    uint16_t pointer_size;
    uint16_t endianness;
    uint32_t vulkan_header_version;
    char client_build_id[64];
} BachataVortekHandshakeRequest;

typedef struct BachataVortekHandshakeResponse {
    uint32_t magic;
    uint16_t proto_major;
    uint16_t proto_minor;
    uint16_t status;
    uint16_t reserved;
} BachataVortekHandshakeResponse;

/* Keep in sync with runtime/vortek-protocol/bachata_vortek_protocol.h */
#define BACHATA_VORTEK_FENCE_WAIT_REPLY_VERSION 1u
#define BACHATA_VORTEK_FENCE_WAIT_MAX_FENCES 64u

typedef enum BachataVortekFenceWaitReplyType {
    BACHATA_VORTEK_FENCE_WAIT_IMMEDIATE = 1,
    BACHATA_VORTEK_FENCE_WAIT_COMPLETION_FD = 2,
    BACHATA_VORTEK_FENCE_WAIT_PROTOCOL_ERROR = 3,
} BachataVortekFenceWaitReplyType;

typedef struct BachataVortekFenceWaitReply {
    uint32_t version;
    uint32_t type;
    int32_t vk_result;
    uint32_t fd_count;
} BachataVortekFenceWaitReply;

#if defined(__cplusplus)
static_assert(sizeof(BachataVortekHandshakeRequest) == 80, "handshake request size");
static_assert(sizeof(BachataVortekHandshakeResponse) == 12, "handshake response size");
#endif

#endif
