#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <alsa/asoundlib.h>
#include <signal.h>

#include "math.h"

#define MIC_DEVICE "hw:0,0"
#define AUD_DATA_SIZE   180      /* samples per frame */
#define BUFFER_DEPTH    128
#define MELP_DATA_SIZE    54
#define SHM_NAME        "/melp_ringbuf"

struct shared_region {
    volatile int write_idx;
    volatile int read_idx;
    volatile int start_flag;
    volatile int tx_ready;
    volatile int mic_ready;
    //int16_t     data[BUFFER_DEPTH][MELP_DATA_SIZE];
    int16_t*    data[BUFFER_DEPTH];
};


/* ================================================================
 * Global
 * ================================================================ */
static volatile int running = 1;

void signal_handler(int sig)
{
    (void)sig;
    running = 0;
    printf("\nStopping...\n");
}

int main()
{
    snd_pcm_t *pcm;
    int shm_fd;
    struct shared_region *shm;
    int16_t pcm_buffer[AUD_DATA_SIZE];
    int err;
    int frame_count = 0;

    signal(SIGINT, signal_handler);

    printf("=== Mic MELP Processor (Linux) ===\n\n");
    err = snd_pcm_open(&pcm, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio: %s\n", snd_strerror(err));
        return 1;
    }

    /* Cấu hình: 16-bit, mono, 8000 Hz (chuẩn cho MELP) */
    snd_pcm_set_params(pcm,
        SND_PCM_FORMAT_S16_LE,       /* 16-bit signed */
        SND_PCM_ACCESS_RW_INTERLEAVED,
        1,                            /* mono */
        8000,                         /* 8 kHz sample rate */
        1,                            /* allow resampling */
        AUD_DATA_SIZE * 1000 / 8      /* latency in us (1 frame = 22.5ms) */
    );

    printf("[OK] ALSA capture opened (8kHz, 16-bit, mono)\n");
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }
    ftruncate(shm_fd, sizeof(struct shared_region));

    shm = (struct shared_region *)mmap(NULL, sizeof(struct shared_region),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Khởi tạo — tương đương baremetal WRITE_IDX = 0 */
    shm->write_idx = 0;
    shm->read_idx = 0;
    shm->start_flag = 0;
    shm->mic_ready = 1;

    printf("[OK] Shared memory created: /dev/shm%s\n", SHM_NAME);
    printf("[OK] Ring buffer: %d slots x %d bytes\n\n", BUFFER_DEPTH, MELP_DATA_SIZE);
    printf("[OK] Processing started. Ctrl+C to stop.\n\n");
    while (running) {

        /* Đọc 180 samples từ mic (blocking — giống DMA wait) */
        err = snd_pcm_readi(pcm, pcm_buffer, AUD_DATA_SIZE);
        if (err < 0) {
            /* ALSA auto-recovery */
            snd_pcm_recover(pcm, err, 0);
            continue;
        }
        if (err != AUD_DATA_SIZE) continue;

        /* Kiểm tra buffer đầy (giống baremetal không có,
         * nhưng trên Linux nên có để tránh overwrite) */
        int next_write = (shm->write_idx + 1) % BUFFER_DEPTH;
        if (next_write == shm->read_idx) {
            /* Buffer đầy — bỏ frame này (hoặc chờ) */
            continue;
        }

        /* Xử lý MELP — giống hệt baremetal */
        shm->data[shm->write_idx] = pcm_buffer;
        __sync_synchronize(); /* Đảm bảo data đã ghi xong trước khi update write_idx */
        /* Tăng write_idx — giống baremetal */
        shm->write_idx = next_write;

        frame_count++;
        if (frame_count % 100 == 0) {
            printf("  Processed %d frames (write=%d, read=%d)\n",
                   frame_count, shm->write_idx, shm->read_idx);
        }
    }

    /* --------------------------------------------------------
     * Cleanup
     * -------------------------------------------------------- */
    printf("\n[OK] Processed %d frames total\n", frame_count);

    snd_pcm_close(pcm);
    munmap(shm, sizeof(struct shared_region));
    close(shm_fd);
    shm_unlink(SHM_NAME);

    printf("[OK] Cleanup done\n");
    return 0;
}