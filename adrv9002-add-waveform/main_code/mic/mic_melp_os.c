#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <alsa/asoundlib.h>
#include <signal.h>

#include "../melp/melpe.h"
#include "../melp/global.h"
#include "../melp/dsp_sub.h"
#include "../melp/melp_sub.h"
#include "../melp/constant.h"
#include "../melp/math_lib.h"
#include "math.h"
#include "shared_region.h"

#define MIC_DEVICE "hw:0,0"
#define AUD_DATA_SIZE   180      /* samples per frame */

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

static void mic_process(int16_t *input_data, uint16_t *output_data)
{
    DATA voice_in[AUD_DATA_SIZE];
    for(int i = 0; i< AUD_DATA_SIZE; i++) {
        voice_in[i] = (DATA)input_data[i];
    }
    analysis(voice_in, (ushort*)output_data);
}

int main()
{
    snd_pcm_t *pcm;
    int shm_fd;
    struct shared_region *shm;
    int err;
    int frame_count = 0;

    signal(SIGINT, signal_handler);

    printf("=== Mic MELP Processor (Linux) ===\n\n");

    /* --------------------------------------------------------
     * Bước 1: Mở ALSA capture device
     *
     * Baremetal: Xil_Out32(I2S_BASEADDR, ...) + DMA config
     * Linux:    1 dòng snd_pcm_open()
     * -------------------------------------------------------- */
    err = snd_pcm_open(&pcm, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio: %s\n", snd_strerror(err));
        return 1;
    }

    /* Cấu hình: 16-bit, mono, 8000 Hz (chuẩn cho MELP) */
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);

    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw_params, 2);

    unsigned int rate = 8000;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, 0);

    err = snd_pcm_hw_params(pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "Cannot set hw params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }
    printf("[OK] ALSA capture opened (%u Hz, S32_LE, stereo)\n", rate);

    int32_t pcm_buffer_raw[AUD_DATA_SIZE * 2];
    int16_t pcm_buffer[AUD_DATA_SIZE];
    /* --------------------------------------------------------
     * Bước 2: Tạo shared memory ring buffer
     *
     * Baremetal: WRITE_IDX = *(volatile uint*)(0xFFFC0000)
     * Linux:    shm_open + mmap
     * -------------------------------------------------------- */
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

    /* --------------------------------------------------------
     * Bước 3: Main loop — giống baremetal while(1)
     *
     * Baremetal:
     *   axi_dmac_transfer_wait_completion()  → chờ DMA
     *   mic_process(ping_buffer, shared[WRITE_IDX])
     *   WRITE_IDX = (WRITE_IDX + 1) % BUFFER_DEPTH
     *
     * Linux:
     *   snd_pcm_readi()  → chờ ALSA (thay DMA)
     *   mic_process(pcm_buffer, shared[write_idx])
     *   write_idx = (write_idx + 1) % BUFFER_DEPTH
     *
     * Không cần ping-pong buffer vì ALSA đã buffer sẵn.
     * -------------------------------------------------------- */
    printf("[OK] Processing started. Ctrl+C to stop.\n\n");
    melp_ana_init();
    shm->start_flag = 1;
    printf("[OK] MELP analysis initialized\n");
    while (running) {

        /* Đọc 180 samples từ mic (blocking — giống DMA wait) */
        err = snd_pcm_readi(pcm, pcm_buffer_raw, AUD_DATA_SIZE);
        if (err < 0) {
            snd_pcm_recover(pcm, err, 0);
            continue;
        }
        if (err != AUD_DATA_SIZE) continue;

        /* Convert S32_LE stereo → S16 mono (lấy channel trái, shift 16 bit) */
        for (int i = 0; i < AUD_DATA_SIZE; i++) {
            pcm_buffer[i] = (int16_t)(pcm_buffer_raw[2 * i] >> 16);
        }

        /* Kiểm tra buffer đầy (giống baremetal không có,
         * nhưng trên Linux nên có để tránh overwrite) */
        int next_write = (shm->write_idx + 1) % BUFFER_DEPTH;
        if (next_write == shm->read_idx) {
            /* Buffer đầy — bỏ frame này (hoặc chờ) */
            continue;
        }

        /* Xử lý MELP — giống hệt baremetal */
        mic_process(pcm_buffer, shm->data[shm->write_idx]);
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