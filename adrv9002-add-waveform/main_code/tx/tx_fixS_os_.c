/*
 * tx_fixs_linux.c — Phiên bản Linux của code baremetal Core0 TX FixS
 *
 * Flow:
 *   IIO init → FixS_TxInit(shm) → loop: FixS_Tx(iq_buffer) → IIO push → RF
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <iio.h>

#include "shared_region.h"
#include "Vhf_FixS_Tx.h"

#define TX_DATA_SIZE    160
#define IIO_DEVICE_NAME "axi-adrv9002-tx-lpc"

static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
    printf("\nStopping...\n");
}

static struct iio_device *find_tx_device(struct iio_context *ctx)
{
    int dev_count = iio_context_get_devices_count(ctx);
    for (int i = 0; i < dev_count; i++) {
        struct iio_device *dev = iio_context_get_device(ctx, i);
        const char *name = iio_device_get_name(dev);
        if (name && strcmp(name, IIO_DEVICE_NAME) == 0)
            return dev;
    }
    return NULL;
}

int main(void)
{
    struct iio_context *ctx;
    struct iio_device *tx_dev;
    struct iio_channel *ch_i, *ch_q;
    struct iio_buffer *iio_buf;
    struct shared_region *shm;
    int shm_fd;
    int frame_count = 0;

    /* I/Q buffer: interleaved [I0, Q0, I1, Q1, ...] — int16_t cho IIO */
    int16_t iq_buffer[2 * TX_DATA_SIZE];

    signal(SIGINT, signal_handler);

    printf("=== TX FixS Processor (Linux) ===\n\n");

    /* Bước 1: Mở IIO context */
    ctx = iio_create_local_context();
    if (!ctx) {
        fprintf(stderr, "Cannot create IIO context\n");
        return 1;
    }

    tx_dev = find_tx_device(ctx);
    if (!tx_dev) {
        fprintf(stderr, "Cannot find TX device: %s\n", IIO_DEVICE_NAME);
        iio_context_destroy(ctx);
        return 1;
    }
    printf("[OK] IIO TX device found: %s\n", IIO_DEVICE_NAME);

    /* Bước 2: Enable I/Q channels */
    ch_i = iio_device_find_channel(tx_dev, "voltage0", true);
    ch_q = iio_device_find_channel(tx_dev, "voltage1", true);
    if (!ch_i || !ch_q) {
        fprintf(stderr, "Cannot find I/Q channels\n");
        iio_context_destroy(ctx);
        return 1;
    }
    iio_channel_enable(ch_i);
    iio_channel_enable(ch_q);
    printf("[OK] I/Q channels enabled\n");

    /* Bước 3: Tạo IIO buffer */
    iio_buf = iio_device_create_buffer(tx_dev, TX_DATA_SIZE, false);
    if (!iio_buf) {
        fprintf(stderr, "Cannot create IIO buffer\n");
        iio_context_destroy(ctx);
        return 1;
    }
    printf("[OK] IIO TX buffer created (%d samples)\n", TX_DATA_SIZE);

    /* Bước 4: Mở shared memory */
    do {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd < 0) {
            printf("[..] Waiting for mic process to create shared memory...\n");
            sleep(1);
        }
    } while (shm_fd < 0 && running);
    if (!running) goto cleanup;

    ftruncate(shm_fd, sizeof(struct shared_region));

    shm = (struct shared_region *)mmap(NULL, sizeof(struct shared_region),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("[OK] Shared memory opened\n");

    /* Bước 5: TX attenuation */
    iio_channel_attr_write(ch_i, "hardwaregain", "-10.000");
    printf("[OK] TX attenuation configured\n");

    /* Bước 6: Khởi tạo FixS_Tx — truyền shm pointer vào
     *
     * Baremetal: FixS_TxInit() — không tham số
     * Linux:    FixS_TxInit(shm) — truyền shared memory pointer
     */
    if (FixS_TxInit(shm) < 0) {
        fprintf(stderr, "FixS_TxInit failed\n");
        goto cleanup;
    }

    /* Bước 7: Đồng bộ */
    shm->tx_ready = 1;
    printf("[..] Waiting for mic process (start_flag)...\n");
    while (running && !shm->start_flag) {
        usleep(1000);
    }
    if (!running) goto cleanup;
    printf("[OK] Sync done, starting TX\n\n");

    usleep(400000);

    /* Bước 8: Main loop
     *
     * Baremetal:
     *   FixS_Tx(tx_buffer_addr[ping])  → uint32_t packed I/Q
     *   DMA transfer → wait
     *
     * Linux:
     *   FixS_Tx(iq_buffer)  → int16_t interleaved I/Q
     *   IIO push (blocking)
     *
     * Không cần tự quản lý read_idx ở đây vì
     * FixS_Tx() bên trong tự đọc shm->data[read_idx]
     */
    while (running) {

        /* FixS_Tx tự đọc shared memory, modulate, ghi I/Q vào iq_buffer */
        FixS_Tx(iq_buffer);

        /* Push qua IIO → DMA → DAC → RF */
        void *buf_start = iio_buffer_start(iio_buf);
        memcpy(buf_start, iq_buffer, 2 * TX_DATA_SIZE * sizeof(int16_t));

        ssize_t nbytes = iio_buffer_push(iio_buf);
        if (nbytes < 0) {
            fprintf(stderr, "iio_buffer_push error: %zd\n", nbytes);
            continue;
        }

        frame_count++;
        if (frame_count % 100 == 0) {
            printf("  TX %d frames\n", frame_count);
        }
    }

cleanup:
    printf("\n[OK] Transmitted %d frames total\n", frame_count);

    FixS_TxCleanup();

    iio_channel_disable(ch_i);
    iio_channel_disable(ch_q);
    iio_buffer_destroy(iio_buf);
    iio_context_destroy(ctx);

    munmap(shm, sizeof(struct shared_region));
    close(shm_fd);

    printf("[OK] Cleanup done\n");
    return 0;
}