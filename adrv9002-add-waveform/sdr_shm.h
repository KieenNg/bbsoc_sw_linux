#ifndef SDR_SHM_H
#define SDR_SHM_H
#include <stdint.h>
#include <pthread.h>

#define SHM_NAME "/sdr_tx_shm"
#define FIFO_SIZE 1024 // Chứa tối đa 1024 bit MELP 

typedef struct {
    uint8_t bits[FIFO_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock; // Mutex để bảo vệ truy cập FIFO giữa các thread
} SharedFIFO_t;

#endif // SDR_SHM_H