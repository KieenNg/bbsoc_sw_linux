/* shared_region.h */
#ifndef SHARED_REGION_H
#define SHARED_REGION_H

#include <stdint.h>

#define MELP_DATA_SIZE  54
#define BUFFER_DEPTH    128
/* ================================================================
 *
 * Baremetal: WRITE_IDX = *(volatile uint*)(0xFFFC0000 + 0)
 * Linux:    shm->write_idx
 * ================================================================ */
#define SHM_NAME        "/melp_ringbuf"

struct shared_region {
    volatile int write_idx;
    volatile int read_idx;
    volatile int start_flag;
    volatile int tx_ready;
    volatile int mic_ready;
    uint16_t     data[BUFFER_DEPTH][MELP_DATA_SIZE];
};

#endif