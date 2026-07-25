#include "bachata_vortek_io.h"

#include <unistd.h>
#include <sys/socket.h>

#include "ring_buffer.h"
#include "socket_utils.h"
#include "sysvshared_memory.h"

int bachata_vortek_sock_read(int fd, char* buffer, int size) {
    return sock_read(fd, buffer, size);
}

int bachata_vortek_sock_write(int fd, char* buffer, int size) {
    return sock_write(fd, buffer, size);
}

int bachata_vortek_send_fds(int sockFd, int* fds, int numFds) {
    return send_fds(sockFd, fds, numFds, NULL, 0);
}

int bachata_vortek_recv_fds(int sockFd, int* outFds, int* outNumFds) {
    return recv_fds(sockFd, outFds, outNumFds, NULL, 0);
}

int bachata_vortek_ashmem_create(const char* name, int64_t size) {
    return ashmemCreateRegion(name, size);
}

void* bachata_vortek_ring_create(int shmFd, uint32_t bufferSize) {
    RingBuffer* ring = RingBuffer_create(shmFd, bufferSize);
    if (!ring) return NULL;
    /* Upstream create omits sharedData; fix for free(). */
    if (ring->sharedData == NULL && ring->head != NULL) {
        ring->sharedData = (void*)ring->head;
    }
    return ring;
}

void bachata_vortek_ring_set_exit(void* ring) {
    if (ring) RingBuffer_setStatus((RingBuffer*)ring, RING_STATUS_EXIT);
}

void bachata_vortek_ring_free(void* ring) {
    if (!ring) return;
    RingBuffer* r = (RingBuffer*)ring;
    if (r->sharedData == NULL && r->head != NULL) {
        r->sharedData = (void*)r->head;
    }
    RingBuffer_free(r);
}

uint32_t bachata_vortek_ring_shm_size(uint32_t bufferSize) {
    return RingBuffer_getSHMemSize(bufferSize);
}

bool bachata_vortek_ring_has_exit(void* ring) {
    if (!ring) return true;
    return RingBuffer_hasStatus((RingBuffer*)ring, RING_STATUS_EXIT);
}
