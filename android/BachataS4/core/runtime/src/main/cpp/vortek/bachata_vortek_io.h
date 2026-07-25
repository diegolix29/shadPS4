#ifndef BACHATA_VORTEK_IO_H
#define BACHATA_VORTEK_IO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bachata_vortek_sock_read(int fd, char* buffer, int size);
int bachata_vortek_sock_write(int fd, char* buffer, int size);
int bachata_vortek_send_fds(int sockFd, int* fds, int numFds);
int bachata_vortek_recv_fds(int sockFd, int* outFds, int* outNumFds);

int bachata_vortek_ashmem_create(const char* name, int64_t size);
void* bachata_vortek_ring_create(int shmFd, uint32_t bufferSize);
void bachata_vortek_ring_set_exit(void* ring);
void bachata_vortek_ring_free(void* ring);
uint32_t bachata_vortek_ring_shm_size(uint32_t bufferSize);
bool bachata_vortek_ring_has_exit(void* ring);

#ifdef __cplusplus
}
#endif

#endif
