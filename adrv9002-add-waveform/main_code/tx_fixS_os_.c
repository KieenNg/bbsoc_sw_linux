/*
 * tx_fixs_linux.c — Phiên bản Linux của code baremetal Core0 TX FixS
 *
 * Baremetal:
 *   ADRV9002 init → FixS_Tx(buffer) → DMA → DAC → RF
 *
 * Linux:
 *   ADRV9002 (IIO driver đã init sẵn) → FixS_Tx(buffer) → IIO buffer push → RF
 *
 * Khác biệt:
 *   - IIO (libiio) thay thế toàn bộ: adrv9002_setup, axi_dac, axi_dmac
 *   - POSIX shared memory thay thế 0xFFFC0000
 *   - Không cần Xil_Out32, cache flush, DMA config thủ công
 *   - ADRV9002 driver trong kernel đã handle: profile load, SSI, AGC, calibration
 *
 * Cách build:
 *   gcc -o tx_fixs tx_fixs_linux.c Vhf_FixS_Tx.c -liio -lrt -lm -lpthread -Wall
 *   ./tx_fixs
 *
 * Cần link thêm thư viện FixS_Tx và MELP decode của bạn khi build.
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

/* ================================================================
 * Tham số giữ nguyên từ baremetal
 * ================================================================ */
#define TX_DATA_SIZE    160      /* samples per frame */

#define IIO_DEVICE_NAME "axi-adrv9002-tx-lpc"

/* ================================================================
 * Global
 * ================================================================ */
static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
    printf("\nStopping...\n");
}

/* ================================================================
 * FixS_Tx placeholder — thay bằng thư viện của bạn
 *
 * Baremetal:
 *   FixS_Tx(tx_buffer_addr[idx])
 *   → đọc shared_memory[READ_IDX], modulate, ghi I/Q vào buffer
 *
 * Linux: giống hệt, chỉ khác buffer là int16_t[] thay vì DDR addr
 *
 * input:  melp_data[MELP_DATA_SIZE]  (uint16_t, từ shared memory)
 * output: iq_buffer[2 * TX_DATA_SIZE] (int16_t, interleaved I/Q)
 * ================================================================ */
static void FixS_Tx_process(uint16_t *melp_data, int16_t *iq_buffer)
{
    /*
     * TODO: Thay bằng code thật của bạn:
     *
     *   #include "Vhf_FixS_Tx.h"
     *
     *   // Decode MELP
     *   // Modulate
     *   // Ghi I/Q samples vào iq_buffer
     *
     * Baremetal FixS_Tx() ghi trực tiếp vào DDR address,
     * ở đây ghi vào iq_buffer[] rồi push qua IIO.
     *
     * Format: iq_buffer[0]=I0, iq_buffer[1]=Q0,
     *         iq_buffer[2]=I1, iq_buffer[3]=Q1, ...
     */

    /* Placeholder: tạo tone đơn giản để test */
    for (int i = 0; i < TX_DATA_SIZE; i++) {
        iq_buffer[2 * i + 0] = melp_data[i % MELP_DATA_SIZE]; /* I */
        iq_buffer[2 * i + 1] = 0;                              /* Q */
    }
}

/* ================================================================
 * IIO helper: tìm device và setup buffer
 * ================================================================ */
static struct iio_device *find_tx_device(struct iio_context *ctx)
{
    struct iio_device *dev;
    int dev_count = iio_context_get_devices_count(ctx);

    for (int i = 0; i < dev_count; i++) {
        dev = iio_context_get_device(ctx, i);
        const char *name = iio_device_get_name(dev);
        if (name && strcmp(name, IIO_DEVICE_NAME) == 0)
            return dev;
    }
    return NULL;
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(void)
{
    struct iio_context *ctx;
    struct iio_device *tx_dev;
    struct iio_channel *ch_i, *ch_q;
    struct iio_buffer *iio_buf;
    struct shared_region *shm;
    int shm_fd;
    int frame_count = 0;

    /* TX I/Q buffer: interleaved [I0, Q0, I1, Q1, ...] */
    int16_t iq_buffer[2 * TX_DATA_SIZE];

    signal(SIGINT, signal_handler);

    printf("=== TX FixS Processor (Linux) ===\n\n");

    /* --------------------------------------------------------
     * Bước 1: Mở IIO context
     *
     * Baremetal: ~200 dòng init ADRV9002 (profile parse, SSI,
     *            AGC, axi_adc_init, axi_dac_init, axi_dmac_init)
     * Linux:    Kernel driver đã làm hết, chỉ cần iio_create_local_context()
     * -------------------------------------------------------- */
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

    /* --------------------------------------------------------
     * Bước 2: Enable I/Q channels
     *
     * Baremetal: axi_dac_set_dataset(phy.tx1_dac, -1, AXI_DAC_DATA_SEL_DMA)
     * Linux:    iio_channel_enable(ch_i), iio_channel_enable(ch_q)
     * -------------------------------------------------------- */
    ch_i = iio_device_find_channel(tx_dev, "voltage0", true);  /* I channel */
    ch_q = iio_device_find_channel(tx_dev, "voltage1", true);  /* Q channel */

    if (!ch_i || !ch_q) {
        fprintf(stderr, "Cannot find I/Q channels\n");
        iio_context_destroy(ctx);
        return 1;
    }

    iio_channel_enable(ch_i);
    iio_channel_enable(ch_q);

    printf("[OK] I/Q channels enabled\n");

    /* --------------------------------------------------------
     * Bước 3: Tạo IIO buffer
     *
     * Baremetal: DMA transfer struct + ping-pong buffer trong DDR
     * Linux:    iio_device_create_buffer() — IIO quản lý DMA bên dưới
     *
     * TX_DATA_SIZE samples, non-cyclic (push từng frame)
     * -------------------------------------------------------- */
    iio_buf = iio_device_create_buffer(tx_dev, TX_DATA_SIZE, false);
    if (!iio_buf) {
        fprintf(stderr, "Cannot create IIO buffer\n");
        iio_context_destroy(ctx);
        return 1;
    }

    printf("[OK] IIO TX buffer created (%d samples)\n", TX_DATA_SIZE);

    /* --------------------------------------------------------
     * Bước 4: Mở shared memory (đọc MELP data từ mic process)
     *
     * Baremetal: READ_IDX = *(volatile uint*)(0xFFFC0000 + 4)
     *            data tại SHARED_DDR_BASEADDR + READ_IDX * ...
     * Linux:    shm_open + mmap (cùng SHM_NAME với mic_melp_linux)
     * -------------------------------------------------------- */
    do {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd < 0) {
            printf("[..] Waiting for mic process to create shared memory...\n");
            sleep(1);
        }
    } while (shm_fd < 0 && running);

