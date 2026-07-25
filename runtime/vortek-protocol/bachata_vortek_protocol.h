/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Shared Bachata Vortek handshake definitions.
 * Client (Task 3) and Android server (Task 4) must agree on this layout.
 *
 * Wire order after AF_UNIX connect (when handshake is enabled):
 *   1. Client sends HEADER_SIZE (8) + BachataVortekHandshakeRequest
 *      requestCode = REQUEST_CODE_BACHATA_HANDSHAKE (3)
 *      requestLength = sizeof(BachataVortekHandshakeRequest)
 *   2. Server replies with HEADER_SIZE + BachataVortekHandshakeResponse
 *   3. Client continues with upstream REQUEST_CODE_CREATE_CONTEXT (1)
 *
 * Task 4 must handle request code 3 in VortekRendererComponent.handleRequest
 * (or equivalent) before createVkContext. Until then, set
 * BACHATA_VORTEK_HANDSHAKE=0 to talk to an unmodified upstream server.
 */
#ifndef BACHATA_VORTEK_PROTOCOL_H
#define BACHATA_VORTEK_PROTOCOL_H

#include <stdint.h>

#define BACHATA_VORTEK_MAGIC 0x4254564Bu /* 'BTVK' little-endian bytes */
#define BACHATA_VORTEK_PROTO_MAJOR 1
#define BACHATA_VORTEK_PROTO_MINOR 0
#define BACHATA_VORTEK_ENDIAN_LITTLE 1
#define BACHATA_VORTEK_ENDIAN_BIG 2

/* Upstream uses 1=CREATE_CONTEXT, 2=SEND_EXTRA_DATA, 100+=VK. Code 3 is free
 * (confirmed against request_codes.h at server e113da42 / client ab7329c4). */
#define REQUEST_CODE_BACHATA_HANDSHAKE 3
#define REQUEST_CODE_CREATE_CONTEXT_UPSTREAM 1
#define REQUEST_CODE_SEND_EXTRA_DATA_UPSTREAM 2
#define REQUEST_CODE_VK_CALL_START_UPSTREAM 100
#if REQUEST_CODE_BACHATA_HANDSHAKE == REQUEST_CODE_CREATE_CONTEXT_UPSTREAM || \
    REQUEST_CODE_BACHATA_HANDSHAKE == REQUEST_CODE_SEND_EXTRA_DATA_UPSTREAM || \
    REQUEST_CODE_BACHATA_HANDSHAKE >= REQUEST_CODE_VK_CALL_START_UPSTREAM
#error "REQUEST_CODE_BACHATA_HANDSHAKE collides with upstream Vortek request codes"
#endif

#define BACHATA_VORTEK_HANDSHAKE_OK 0
#define BACHATA_VORTEK_HANDSHAKE_UNSUPPORTED 1
#define BACHATA_VORTEK_HANDSHAKE_MISMATCH 2

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

/* Fence wait completion protocol (Task 9 false DEVICE_LOST fix).
 * Nonzero-timeout vkWaitForFences uses SCM_RIGHTS with this payload.
 * Do NOT treat numFds==0 as device loss. */
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

#endif /* BACHATA_VORTEK_PROTOCOL_H */