    if (!running) goto cleanup;

    ftruncate(shm_fd, sizeof(struct shared_region));  // đảm bảo size đúng

    shm = (struct shared_region *)mmap(NULL, sizeof(struct shared_region),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("[OK] Shared memory opened: /dev/shm%s\n", SHM_NAME);

    /* --------------------------------------------------------
     * Bước 5: Cấu hình TX attenuation
     *
     * Baremetal:
     *   adi_adrv9001_Tx_Attenuation_Set(phy.adrv9001, ADI_CHANNEL_1, 10000);
     *   adi_adrv9001_Tx_Attenuation_Set(phy.adrv9001, ADI_CHANNEL_2, 0);
     *
     * Linux: qua IIO sysfs attribute
     *   echo 10000 > /sys/bus/iio/devices/.../out_voltage0_hardwaregain
     *   hoặc dùng iio_device_attr_write()
     * -------------------------------------------------------- */
    /* TX1 attenuation = 10000 mdB (loopback) */
    iio_channel_attr_write(ch_i, "hardwaregain", "-10.000");

    printf("[OK] TX attenuation configured\n");

    /* --------------------------------------------------------
     * Bước 6: Đồng bộ với mic process
     *
     * Baremetal:
     *   TX_READY = 1;
     *   while(!(TX_READY && MIC_READY)) {};
     *   while(!START_FLAG) {};
     *
     * Linux: đợi mic process set start_flag
     * -------------------------------------------------------- */
    shm->tx_ready = 1;
    printf("[..] Waiting for mic process (start_flag)...\n");

    while (running && !shm->start_flag) {
        usleep(1000);
    }
    if (!running) goto cleanup;
    printf("[OK] Sync done, starting TX\n\n");

    /* Delay tương đương no_os_mdelay(400) trong baremetal */
    usleep(400000);

    /* --------------------------------------------------------
     * Bước 7: Main loop
     *
     * Baremetal:
     *   FixS_Tx(tx_buffer_addr[ping]) → DMA transfer → wait
     *   (ping-pong swap)
     *
     * Linux:
     *   Đọc MELP từ shared memory → FixS_Tx_process() → push IIO buffer
     *   Không cần ping-pong vì IIO quản lý buffer bên dưới
     * -------------------------------------------------------- */
    while (running) {

        /* Kiểm tra có data mới trong ring buffer không */
        if (shm->read_idx == shm->write_idx) {
            /* Ring buffer rỗng — chờ mic process ghi thêm */
            usleep(100);
            continue;
        }

        /* Đọc MELP data và modulate — tương đương FixS_Tx() baremetal */
        FixS_Tx_process(shm->data[shm->read_idx], iq_buffer);

        /* Tăng read_idx — tương đương baremetal (baremetal không tăng
         * vì FixS_Tx đọc trực tiếp, nhưng trên Linux cần quản lý rõ) */
        __sync_synchronize();
        shm->read_idx = (shm->read_idx + 1) % BUFFER_DEPTH;

        /* --------------------------------------------------------
         * Push I/Q data vào IIO buffer → DMA → DAC → RF
         *
         * Baremetal:
         *   Xil_DCacheFlushRange(buffer, size)
         *   axi_dmac_transfer_start(phy.tx1_dmac, &transfer_tx)
         *   axi_dmac_transfer_wait_completion(phy.tx1_dmac, 50000)
         *
         * Linux:
         *   memcpy vào IIO buffer → iio_buffer_push() (blocking)
         *   IIO framework tự handle DMA + cache
         * -------------------------------------------------------- */
        void *buf_start = iio_buffer_start(iio_buf);
        memcpy(buf_start, iq_buffer, 2 * TX_DATA_SIZE * sizeof(int16_t));

        ssize_t nbytes = iio_buffer_push(iio_buf);
        if (nbytes < 0) {
            fprintf(stderr, "iio_buffer_push error: %zd\n", nbytes);
            continue;
        }

        frame_count++;
        if (frame_count % 100 == 0) {
            printf("  TX %d frames (read=%d, write=%d)\n",
                   frame_count, shm->read_idx, shm->write_idx);
        }
    }

    /* --------------------------------------------------------
     * Cleanup
     *
     * Baremetal: không có (chạy mãi)
     * Linux:    giải phóng tài nguyên
     * -------------------------------------------------------- */
cleanup:
    printf("\n[OK] Transmitted %d frames total\n", frame_count);

    iio_channel_disable(ch_i);
    iio_channel_disable(ch_q);
    iio_buffer_destroy(iio_buf);
    iio_context_destroy(ctx);

    munmap(shm, sizeof(struct shared_region));
    close(shm_fd);
    /* Không shm_unlink vì mic process sở hữu shared memory */

    printf("[OK] Cleanup done\n");
    return 0;
}